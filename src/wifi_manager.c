/**
 * =============================================================
 * ESP32 WiFi Extender — WiFi Manager Implementation (ESP-IDF)
 * =============================================================
 * Dual-mode WiFi (AP + STA) management with auto-reconnect,
 * network scanning, and NVS configuration persistence.
 * =============================================================
 */

#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "esp_netif_net_stack.h"

static const char *TAG = "WiFi";

// Forward declarations
bool wifi_manager_is_mac_blacklisted(const uint8_t *mac);

// Event group bits
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

// ---- Module State ----
static wifi_profile_t s_profiles[MAX_SAVED_NETWORKS] = {0};
static char s_sta_ssid[MAX_SSID_LEN + 1] = "";
static char s_sta_password[MAX_PASS_LEN + 1] = "";
static char s_ap_ssid[MAX_SSID_LEN + 1] = DEFAULT_AP_SSID;
static char s_ap_password[MAX_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;
static uint8_t s_ap_channel = DEFAULT_AP_CHANNEL;

static wifi_ext_status_t s_sta_status = EXT_IDLE;
static bool s_sta_configured = false;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;

// ---- Traffic Monitor & Access Control ----
static portMUX_TYPE s_traffic_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_blacklist_mux = portMUX_INITIALIZER_UNLOCKED;

static err_t (*s_orig_sta_input)(struct pbuf *p, struct netif *inp) = NULL;
static err_t (*s_orig_ap_input)(struct pbuf *p, struct netif *inp) = NULL;
static err_t (*s_orig_sta_linkoutput)(struct netif *netif, struct pbuf *p) = NULL;
static err_t (*s_orig_ap_linkoutput)(struct netif *netif, struct pbuf *p) = NULL;

static uint64_t s_sta_rx_bytes = 0;
static uint64_t s_sta_tx_bytes = 0;
static uint64_t s_ap_rx_bytes = 0;
static uint64_t s_ap_tx_bytes = 0;

static uint32_t s_sta_rx_bps = 0;
static uint32_t s_sta_tx_bps = 0;
static uint32_t s_ap_rx_bps = 0;
static uint32_t s_ap_tx_bps = 0;

static uint64_t s_last_sta_rx = 0;
static uint64_t s_last_sta_tx = 0;
static uint64_t s_last_ap_rx = 0;
static uint64_t s_last_ap_tx = 0;

static blacklist_entry_t s_blacklist[MAX_BLACKLIST_MACS] = {0};
static int s_blacklist_count = 0;

typedef struct {
    uint8_t mac[6];
    char hostname[32];
} client_name_entry_t;

#define CLIENT_CACHE_SIZE 20
static client_name_entry_t s_client_names[CLIENT_CACHE_SIZE] = {0};

static err_t custom_sta_input(struct pbuf *p, struct netif *inp) {
    if (p != NULL) {
        portENTER_CRITICAL(&s_traffic_mux);
        s_sta_rx_bytes += p->tot_len;
        portEXIT_CRITICAL(&s_traffic_mux);
    }
    if (s_orig_sta_input) {
        return s_orig_sta_input(p, inp);
    }
    return ERR_OK;
}

static err_t custom_ap_input(struct pbuf *p, struct netif *inp) {
    if (p != NULL) {
        if (p->len >= 12) {
            uint8_t *src_mac = (uint8_t *)p->payload + 6;
            if (wifi_manager_is_mac_blacklisted(src_mac)) {
                pbuf_free(p);
                return ERR_OK; // Drop incoming packet
            }
        }

        portENTER_CRITICAL(&s_traffic_mux);
        s_ap_rx_bytes += p->tot_len;
        portEXIT_CRITICAL(&s_traffic_mux);
    }
    if (s_orig_ap_input) {
        return s_orig_ap_input(p, inp);
    }
    return ERR_OK;
}

static err_t custom_sta_linkoutput(struct netif *netif, struct pbuf *p) {
    if (p != NULL) {
        portENTER_CRITICAL(&s_traffic_mux);
        s_sta_tx_bytes += p->tot_len;
        portEXIT_CRITICAL(&s_traffic_mux);
    }
    if (s_orig_sta_linkoutput) {
        return s_orig_sta_linkoutput(netif, p);
    }
    return ERR_OK;
}

static err_t custom_ap_linkoutput(struct netif *netif, struct pbuf *p) {
    if (p != NULL) {
        if (p->len >= 6) {
            uint8_t *dest_mac = (uint8_t *)p->payload;
            if (wifi_manager_is_mac_blacklisted(dest_mac)) {
                return ERR_OK; // Drop outgoing packet (do not pass to driver)
            }
        }

        portENTER_CRITICAL(&s_traffic_mux);
        s_ap_tx_bytes += p->tot_len;
        portEXIT_CRITICAL(&s_traffic_mux);
    }
    if (s_orig_ap_linkoutput) {
        return s_orig_ap_linkoutput(netif, p);
    }
    return ERR_OK;
}

static void wifi_manager_traffic_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        struct netif *sta_nif = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
        if (sta_nif && sta_nif->input != custom_sta_input) {
            s_orig_sta_input = sta_nif->input;
            s_orig_sta_linkoutput = sta_nif->linkoutput;
            sta_nif->input = custom_sta_input;
            sta_nif->linkoutput = custom_sta_linkoutput;
            ESP_LOGI(TAG, "Traffic monitoring hooked on STA interface dynamically");
        }

        struct netif *ap_nif = (struct netif *)esp_netif_get_netif_impl(s_ap_netif);
        if (ap_nif && ap_nif->input != custom_ap_input) {
            s_orig_ap_input = ap_nif->input;
            s_orig_ap_linkoutput = ap_nif->linkoutput;
            ap_nif->input = custom_ap_input;
            ap_nif->linkoutput = custom_ap_linkoutput;
            ESP_LOGI(TAG, "Traffic monitoring hooked on AP interface dynamically");
        }

        portENTER_CRITICAL(&s_traffic_mux);
        s_sta_rx_bps = (uint32_t)((s_sta_rx_bytes - s_last_sta_rx) * 8);
        s_sta_tx_bps = (uint32_t)((s_sta_tx_bytes - s_last_sta_tx) * 8);
        s_ap_rx_bps = (uint32_t)((s_ap_rx_bytes - s_last_ap_rx) * 8);
        s_ap_tx_bps = (uint32_t)((s_ap_tx_bytes - s_last_ap_tx) * 8);

        s_last_sta_rx = s_sta_rx_bytes;
        s_last_sta_tx = s_sta_tx_bytes;
        s_last_ap_rx = s_ap_rx_bytes;
        s_last_ap_tx = s_ap_tx_bytes;
        portEXIT_CRITICAL(&s_traffic_mux);

        // Periodically deauthenticate any connected blacklisted clients
        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            for (int i = 0; i < sta_list.num; i++) {
                if (wifi_manager_is_mac_blacklisted(sta_list.sta[i].mac)) {
                    uint16_t aid = 0;
                    if (esp_wifi_ap_get_sta_aid(sta_list.sta[i].mac, &aid) == ESP_OK) {
                        esp_wifi_deauth_sta(aid);
                        ESP_LOGW(TAG, "Periodic check: Deauthenticated blacklisted client " MACSTR " (AID: %d)",
                                 MAC2STR(sta_list.sta[i].mac), aid);
                    }
                }
            }
        }
    }
}

