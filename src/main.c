/**
 * =============================================================
 * ESP32 WiFi Extender — Main Entry Point (ESP-IDF)
 * =============================================================
 *
 * Turns your ESP32 into a WiFi range extender that:
 *   1. Connects to your home WiFi (Station mode)
 *   2. Creates its own hotspot (Access Point mode)
 *   3. Routes internet traffic via NAPT between them
 *   4. Provides a web dashboard for configuration
 *
 * First-time setup:
 *   1. Flash this firmware to your ESP32
 *   2. Connect your phone/laptop to "ESP32_Extender" WiFi
 *      (password: ext12345678)
 *   3. Open http://192.168.4.1 in your browser
 *   4. Select your home WiFi and enter the password
 *
 * =============================================================
 */

#include "config.h"
#include "wifi_manager.h"
#include "napt_router.h"
#include "web_portal.h"
#include "status_monitor.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Main";

/* Track NAPT state */
static bool s_napt_started = false;

/**
 * Main monitoring task — runs in a loop checking connection
 * state, starting/stopping NAPT, and updating the status LED.
 */
static void main_task(void *pvParameters)
{
    while (1) {
        /* Start NAPT when STA connects (if not already started) */
        if (!s_napt_started && wifi_manager_is_sta_connected()) {
            ESP_LOGI(TAG, "STA connected! Starting NAPT routing...");
            s_napt_started = napt_router_start();
            if (s_napt_started) {
                ESP_LOGI(TAG, "NAPT routing activated — internet sharing is ON!");
                status_monitor_set_led_mode(3); /* Fast blink = active */
            }
        }

        /* Stop NAPT tracking if STA disconnects */
        if (s_napt_started && !wifi_manager_is_sta_connected()) {
            s_napt_started = false;
            ESP_LOGW(TAG, "STA disconnected. NAPT paused until reconnect.");
            status_monitor_set_led_mode(2); /* Slow blink = waiting */
        }

        /* Update LED based on current state */
        if (wifi_manager_is_sta_connected() && s_napt_started) {
            status_monitor_set_led_mode(3);
        } else if (wifi_manager_get_sta_status() == EXT_CONNECTING) {
            status_monitor_set_led_mode(2);
        } else if (strlen(wifi_manager_get_sta_ssid()) > 0) {
            status_monitor_set_led_mode(2);
        } else {
            status_monitor_set_led_mode(1); /* Solid = AP only, waiting for config */
        }

        /* Update status monitor (LED + periodic serial dump) */
        status_monitor_update();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* ---- Initialize NVS ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- Create default event loop ---- */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 WiFi Extender v%s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "  Dual-Mode WiFi Repeater with NAPT");
    ESP_LOGI(TAG, "========================================");

    /* ---- Initialize Status Monitor ---- */
    status_monitor_init();
    status_monitor_set_led_mode(1); /* Solid = booting */

    /* ---- Initialize WiFi Manager ---- */
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_manager_init();

    /* ---- Start NAPT if STA is already connected ---- */
    if (wifi_manager_is_sta_connected()) {
        ESP_LOGI(TAG, "Initializing NAPT Routing...");
        s_napt_started = napt_router_start();
        if (s_napt_started) {
            status_monitor_set_led_mode(3); /* Fast blink = active */
        }
    } else {
        ESP_LOGI(TAG, "STA not connected — configure via web portal at http://192.168.4.1");
        status_monitor_set_led_mode(2); /* Slow blink = waiting */
    }

    /* ---- Start Web Portal ---- */
    ESP_LOGI(TAG, "Starting Web Portal...");
    web_portal_start();

    /* ---- Start auto-reconnect task ---- */
    xTaskCreate(wifi_manager_reconnect_task, "wifi_reconnect", 4096, NULL, 5, NULL);

    /* ---- Start main monitoring task ---- */
    xTaskCreate(main_task, "main_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 WiFi Extender is READY!");
    ESP_LOGI(TAG, "  Hotspot: %s", wifi_manager_get_ap_ssid());
    ESP_LOGI(TAG, "  Portal:  http://192.168.4.1");
    ESP_LOGI(TAG, "========================================");
}
