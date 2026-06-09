/**
 * =============================================================
 * ESP32 WiFi Extender — Web Portal Implementation (ESP-IDF)
 * =============================================================
 * HTTP server using esp_http_server for configuration dashboard,
 * network scanning, WiFi connection, and device management.
 * =============================================================
 */

#include "web_portal.h"
#include "wifi_manager.h"
#include "napt_router.h"
#include "status_monitor.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "Web";
static httpd_handle_t s_server = NULL;

// Helper: Check if request is authenticated
static bool is_authorized(httpd_req_t *req)
{
    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Extender Admin\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }

    char *auth_header = malloc(header_len + 1);
    if (!auth_header) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, header_len + 1) != ESP_OK) {
        free(auth_header);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }

    if (strncmp(auth_header, "Basic ", 6) != 0) {
        free(auth_header);
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Extender Admin\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }

    char *encoded = auth_header + 6;
    size_t encoded_len = strlen(encoded);

    unsigned char decoded[128];
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char *)encoded, encoded_len);
    free(auth_header);

    if (ret != 0) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Extender Admin\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Extender Admin\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }

    *colon = '\0';
    char *username = (char *)decoded;
    char *password = colon + 1;

    const char *expected_user = "admin";
    const char *expected_pass = wifi_manager_get_ap_password();

    if (strcmp(username, expected_user) == 0 && strcmp(password, expected_pass) == 0) {
        return true;
    }

    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Extender Admin\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return false;
}

// Wrapper for all authenticated URI handlers
static esp_err_t auth_wrapper(httpd_req_t *req)
{
    if (!is_authorized(req)) {
        return ESP_OK;
    }
    esp_err_t (*real_handler)(httpd_req_t *) = (esp_err_t (*)(httpd_req_t *))req->user_ctx;
    if (real_handler) {
        return real_handler(req);
    }
    return ESP_FAIL;
}


/* Embedded HTML page (GZIP compressed, from embed_txtfiles in platformio.ini) */
extern const uint8_t index_html_gz_txt_start[] asm("_binary_index_html_gz_txt_start");
extern const uint8_t index_html_gz_txt_end[]   asm("_binary_index_html_gz_txt_end");

/* ---- Route: Main Dashboard (GET /) ---- */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    
    size_t len = index_html_gz_txt_end - index_html_gz_txt_start;
    if (len > 0) {
        len--; // Exclude the null-terminator byte appended by EMBED_TXTFILES
    }
    httpd_resp_send(req, (const char *)index_html_gz_txt_start, len);
    return ESP_OK;
}

/* ---- Route: Scan Networks (GET /scan) ---- */
static esp_err_t scan_handler(httpd_req_t *req)
{
    scanned_network_t networks[20];
    int count = wifi_manager_scan_networks(networks, 20);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(net, "enc", networks[i].enc_type);
        cJSON_AddNumberToObject(net, "ch", networks[i].channel);
        cJSON_AddItemToArray(arr, net);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ---- Helper: Read POST body ---- */
static int read_post_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)max_len) {
        return -1;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        received += ret;
    }
    buf[received] = '\0';
    return received;
}

/* ---- Helper: URL-decode a string in-place ---- */
static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ---- Helper: Extract form field from URL-encoded body ---- */
static bool get_form_field(const char *body, const char *key, char *out, size_t out_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "%s=", key);

    const char *start = strstr(body, search_key);
    if (!start) {
        out[0] = '\0';
        return false;
    }
    start += strlen(search_key);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    char temp[256];
    if (len >= sizeof(temp)) {
        len = sizeof(temp) - 1;
    }
    strncpy(temp, start, len);
    temp[len] = '\0';

    url_decode(temp);

    strncpy(out, temp, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

/* ---- Route: Connect to WiFi (POST /connect) ---- */
static esp_err_t connect_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char ssid[MAX_SSID_LEN + 1];
    char password[MAX_PASS_LEN + 1];
    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "password", password, sizeof(password));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID is required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Connect request: SSID=%s", ssid);

    bool connected = wifi_manager_connect_sta(ssid, password);

    if (connected) {
        napt_router_start();

        char ip_str[16];
        wifi_manager_get_sta_ip_str(ip_str, sizeof(ip_str));

        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"success\":true,\"message\":\"Connected!\",\"ip\":\"%s\"}", ip_str);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Failed to connect. Check password.\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ---- Route: Update AP Config (POST /ap-config) ---- */