void wifi_manager_get_traffic_stats(wifi_traffic_stats_t *out_stats)
{
    portENTER_CRITICAL(&s_traffic_mux);
    out_stats->sta_rx_bytes = s_sta_rx_bytes;
    out_stats->sta_tx_bytes = s_sta_tx_bytes;
    out_stats->ap_rx_bytes = s_ap_rx_bytes;
    out_stats->ap_tx_bytes = s_ap_tx_bytes;
    out_stats->sta_rx_bps = s_sta_rx_bps;
    out_stats->sta_tx_bps = s_sta_tx_bps;
    out_stats->ap_rx_bps = s_ap_rx_bps;
    out_stats->ap_tx_bps = s_ap_tx_bps;
    portEXIT_CRITICAL(&s_traffic_mux);
}

static void save_blacklist_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for blacklist: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, "blacklist", s_blacklist, sizeof(s_blacklist));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save blacklist blob: %s", esp_err_to_name(err));
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Blacklist saved to NVS.");
}

static void load_blacklist_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace for blacklist not found, using empty.");
        memset(s_blacklist, 0, sizeof(s_blacklist));
        return;
    }
    size_t size = sizeof(s_blacklist);
    err = nvs_get_blob(handle, "blacklist", s_blacklist, &size);
    if (err != ESP_OK || size != sizeof(s_blacklist)) {
        ESP_LOGW(TAG, "Blacklist blob not found or size mismatch, resetting.");
        memset(s_blacklist, 0, sizeof(s_blacklist));
    }
    nvs_close(handle);

    s_blacklist_count = 0;
    for (int i = 0; i < MAX_BLACKLIST_MACS; i++) {
        bool empty = true;
        for (int j = 0; j < 6; j++) {
            if (s_blacklist[i].mac[j] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            s_blacklist_count++;
        }
    }
    ESP_LOGI(TAG, "Loaded %d blacklisted MACs from NVS.", s_blacklist_count);
}

