/**
 * =============================================================
 * ESP32 WiFi Extender — Web Portal Header (ESP-IDF)
 * =============================================================
 * HTTP server handling configuration dashboard,
 * network scanning, WiFi connection, and device management.
 * =============================================================
 */

#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#ifdef __cplusplus
extern "C" {
#endif

// Start the web portal HTTP server
void web_portal_start(void);

// Stop the web portal
void web_portal_stop(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_PORTAL_H
