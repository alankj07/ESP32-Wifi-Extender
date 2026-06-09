/**
 * =============================================================
 * ESP32 WiFi Extender — NAPT Router Header (ESP-IDF)
 * =============================================================
 * Handles Network Address Port Translation (NAPT) to route
 * traffic from AP-connected clients through the STA interface.
 * =============================================================
 */

#ifndef NAPT_ROUTER_H
#define NAPT_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NAPT routing (call after STA is connected and AP is started)
bool napt_router_start(void);

// Stop NAPT routing
void napt_router_stop(void);

// Check if NAPT is active
bool napt_router_is_active(void);

// Get NAPT table size
uint32_t napt_router_get_table_size(void);

#ifdef __cplusplus
}
#endif

#endif // NAPT_ROUTER_H