int wifi_manager_get_blacklist(blacklist_entry_t *out_list, int max_entries)
{
    int count = 0;
    portENTER_CRITICAL(&s_blacklist_mux);
    for (int i = 0; i < MAX_BLACKLIST_MACS && count < max_entries; i++) {
        bool empty = true;
        for (int j = 0; j < 6; j++) {
            if (s_blacklist[i].mac[j] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            memcpy(&out_list[count], &s_blacklist[i], sizeof(blacklist_entry_t));
            count++;
        }
    }
    portEXIT_CRITICAL(&s_blacklist_mux);
    return count;
}

bool wifi_manager_is_mac_blacklisted(const uint8_t *mac)
{
    if (mac == NULL) return false;
    bool blocked = false;
    portENTER_CRITICAL(&s_blacklist_mux);
    for (int i = 0; i < MAX_BLACKLIST_MACS; i++) {
        if (memcmp(s_blacklist[i].mac, mac, 6) == 0) {
            blocked = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_blacklist_mux);
    return blocked;
}

static void deauth_client_by_mac(const uint8_t *mac)
{
    uint16_t aid = 0;
    if (esp_wifi_ap_get_sta_aid(mac, &aid) == ESP_OK) {
        esp_wifi_deauth_sta(aid);
        ESP_LOGI(TAG, "Deauthenticated blacklisted client " MACSTR " (AID: %d)",
                 MAC2STR(mac), aid);
    }
}

void wifi_manager_get_client_hostname(const uint8_t *mac, char *out_name, size_t max_len)
{
    out_name[0] = '\0';
    portENTER_CRITICAL(&s_blacklist_mux);
    for (int i = 0; i < CLIENT_CACHE_SIZE; i++) {
        if (memcmp(s_client_names[i].mac, mac, 6) == 0) {
            strncpy(out_name, s_client_names[i].hostname, max_len - 1);
            out_name[max_len - 1] = '\0';
            break;
        }
    }
    portEXIT_CRITICAL(&s_blacklist_mux);
    if (out_name[0] == '\0') {
        strncpy(out_name, "Unknown Device", max_len - 1);
        out_name[max_len - 1] = '\0';
    }
}

bool wifi_manager_add_blacklist(const uint8_t *mac, const char *name)
{
    if (mac == NULL) return false;

    if (wifi_manager_is_mac_blacklisted(mac)) {
        return true; 
    }

    bool success = false;
    portENTER_CRITICAL(&s_blacklist_mux);
    for (int i = 0; i < MAX_BLACKLIST_MACS; i++) {
        bool empty = true;
        for (int j = 0; j < 6; j++) {
            if (s_blacklist[i].mac[j] != 0) {
                empty = false;
                break;
            }
        }
        if (empty) {
            memcpy(s_blacklist[i].mac, mac, 6);
            if (name) {
                strncpy(s_blacklist[i].name, name, sizeof(s_blacklist[i].name) - 1);
                s_blacklist[i].name[sizeof(s_blacklist[i].name) - 1] = '\0';
            } else {
                s_blacklist[i].name[0] = '\0';
            }
            s_blacklist_count++;
            success = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_blacklist_mux);

    if (success) {
        save_blacklist_to_nvs();
        deauth_client_by_mac(mac);
    }

    return success;
}

bool wifi_manager_delete_blacklist(const uint8_t *mac)
{
    if (mac == NULL) return false;

    bool found = false;
    portENTER_CRITICAL(&s_blacklist_mux);
    for (int i = 0; i < MAX_BLACKLIST_MACS; i++) {
        if (memcmp(s_blacklist[i].mac, mac, 6) == 0) {
            memset(&s_blacklist[i], 0, sizeof(blacklist_entry_t));
            s_blacklist_count--;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_blacklist_mux);

    if (found) {
        save_blacklist_to_nvs();
    }
    return found;
}

// ---- Forward declarations ----
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void setup_ap(void);

// ---- WiFi Event Handler ----
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected.");
                s_sta_status = EXT_DISCONNECTED;
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Client connected to AP - MAC: " MACSTR, MAC2STR(event->mac));
                if (wifi_manager_is_mac_blacklisted(event->mac)) {
                    ESP_LOGW(TAG, "Blacklisted client MAC " MACSTR " tried to connect. Deauthenticating...", MAC2STR(event->mac));
                    esp_wifi_deauth_sta(event->aid);
                }
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Client disconnected from AP - MAC: " MACSTR, MAC2STR(event->mac));
                // Clear from hostname cache
                portENTER_CRITICAL(&s_blacklist_mux);
                for (int i = 0; i < CLIENT_CACHE_SIZE; i++) {
                    if (memcmp(s_client_names[i].mac, event->mac, 6) == 0) {
                        memset(&s_client_names[i], 0, sizeof(client_name_entry_t));
                        break;
                    }
                }
                portEXIT_CRITICAL(&s_blacklist_mux);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_sta_status = EXT_CONNECTED;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        } else if (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            ip_event_assigned_ip_to_client_t *event = (ip_event_assigned_ip_to_client_t *)event_data;
            // Update client cache
            portENTER_CRITICAL(&s_blacklist_mux);
            bool updated = false;
            for (int i = 0; i < CLIENT_CACHE_SIZE; i++) {
                if (memcmp(s_client_names[i].mac, event->mac, 6) == 0) {
                    strncpy(s_client_names[i].hostname, event->hostname, sizeof(s_client_names[i].hostname) - 1);
                    s_client_names[i].hostname[sizeof(s_client_names[i].hostname) - 1] = '\0';
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                for (int i = 0; i < CLIENT_CACHE_SIZE; i++) {
                    bool empty = true;
                    for (int j = 0; j < 6; j++) {
                        if (s_client_names[i].mac[j] != 0) {
                            empty = false;
                            break;
                        }
                    }
                    if (empty) {
                        memcpy(s_client_names[i].mac, event->mac, 6);
                        strncpy(s_client_names[i].hostname, event->hostname, sizeof(s_client_names[i].hostname) - 1);
                        s_client_names[i].hostname[sizeof(s_client_names[i].hostname) - 1] = '\0';
                        break;
                    }
                }
            }
            portEXIT_CRITICAL(&s_blacklist_mux);
            ESP_LOGI(TAG, "DHCP IP assigned to client " MACSTR " -> Hostname: %s",
                     MAC2STR(event->mac), event->hostname[0] != '\0' ? event->hostname : "Unknown");
        }
    }
}

// ---- Initialize WiFi ----
void wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing dual-mode WiFi (AP + STA)...");

    // Load saved configuration from NVS
    wifi_manager_load_config();

    // Create event group for connection synchronization
    s_wifi_event_group = xEventGroupCreate();

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default WiFi interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &wifi_event_handler, NULL, NULL));

    // Set mode to AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure and start AP
    setup_ap();

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // ---- Speed & Range Optimizations ----
    ESP_LOGI(TAG, "Applying performance optimizations...");
    // 1. Maximize transmit power to 20 dBm (80 * 0.25 dBm) for maximum range
    esp_wifi_set_max_tx_power(80); 
    // 2. Disable power saving to reduce latency and maximize throughput
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Hooking is now done dynamically in the traffic monitoring task once netifs are ready.

    // Spawn traffic monitoring task
    xTaskCreate(wifi_manager_traffic_task, "wifi_traffic", 2048, NULL, 4, NULL);

    // Load blacklist from NVS
    load_blacklist_from_nvs();

    // If profiles are configured, scan and connect to the best one
    bool any_profiles = false;
    for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
        if (strlen(s_profiles[i].ssid) > 0) {
            any_profiles = true;
            break;
        }
    }

    if (any_profiles) {
        s_sta_configured = true;
        wifi_manager_connect_best_profile();
    } else {
        ESP_LOGI(TAG, "No STA credentials configured.");
        ESP_LOGI(TAG, "Connect to AP and configure via web portal.");
        s_sta_status = EXT_IDLE;
    }
}

