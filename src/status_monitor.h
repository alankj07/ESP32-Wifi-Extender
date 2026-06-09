/**
 * =============================================================
 * ESP32 WiFi Extender — Status Monitor Header (ESP-IDF)
 * =============================================================
 * Tracks signal strength, uptime, connected clients, memory,
 * and drives the status LED indicator.
 * =============================================================
 */

#ifndef STATUS_MONITOR_H
#define STATUS_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize LED and counters
void status_monitor_init(void);

// Update LED and stats (call periodically)
void status_monitor_update(void);

// Getters
uint32_t status_monitor_get_uptime_seconds(void);
void status_monitor_get_uptime_formatted(char *buf, size_t len);
int32_t status_monitor_get_average_rssi(void);
uint32_t status_monitor_get_free_heap(void);
uint32_t status_monitor_get_min_free_heap(void);

// LED control: 0=off, 1=solid, 2=slow blink, 3=fast blink
void status_monitor_set_led_mode(uint8_t mode);

// Serial status dump
void status_monitor_print_status(void);

#ifdef __cplusplus
}
#endif

#endif // STATUS_MONITOR_H
