/**
 * =============================================================
 * ESP32 WiFi Extender — WiFi Manager Header (ESP-IDF)
 * =============================================================
 * Manages dual-mode WiFi (AP + STA), connection lifecycle,
 * auto-reconnect, and network scanning using ESP-IDF APIs.
 * =============================================================
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connection status enumeration
typedef enum {
    EXT_IDLE,
    EXT_CONNECTING,
    EXT_CONNECTED,
    EXT_DISCONNECTED,
    EXT_CONNECT_FAILED
} wifi_ext_status_t;

// Scanned network info
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t enc_type;   // wifi_auth_mode_t
    uint8_t channel;
} scanned_network_t;

// Initialize WiFi in AP+STA mode
void wifi_manager_init(void);

// Station (home WiFi) management
bool wifi_manager_connect_sta(const char *ssid, const char *password);
void wifi_manager_disconnect_sta(void);
bool wifi_manager_is_sta_connected(void);
wifi_ext_status_t wifi_manager_get_sta_status(void);

// Access Point (hotspot) management
void wifi_manager_start_ap(void);
void wifi_manager_update_ap_config(const char *ssid, const char *password, uint8_t channel);
uint8_t wifi_manager_get_connected_clients(void);

// Network scanning
int wifi_manager_scan_networks(scanned_network_t *results, int max_results);

// Configuration persistence (NVS)
#define MAX_SAVED_NETWORKS 5

typedef struct {
    char ssid[33];
    char password[65];
} wifi_profile_t;

#define MAX_BLACKLIST_MACS 10

typedef struct {
    uint8_t mac[6];
    char name[32];
} blacklist_entry_t;

typedef struct {
    uint64_t sta_rx_bytes;
    uint64_t sta_tx_bytes;
    uint64_t ap_rx_bytes;
    uint64_t ap_tx_bytes;
    uint32_t sta_rx_bps;
    uint32_t sta_tx_bps;
    uint32_t ap_rx_bps;
    uint32_t ap_tx_bps;
} wifi_traffic_stats_t;

void wifi_manager_save_sta_config(const char *ssid, const char *password);
void wifi_manager_save_ap_config(const char *ssid, const char *password, uint8_t channel);
void wifi_manager_load_config(void);
void wifi_manager_factory_reset(void);

// Profile management
int wifi_manager_get_profiles(wifi_profile_t *out_profiles, int max_profiles);
bool wifi_manager_save_profile(const char *ssid, const char *password);
bool wifi_manager_delete_profile(const char *ssid);
bool wifi_manager_connect_best_profile(void);

// Traffic statistics & Access control (Blacklist)
void wifi_manager_get_traffic_stats(wifi_traffic_stats_t *out_stats);
int wifi_manager_get_blacklist(blacklist_entry_t *out_list, int max_entries);
bool wifi_manager_add_blacklist(const uint8_t *mac, const char *name);
bool wifi_manager_delete_blacklist(const uint8_t *mac);
bool wifi_manager_is_mac_blacklisted(const uint8_t *mac);
void wifi_manager_get_client_hostname(const uint8_t *mac, char *out_name, size_t max_len);

// Getters
const char* wifi_manager_get_sta_ssid(void);
const char* wifi_manager_get_ap_ssid(void);
const char* wifi_manager_get_ap_password(void);
uint8_t wifi_manager_get_ap_channel(void);
void wifi_manager_get_sta_ip_str(char *buf, size_t len);
void wifi_manager_get_ap_ip_str(char *buf, size_t len);
int8_t wifi_manager_get_sta_rssi(void);
void wifi_manager_get_sta_mac_str(char *buf, size_t len);
void wifi_manager_get_ap_mac_str(char *buf, size_t len);

// Auto-reconnect (call periodically or from a task)
void wifi_manager_reconnect_task(void *pvParameters);

// Get the ESP netif handles
esp_netif_t* wifi_manager_get_sta_netif(void);
esp_netif_t* wifi_manager_get_ap_netif(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