// ---- Connect to Home WiFi (STA) ----
bool wifi_manager_connect_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Error: SSID is empty!");
        return false;
    }

    strncpy(s_sta_ssid, ssid, MAX_SSID_LEN);
    s_sta_ssid[MAX_SSID_LEN] = '\0';
    if (password != NULL && strlen(password) > 0) {
        strncpy(s_sta_password, password, MAX_PASS_LEN);
        s_sta_password[MAX_PASS_LEN] = '\0';
    } else {
        bool found_saved = false;
        for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
            if (strcmp(s_profiles[i].ssid, ssid) == 0) {
                strncpy(s_sta_password, s_profiles[i].password, MAX_PASS_LEN);
                s_sta_password[MAX_PASS_LEN] = '\0';
                found_saved = true;
                ESP_LOGI(TAG, "Found saved password for SSID: %s", ssid);
                break;
            }
        }
        if (!found_saved) {
            s_sta_password[0] = '\0';
        }
    }
    s_sta_configured = true;
    s_sta_status = EXT_CONNECTING;

    ESP_LOGI(TAG, "Connecting to: %s", s_sta_ssid);

    // Disconnect if already connected
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configure STA
    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid, s_sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, s_sta_password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = (strlen(s_sta_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // Clear event bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Start connection
    esp_wifi_connect();

    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        s_sta_status = EXT_CONNECTED;

        // Log connection info
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
        ESP_LOGI(TAG, "Connected to home WiFi!");
        ESP_LOGI(TAG, "  IP Address : " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "  Gateway    : " IPSTR, IP2STR(&ip_info.gw));

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "  RSSI       : %d dBm", ap_info.rssi);
            ESP_LOGI(TAG, "  Channel    : %d", ap_info.primary);

            // Sync AP channel to match STA for best performance
            if (s_ap_channel != ap_info.primary) {
                ESP_LOGI(TAG, "Syncing AP channel to %d (matching STA)", ap_info.primary);
                s_ap_channel = ap_info.primary;
                setup_ap();
            }
        }

        // Save successful credentials
        wifi_manager_save_sta_config(s_sta_ssid, s_sta_password);
        return true;
    } else {
        s_sta_status = EXT_CONNECT_FAILED;
        ESP_LOGE(TAG, "Failed to connect to home WiFi!");
        return false;
    }
}

