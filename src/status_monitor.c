/**
 * =============================================================
 * ESP32 WiFi Extender — Status Monitor Implementation (ESP-IDF)
 * =============================================================
 * Tracks device health metrics and drives status LED indicator.
 * =============================================================
 */

#include "status_monitor.h"
#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/gpio.h"

static const char *TAG = "Status";

// ---- Module State ----
static int64_t s_boot_time_us = 0;
static int64_t s_last_led_toggle_us = 0;
static int64_t s_last_status_print_us = 0;
static bool s_led_state = false;
static uint8_t s_led_mode = 0;

// RSSI averaging
static int32_t s_rssi_buffer[10] = {0};
static uint8_t s_rssi_index = 0;
static uint8_t s_rssi_count = 0;

static void update_rssi(void)
{
    int8_t rssi = wifi_manager_get_sta_rssi();
    if (rssi != 0) {
        s_rssi_buffer[s_rssi_index] = rssi;
        s_rssi_index = (s_rssi_index + 1) % 10;
        if (s_rssi_count < 10) s_rssi_count++;
    }
}

void status_monitor_init(void)
{
    s_boot_time_us = esp_timer_get_time();

    // Initialize status LED
    gpio_reset_pin((gpio_num_t)STATUS_LED_PIN);
    gpio_set_direction((gpio_num_t)STATUS_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)STATUS_LED_PIN, 0);

    ESP_LOGI(TAG, "Monitor initialized.");
}

void status_monitor_update(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;
    int64_t last_toggle_ms = s_last_led_toggle_us / 1000;

    // ---- Update RSSI buffer ----
    if (wifi_manager_is_sta_connected()) {
        update_rssi();
    }

    // ---- LED Control ----
    switch (s_led_mode) {
        case 0: // Off
            gpio_set_level((gpio_num_t)STATUS_LED_PIN, 0);
            s_led_state = false;
            break;

        case 1: // Solid ON (not connected, AP only)
            gpio_set_level((gpio_num_t)STATUS_LED_PIN, 1);
            s_led_state = true;
            break;

        case 2: // Slow blink (connecting)
            if (now_ms - last_toggle_ms >= LED_BLINK_SLOW_MS) {
                s_led_state = !s_led_state;
                gpio_set_level((gpio_num_t)STATUS_LED_PIN, s_led_state ? 1 : 0);
                s_last_led_toggle_us = now_us;
            }
            break;

        case 3: // Fast blink (connected & active)
            if (now_ms - last_toggle_ms >= LED_BLINK_FAST_MS) {
                s_led_state = !s_led_state;
                gpio_set_level((gpio_num_t)STATUS_LED_PIN, s_led_state ? 1 : 0);
                s_last_led_toggle_us = now_us;
            }
            break;
    }

    // ---- Periodic serial status dump (every 30 seconds) ----
    int64_t last_print_ms = s_last_status_print_us / 1000;
    if (now_ms - last_print_ms >= 30000) {
        s_last_status_print_us = now_us;
        status_monitor_print_status();
    }
}

// ---- Getters ----

uint32_t status_monitor_get_uptime_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - s_boot_time_us) / 1000000);
}

void status_monitor_get_uptime_formatted(char *buf, size_t len)
{
    uint32_t totalSec = status_monitor_get_uptime_seconds();
    uint32_t days = totalSec / 86400;
    uint32_t hours = (totalSec % 86400) / 3600;
    uint32_t minutes = (totalSec % 3600) / 60;
    uint32_t seconds = totalSec % 60;

    if (days > 0) {
        snprintf(buf, len, "%lud %02lu:%02lu:%02lu",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)minutes, (unsigned long)seconds);
    } else {
        snprintf(buf, len, "%02lu:%02lu:%02lu",
                 (unsigned long)hours, (unsigned long)minutes,
                 (unsigned long)seconds);
    }
}

int32_t status_monitor_get_average_rssi(void)
{
    if (s_rssi_count == 0) return 0;

    int32_t sum = 0;
    for (uint8_t i = 0; i < s_rssi_count; i++) {
        sum += s_rssi_buffer[i];
    }
    return sum / s_rssi_count;
}

uint32_t status_monitor_get_free_heap(void)
{
    return esp_get_free_heap_size();
}

uint32_t status_monitor_get_min_free_heap(void)
{
    return esp_get_minimum_free_heap_size();
}

void status_monitor_set_led_mode(uint8_t mode)
{
    s_led_mode = mode;
}

void status_monitor_print_status(void)
{
    char uptime[32];
    status_monitor_get_uptime_formatted(uptime, sizeof(uptime));

    ESP_LOGI(TAG, "========== ESP32 WiFi Extender Status ==========");
    ESP_LOGI(TAG, "  Uptime     : %s", uptime);
    ESP_LOGI(TAG, "  STA Status : %s",
             wifi_manager_is_sta_connected() ? "Connected" : "Disconnected");

    if (wifi_manager_is_sta_connected()) {
        char ip_str[16];
        wifi_manager_get_sta_ip_str(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "  STA IP     : %s", ip_str);
        ESP_LOGI(TAG, "  RSSI       : %d dBm", status_monitor_get_average_rssi());
    }

    ESP_LOGI(TAG, "  AP Clients : %d", wifi_manager_get_connected_clients());
    ESP_LOGI(TAG, "  Free Heap  : %lu bytes", (unsigned long)status_monitor_get_free_heap());
    ESP_LOGI(TAG, "================================================");
}
