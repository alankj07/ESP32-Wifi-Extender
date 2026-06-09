/**
 * =============================================================
 * ESP32 WiFi Extender — NAPT Router Implementation (ESP-IDF)
 * =============================================================
 * Enables Network Address Port Translation (NAPT) to forward
 * traffic from AP-connected clients through the STA interface
 * to the internet. This is the core "repeater" functionality.
 *
 * NAPT works by:
 * 1. Intercepting outgoing packets from AP clients (192.168.4.x)
 * 2. Rewriting source IP/port to the STA interface IP
 * 3. Forwarding to the home router
 * 4. Rewriting incoming response packets back to the AP client
 * =============================================================
 */

#include "napt_router.h"
#include "wifi_manager.h"
#include "config.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "NAPT";

static bool s_active = false;
static uint32_t s_table_size = NAPT_TABLE_SIZE;

bool napt_router_start(void)
{
    ESP_LOGI(TAG, "Initializing NAT routing...");

    // Verify STA is connected (we need an upstream connection)
    if (!wifi_manager_is_sta_connected()) {
        ESP_LOGW(TAG, "Warning: STA not connected. NAPT will activate but");
        ESP_LOGW(TAG, "         won't route traffic until STA connects.");
    }

    // ---- Step 1: Enable NAPT on the AP (softAP) interface ----
    // Get the AP interface IP to identify it for NAPT
    esp_netif_t *ap_netif = wifi_manager_get_ap_netif();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "AP netif not available!");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP IP info: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "AP IP for NAPT: " IPSTR, IP2STR(&ip_info.ip));

    // Enable NAPT on the AP interface using its IP address
    ip_napt_enable(ip_info.ip.addr, 1);
    ESP_LOGI(TAG, "NAPT enabled on AP interface");

    // ---- Step 2: Configure DNS for AP clients ----
    // Set the DHCP server on the AP to serve DNS to connected clients.
    // Without this, clients can connect but can't resolve domain names.
    ESP_LOGI(TAG, "Configuring DNS forwarding...");

    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = 0;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;

    // Try to use the upstream DNS from STA connection
    esp_netif_t *sta_netif = wifi_manager_get_sta_netif();
    if (sta_netif != NULL &&
        esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.u_addr.ip4.addr != 0) {
        ESP_LOGI(TAG, "  Using upstream DNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
    } else {
        // Fallback to Google DNS
        IP4_ADDR(&dns_info.ip.u_addr.ip4, 8, 8, 8, 8);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_LOGI(TAG, "  Using fallback DNS: 8.8.8.8");
    }

    // Set the DNS server on the AP's DHCP server
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_stop(ap_netif);
    esp_err_t dns_opt_err = esp_netif_dhcps_option(
        ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER,
        &dhcps_dns_value,
        sizeof(dhcps_dns_value)
    );
    if (dns_opt_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DHCP DNS option: %s", esp_err_to_name(dns_opt_err));
    }
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    esp_netif_dhcps_start(ap_netif);

    s_active = true;
    ESP_LOGI(TAG, "NAT routing is ACTIVE");
    ESP_LOGI(TAG, "  AP clients can now access the internet");
    ESP_LOGI(TAG, "  through the home WiFi connection.");

    return true;
}

void napt_router_stop(void)
{
    if (!s_active) return;

    // Disable NAPT on the AP interface
    esp_netif_t *ap_netif = wifi_manager_get_ap_netif();
    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            ip_napt_enable(ip_info.ip.addr, 0);
        }
    }

    s_active = false;
    ESP_LOGI(TAG, "NAT routing stopped.");
}

bool napt_router_is_active(void)
{
    return s_active;
}

uint32_t napt_router_get_table_size(void)
{
    return s_table_size;
}