void wifi_manager_disconnect_sta(void)
{
    esp_wifi_disconnect();
    s_sta_status = EXT_DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected from home WiFi.");
}

bool wifi_manager_is_sta_connected(void)
{
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}

wifi_ext_status_t wifi_manager_get_sta_status(void)
{
    if (wifi_manager_is_sta_connected()) {
        s_sta_status = EXT_CONNECTED;
    } else if (s_sta_status == EXT_CONNECTED) {
        s_sta_status = EXT_DISCONNECTED;
    }
    return s_sta_status;
}

// ---- Access Point Setup ----
static void setup_ap(void)
{
    ESP_LOGI(TAG, "Starting Access Point: %s", s_ap_ssid);

    wifi_config_t ap_config = {};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap_config.ap.password, s_ap_password, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = s_ap_channel;
    ap_config.ap.max_connection = DEFAULT_AP_MAX_CONN;
    ap_config.ap.ssid_hidden = 0;

    if (strlen(s_ap_password) >= 8) {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ap_config.ap.password[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "  AP SSID    : %s", s_ap_ssid);
    ESP_LOGI(TAG, "  AP Channel : %d", s_ap_channel);
    ESP_LOGI(TAG, "  Max Clients: %d", DEFAULT_AP_MAX_CONN);
}

void wifi_manager_start_ap(void)
{
    setup_ap();
}

void wifi_manager_update_ap_config(const char *ssid, const char *password, uint8_t channel)
{
    if (ssid != NULL && strlen(ssid) > 0) {
        strncpy(s_ap_ssid, ssid, MAX_SSID_LEN);
        s_ap_ssid[MAX_SSID_LEN] = '\0';
    } else {
        strncpy(s_ap_ssid, DEFAULT_AP_SSID, MAX_SSID_LEN);
    }

    if (password != NULL && strlen(password) >= 8) {
        strncpy(s_ap_password, password, MAX_PASS_LEN);
        s_ap_password[MAX_PASS_LEN] = '\0';
    } else {
        strncpy(s_ap_password, DEFAULT_AP_PASSWORD, MAX_PASS_LEN);
    }

    if (channel > 0 && channel <= 13) {
        s_ap_channel = channel;
    } else {
        s_ap_channel = DEFAULT_AP_CHANNEL;
    }

    wifi_manager_save_ap_config(s_ap_ssid, s_ap_password, s_ap_channel);
    setup_ap();
    ESP_LOGI(TAG, "AP configuration updated and restarted.");
}

uint8_t wifi_manager_get_connected_clients(void)
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

// ---- Network Scanning ----
int wifi_manager_scan_networks(scanned_network_t *results, int max_results)
{
    ESP_LOGI(TAG, "Scanning for networks...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } }
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true); // blocking scan
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t num_found = 0;
    esp_wifi_scan_get_ap_num(&num_found);

    uint16_t to_get = (num_found < max_results) ? num_found : max_results;
    wifi_ap_record_t *ap_records = calloc(to_get, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Scan: out of memory");
        esp_wifi_scan_get_ap_records(&to_get, NULL); // clear results
        return 0;
    }

    esp_wifi_scan_get_ap_records(&to_get, ap_records);

    for (int i = 0; i < to_get; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = ap_records[i].rssi;
        results[i].enc_type = ap_records[i].authmode;
        results[i].channel = ap_records[i].primary;
    }

    free(ap_records);
    ESP_LOGI(TAG, "Found %d networks.", num_found);
    return to_get;
}

// ---- NVS Persistence ----
static void save_profiles_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, "saved_profiles", s_profiles, sizeof(s_profiles));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save profiles blob: %s", esp_err_to_name(err));
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Profiles saved to NVS.");
}