static esp_err_t ap_config_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char ssid[MAX_SSID_LEN + 1];
    char password[MAX_PASS_LEN + 1];
    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "password", password, sizeof(password));

    if (strlen(password) > 0 && strlen(password) < 8) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Password must be at least 8 characters\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "AP config update: SSID=%s", ssid);

    wifi_manager_update_ap_config(
        strlen(ssid) > 0 ? ssid : DEFAULT_AP_SSID,
        strlen(password) >= 8 ? password : wifi_manager_get_ap_password(),
        wifi_manager_get_ap_channel()
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Hotspot configuration updated\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Route: Status JSON (GET /status) ---- */
static esp_err_t status_handler(httpd_req_t *req)
{
    bool sta_connected = wifi_manager_is_sta_connected();

    char sta_ip[16], ap_ip[16], sta_mac[18], ap_mac[18], uptime[32];
    wifi_manager_get_sta_ip_str(sta_ip, sizeof(sta_ip));
    wifi_manager_get_ap_ip_str(ap_ip, sizeof(ap_ip));
    wifi_manager_get_sta_mac_str(sta_mac, sizeof(sta_mac));
    wifi_manager_get_ap_mac_str(ap_mac, sizeof(ap_mac));
    status_monitor_get_uptime_formatted(uptime, sizeof(uptime));

    // Placement helper logic
    int placement_status = 0; // 0=disconnected, 1=too_close, 2=optimal, 3=too_far
    const char *placement_msg = "Extender is not connected to home WiFi.";
    if (sta_connected) {
        int8_t rssi = status_monitor_get_average_rssi();
        if (rssi >= -55) {
            placement_status = 1;
            placement_msg = "Extender is too close to your router. Move it further away to increase your overall extended range.";
        } else if (rssi >= -75) {
            placement_status = 2;
            placement_msg = "Perfect placement! The extender has an optimal connection and expands range efficiently.";
        } else {
            placement_status = 3;
            placement_msg = "Extender is too far from your router. Move it closer to avoid slow speeds or connection drops.";
        }
    }

    // Get traffic stats
    wifi_traffic_stats_t stats;
    wifi_manager_get_traffic_stats(&stats);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", wifi_manager_get_sta_ssid());
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddStringToObject(root, "sta_mac", sta_mac);
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_ip", ap_ip);
    cJSON_AddStringToObject(root, "ap_mac", ap_mac);
    cJSON_AddNumberToObject(root, "rssi", sta_connected ? status_monitor_get_average_rssi() : 0);
    cJSON_AddNumberToObject(root, "clients", wifi_manager_get_connected_clients());
    cJSON_AddNumberToObject(root, "channel", wifi_manager_get_ap_channel());
    cJSON_AddStringToObject(root, "uptime", uptime);
    cJSON_AddNumberToObject(root, "free_heap", status_monitor_get_free_heap());
    cJSON_AddNumberToObject(root, "min_heap", status_monitor_get_min_free_heap());
    cJSON_AddBoolToObject(root, "napt_active", napt_router_is_active());
    cJSON_AddNumberToObject(root, "napt_table", napt_router_get_table_size());

    // Add traffic stats
    cJSON_AddNumberToObject(root, "sta_rx_bytes", (double)stats.sta_rx_bytes);
    cJSON_AddNumberToObject(root, "sta_tx_bytes", (double)stats.sta_tx_bytes);
    cJSON_AddNumberToObject(root, "ap_rx_bytes", (double)stats.ap_rx_bytes);
    cJSON_AddNumberToObject(root, "ap_tx_bytes", (double)stats.ap_tx_bytes);
    cJSON_AddNumberToObject(root, "sta_rx_bps", stats.sta_rx_bps);
    cJSON_AddNumberToObject(root, "sta_tx_bps", stats.sta_tx_bps);
    cJSON_AddNumberToObject(root, "ap_rx_bps", stats.ap_rx_bps);
    cJSON_AddNumberToObject(root, "ap_tx_bps", stats.ap_tx_bps);

    // Add placement info
    cJSON_AddNumberToObject(root, "placement_status", placement_status);
    cJSON_AddStringToObject(root, "placement_msg", placement_msg);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---- Route: Get Connected Clients (GET /clients) ---- */
static esp_err_t clients_handler(httpd_req_t *req)
{
    wifi_sta_list_t sta_list;
    wifi_sta_mac_ip_list_t sta_ip_list;

    cJSON *root = cJSON_CreateObject();
    cJSON *clients = cJSON_AddArrayToObject(root, "clients");

    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        if (esp_wifi_ap_get_sta_list_with_ip(&sta_list, &sta_ip_list) == ESP_OK) {
            for (int i = 0; i < sta_ip_list.num; i++) {
                esp_netif_pair_mac_ip_t info = sta_ip_list.sta[i];
                cJSON *client = cJSON_CreateObject();

                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         info.mac[0], info.mac[1], info.mac[2],
                         info.mac[3], info.mac[4], info.mac[5]);

                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&info.ip));

                char name_str[32];
                wifi_manager_get_client_hostname(info.mac, name_str, sizeof(name_str));
                cJSON_AddStringToObject(client, "name", name_str);

                cJSON_AddStringToObject(client, "mac", mac_str);
                cJSON_AddStringToObject(client, "ip", ip_str);
                cJSON_AddItemToArray(clients, client);
            }
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---- Route: Restart Device (POST /restart) ---- */
static esp_err_t restart_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Restarting...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---- Route: Factory Reset (POST /reset) ---- */
static esp_err_t reset_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Factory reset...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_factory_reset(); /* This calls esp_restart() internally */
    return ESP_OK;
}

