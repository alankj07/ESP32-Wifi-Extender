/**
 * =============================================================
 * ESP32 WiFi Extender — Configuration Defaults
 * =============================================================
 * All configurable parameters with sensible defaults.
 * These values are used on first boot; after that, the web
 * portal saves user settings to NVS (non-volatile storage).
 * =============================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

// ---- Access Point (Hotspot) Defaults ----
#define DEFAULT_AP_SSID       "ESP32_Extender"
#define DEFAULT_AP_PASSWORD   "ext12345678"
#define DEFAULT_AP_CHANNEL    1
#define DEFAULT_AP_MAX_CONN   8          // Max simultaneous clients

// ---- Station (Home WiFi) Defaults ----
// Leave empty to force configuration via web portal
#define DEFAULT_STA_SSID      ""
#define DEFAULT_STA_PASSWORD  ""

// ---- NAPT Configuration ----
#define NAPT_TABLE_SIZE       512        // NAT translation table entries
#define NAPT_TCP_TIMEOUT      300        // TCP NAT entry timeout (seconds)
#define NAPT_UDP_TIMEOUT      30         // UDP NAT entry timeout (seconds)

// ---- Web Portal ----
#define WEB_SERVER_PORT       80
#define STATUS_UPDATE_MS      5000       // Status JSON refresh interval

// ---- WiFi Connection ----
#define WIFI_CONNECT_TIMEOUT_MS  30000   // 30 seconds to connect
#define WIFI_RETRY_DELAY_MS      500     // ms between connection attempts
#define WIFI_RECONNECT_DELAY_MS  10000   // 10 seconds before auto-reconnect

// ---- Status LED ----
#define STATUS_LED_PIN        2          // Built-in LED on most ESP32 boards
#define LED_BLINK_SLOW_MS     1000       // Slow blink interval (connecting)
#define LED_BLINK_FAST_MS     250        // Fast blink interval (active)

// ---- NVS Storage ----
#define NVS_NAMESPACE         "wifiext"
#define NVS_KEY_STA_SSID      "sta_ssid"
#define NVS_KEY_STA_PASS      "sta_pass"
#define NVS_KEY_AP_SSID       "ap_ssid"
#define NVS_KEY_AP_PASS       "ap_pass"
#define NVS_KEY_AP_CHANNEL    "ap_chan"

// ---- Max string lengths ----
#define MAX_SSID_LEN          32
#define MAX_PASS_LEN          64

// ---- Debug ----
#define SERIAL_BAUD           115200

// ---- Version ----
#define FIRMWARE_VERSION      "2.0.0"
#define DEVICE_NAME           "ESP32 WiFi Extender"

#endif // CONFIG_H