void wifi_manager_save_sta_config(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_STA_SSID, ssid);
        nvs_set_str(handle, NVS_KEY_STA_PASS, password);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Legacy STA credentials saved to NVS.");
    }
    wifi_manager_save_profile(ssid, password);
}

void wifi_manager_save_ap_config(const char *ssid, const char *password, uint8_t channel)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_str(handle, NVS_KEY_AP_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_AP_PASS, password);
    nvs_set_u8(handle, NVS_KEY_AP_CHANNEL, channel);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "AP config saved to NVS.");
}

void wifi_manager_load_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults.");
        return;
    }

    size_t len;

    // Load profiles blob
    size_t blob_size = sizeof(s_profiles);
    err = nvs_get_blob(handle, "saved_profiles", s_profiles, &blob_size);
    bool migrated = false;

    if (err != ESP_OK || blob_size != sizeof(s_profiles)) {
        ESP_LOGW(TAG, "Profiles blob not found or size mismatch, trying migration...");
        memset(s_profiles, 0, sizeof(s_profiles));

        // Attempt migration from legacy single config keys
        char legacy_ssid[MAX_SSID_LEN + 1] = "";
        char legacy_pass[MAX_PASS_LEN + 1] = "";
        len = sizeof(legacy_ssid);
        esp_err_t err_ssid = nvs_get_str(handle, NVS_KEY_STA_SSID, legacy_ssid, &len);
        len = sizeof(legacy_pass);
        esp_err_t err_pass = nvs_get_str(handle, NVS_KEY_STA_PASS, legacy_pass, &len);

        if (err_ssid == ESP_OK && strlen(legacy_ssid) > 0) {
            strncpy(s_profiles[0].ssid, legacy_ssid, sizeof(s_profiles[0].ssid) - 1);
            if (err_pass == ESP_OK) {
                strncpy(s_profiles[0].password, legacy_pass, sizeof(s_profiles[0].password) - 1);
            }
            migrated = true;
            ESP_LOGI(TAG, "Migrated legacy WiFi credentials to profile index 0: %s", legacy_ssid);
        }
    }

    // Now set current active s_sta_ssid and s_sta_password to the first valid profile
    s_sta_ssid[0] = '\0';
    s_sta_password[0] = '\0';
    for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
        if (strlen(s_profiles[i].ssid) > 0) {
            strncpy(s_sta_ssid, s_profiles[i].ssid, MAX_SSID_LEN);
            strncpy(s_sta_password, s_profiles[i].password, MAX_PASS_LEN);
            break;
        }
    }

    // Load AP configurations
    len = sizeof(s_ap_ssid);
    if (nvs_get_str(handle, NVS_KEY_AP_SSID, s_ap_ssid, &len) != ESP_OK) {
        strncpy(s_ap_ssid, DEFAULT_AP_SSID, MAX_SSID_LEN);
    }

    len = sizeof(s_ap_password);
    if (nvs_get_str(handle, NVS_KEY_AP_PASS, s_ap_password, &len) != ESP_OK) {
        strncpy(s_ap_password, DEFAULT_AP_PASSWORD, MAX_PASS_LEN);
    }

    if (nvs_get_u8(handle, NVS_KEY_AP_CHANNEL, &s_ap_channel) != ESP_OK) {
        s_ap_channel = DEFAULT_AP_CHANNEL;
    }

    if (migrated) {
        nvs_set_blob(handle, "saved_profiles", s_profiles, sizeof(s_profiles));
        nvs_commit(handle);
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Configuration loaded from NVS:");
    ESP_LOGI(TAG, "  STA SSID : %s", strlen(s_sta_ssid) > 0 ? s_sta_ssid : "(not configured)");
    ESP_LOGI(TAG, "  AP SSID  : %s", s_ap_ssid);
}