/* ---- Route: Get Saved Profiles (GET /profiles) ---- */
static esp_err_t profiles_get_handler(httpd_req_t *req)
{
    wifi_profile_t profiles[MAX_SAVED_NETWORKS];
    int count = wifi_manager_get_profiles(profiles, MAX_SAVED_NETWORKS);

    cJSON *arr = cJSON_CreateArray();
    const char *current_ssid = wifi_manager_get_sta_ssid();
    bool is_connected = wifi_manager_is_sta_connected();

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", profiles[i].ssid);
        
        bool current = (is_connected && strcmp(profiles[i].ssid, current_ssid) == 0);
        cJSON_AddBoolToObject(item, "connected", current);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ---- Route: Add Profile (POST /profiles/add) ---- */
static esp_err_t profiles_add_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char ssid[MAX_SSID_LEN + 1] = "";
    char password[MAX_PASS_LEN + 1] = "";
    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "password", password, sizeof(password));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID is required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool success = wifi_manager_save_profile(ssid, password);
    char resp[128];
    if (success) {
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"Profile saved\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"Profiles list is full (max %d)\"}", MAX_SAVED_NETWORKS);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Route: Delete Profile (POST /profiles/delete) ---- */
static esp_err_t profiles_delete_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char ssid[MAX_SSID_LEN + 1] = "";
    get_form_field(body, "ssid", ssid, sizeof(ssid));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID is required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool success = wifi_manager_delete_profile(ssid);
    char resp[128];
    if (success) {
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"Profile deleted\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"Profile not found\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Route: Connect Best Profile (POST /profiles/connect-best) ---- */
static esp_err_t profiles_connect_best_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Connect to best network triggered from web portal");
    bool success = wifi_manager_connect_best_profile();
    char resp[128];
    if (success) {
        char ip_str[16];
        wifi_manager_get_sta_ip_str(ip_str, sizeof(ip_str));
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"Connected!\",\"ip\":\"%s\"}", ip_str);
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"Failed to connect to any saved network.\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Route: Get Blacklist (GET /blacklist) ---- */
static esp_err_t blacklist_get_handler(httpd_req_t *req)
{
    blacklist_entry_t list[MAX_BLACKLIST_MACS];
    int count = wifi_manager_get_blacklist(list, MAX_BLACKLIST_MACS);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 list[i].mac[0], list[i].mac[1], list[i].mac[2],
                 list[i].mac[3], list[i].mac[4], list[i].mac[5]);
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddStringToObject(item, "name", list[i].name);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ---- Route: Add Blacklist (POST /blacklist/add) ---- */
static esp_err_t blacklist_add_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char mac_str[18] = "";
    char name[32] = "";
    get_form_field(body, "mac", mac_str, sizeof(mac_str));
    get_form_field(body, "name", name, sizeof(name));

    if (strlen(mac_str) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"MAC address is required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint8_t mac[6];
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6 &&
        sscanf(mac_str, "%x-%x-%x-%x-%x-%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Invalid MAC address format\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }

    bool success = wifi_manager_add_blacklist(mac, name);
    char resp[128];
    if (success) {
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"Device blocked successfully\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"Blocklist is full (max %d)\"}", MAX_BLACKLIST_MACS);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Route: Delete Blacklist (POST /blacklist/delete) ---- */
static esp_err_t blacklist_delete_handler(httpd_req_t *req)
{
    char body[256];
    if (read_post_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char mac_str[18] = "";
    get_form_field(body, "mac", mac_str, sizeof(mac_str));

    if (strlen(mac_str) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"MAC address is required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint8_t mac[6];
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6 &&
        sscanf(mac_str, "%x-%x-%x-%x-%x-%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Invalid MAC address format\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }

    bool success = wifi_manager_delete_blacklist(mac);
    char resp[128];
    if (success) {
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"Device unblocked successfully\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"Device not found in blocklist\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Start Web Server ---- */
void web_portal_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running.");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return;
    }

    /* Register URI handlers */
    const httpd_uri_t uri_root         = { .uri = "/",                   .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = root_handler };
    const httpd_uri_t uri_scan         = { .uri = "/scan",               .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = scan_handler };
    const httpd_uri_t uri_connect      = { .uri = "/connect",            .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = connect_handler };
    const httpd_uri_t uri_apconf       = { .uri = "/ap-config",          .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = ap_config_handler };
    const httpd_uri_t uri_status       = { .uri = "/status",             .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = status_handler };
    const httpd_uri_t uri_clients      = { .uri = "/clients",            .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = clients_handler };
    const httpd_uri_t uri_restart      = { .uri = "/restart",            .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = restart_handler };
    const httpd_uri_t uri_reset        = { .uri = "/reset",              .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = reset_handler };
    const httpd_uri_t uri_prof_get     = { .uri = "/profiles",           .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = profiles_get_handler };
    const httpd_uri_t uri_prof_add     = { .uri = "/profiles/add",       .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = profiles_add_handler };
    const httpd_uri_t uri_prof_del     = { .uri = "/profiles/delete",    .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = profiles_delete_handler };
    const httpd_uri_t uri_prof_best    = { .uri = "/profiles/connect-best", .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = profiles_connect_best_handler };
    const httpd_uri_t uri_blk_get      = { .uri = "/blacklist",          .method = HTTP_GET,  .handler = auth_wrapper, .user_ctx = blacklist_get_handler };
    const httpd_uri_t uri_blk_add      = { .uri = "/blacklist/add",      .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = blacklist_add_handler };
    const httpd_uri_t uri_blk_del      = { .uri = "/blacklist/delete",   .method = HTTP_POST, .handler = auth_wrapper, .user_ctx = blacklist_delete_handler };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_connect);
    httpd_register_uri_handler(s_server, &uri_apconf);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_clients);
    httpd_register_uri_handler(s_server, &uri_restart);
    httpd_register_uri_handler(s_server, &uri_reset);
    httpd_register_uri_handler(s_server, &uri_prof_get);
    httpd_register_uri_handler(s_server, &uri_prof_add);
    httpd_register_uri_handler(s_server, &uri_prof_del);
    httpd_register_uri_handler(s_server, &uri_prof_best);
    httpd_register_uri_handler(s_server, &uri_blk_get);
    httpd_register_uri_handler(s_server, &uri_blk_add);
    httpd_register_uri_handler(s_server, &uri_blk_del);

    ESP_LOGI(TAG, "Portal running at http://192.168.4.1");
}

/* ---- Stop Web Server ---- */
void web_portal_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Portal stopped.");
    }
}