// Profile management implementations
int wifi_manager_get_profiles(wifi_profile_t *out_profiles, int max_profiles)
{
    int count = 0;
    for (int i = 0; i < MAX_SAVED_NETWORKS && count < max_profiles; i++) {
        if (strlen(s_profiles[i].ssid) > 0) {
            memcpy(&out_profiles[count], &s_profiles[i], sizeof(wifi_profile_t));
            count++;
        }
    }
    return count;
}

bool wifi_manager_save_profile(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) return false;

    // Check if profile already exists
    int empty_slot = -1;
    for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) {
            strncpy(s_profiles[i].password, password ? password : "", sizeof(s_profiles[i].password) - 1);
            s_profiles[i].password[sizeof(s_profiles[i].password) - 1] = '\0';
            save_profiles_to_nvs();
            return true;
        }
        if (empty_slot == -1 && strlen(s_profiles[i].ssid) == 0) {
            empty_slot = i;
        }
    }

    if (empty_slot != -1) {
        strncpy(s_profiles[empty_slot].ssid, ssid, sizeof(s_profiles[empty_slot].ssid) - 1);
        s_profiles[empty_slot].ssid[sizeof(s_profiles[empty_slot].ssid) - 1] = '\0';
        strncpy(s_profiles[empty_slot].password, password ? password : "", sizeof(s_profiles[empty_slot].password) - 1);
        s_profiles[empty_slot].password[sizeof(s_profiles[empty_slot].password) - 1] = '\0';
        save_profiles_to_nvs();
        return true;
    }

    ESP_LOGW(TAG, "Cannot save profile. Profiles list is full!");
    return false;
}

bool wifi_manager_delete_profile(const char *ssid)
{
    if (ssid == NULL || strlen(ssid) == 0) return false;

    for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) {
            memset(&s_profiles[i], 0, sizeof(wifi_profile_t));
            save_profiles_to_nvs();
            ESP_LOGI(TAG, "Profile deleted: %s", ssid);
            return true;
        }
    }
    return false;
}

bool wifi_manager_connect_best_profile(void)
{
    ESP_LOGI(TAG, "Starting scan to find best saved profile...");
    
    bool any_profiles = false;
    for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
        if (strlen(s_profiles[i].ssid) > 0) {
            any_profiles = true;
            break;
        }
    }
    if (!any_profiles) {
        ESP_LOGW(TAG, "No saved profiles to scan and connect.");
        return false;
    }

    scanned_network_t scanned[20];
    int count = wifi_manager_scan_networks(scanned, 20);
    
    int best_profile_index = -1;
    int8_t best_rssi = -127;
    
    for (int i = 0; i < count; i++) {
        for (int p = 0; p < MAX_SAVED_NETWORKS; p++) {
            if (strlen(s_profiles[p].ssid) > 0 && strcmp(s_profiles[p].ssid, scanned[i].ssid) == 0) {
                if (scanned[i].rssi > best_rssi) {
                    best_rssi = scanned[i].rssi;
                    best_profile_index = p;
                }
            }
        }
    }
    
    if (best_profile_index != -1) {
        ESP_LOGI(TAG, "Best saved profile found: SSID='%s', RSSI=%d dBm", 
                 s_profiles[best_profile_index].ssid, best_rssi);
        return wifi_manager_connect_sta(s_profiles[best_profile_index].ssid, 
                                       s_profiles[best_profile_index].password);
    }
    
    ESP_LOGW(TAG, "No saved profiles visible in the scan. Trying connection to first valid profile as fallback.");
    for (int p = 0; p < MAX_SAVED_NETWORKS; p++) {
        if (strlen(s_profiles[p].ssid) > 0) {
            return wifi_manager_connect_sta(s_profiles[p].ssid, s_profiles[p].password);
        }
    }
    
    return false;
}

void wifi_manager_factory_reset(void)
{
    ESP_LOGW(TAG, "*** FACTORY RESET ***");
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGW(TAG, "All settings cleared. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ---- Getters ----
const char* wifi_manager_get_sta_ssid(void)    { return s_sta_ssid; }
const char* wifi_manager_get_ap_ssid(void)     { return s_ap_ssid; }
const char* wifi_manager_get_ap_password(void) { return s_ap_password; }
uint8_t wifi_manager_get_ap_channel(void)      { return s_ap_channel; }

void wifi_manager_get_sta_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip_info;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(buf, "N/A", len);
    }
}

void wifi_manager_get_ap_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip_info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(buf, "192.168.4.1", len);
    }
}

int8_t wifi_manager_get_sta_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void wifi_manager_get_sta_mac_str(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wifi_manager_get_ap_mac_str(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_netif_t* wifi_manager_get_sta_netif(void) { return s_sta_netif; }
esp_netif_t* wifi_manager_get_ap_netif(void)  { return s_ap_netif; }

// ---- Auto-Reconnect Task ----
void wifi_manager_reconnect_task(void *pvParameters)
{
    int consecutive_failures = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));

        if (!s_sta_configured) continue;
        if (wifi_manager_is_sta_connected()) {
            consecutive_failures = 0;
            continue;
        }

        ESP_LOGW(TAG, "Connection lost. Attempting reconnect...");
        s_sta_status = EXT_CONNECTING;

        if (consecutive_failures >= 3) {
            ESP_LOGI(TAG, "Consecutive failures >= 3. Scanning for better saved profiles...");
            if (wifi_manager_connect_best_profile()) {
                consecutive_failures = 0;
                continue;
            }
        }

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_connect();

        // Wait a bit to see if it connects
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(5000));

        if (bits & WIFI_CONNECTED_BIT) {
            s_sta_status = EXT_CONNECTED;
            ESP_LOGI(TAG, "Reconnected successfully!");
            consecutive_failures = 0;
        } else {
            s_sta_status = EXT_DISCONNECTED;
            ESP_LOGW(TAG, "Reconnect failed. Will retry...");
            consecutive_failures++;
        }
    }
}
