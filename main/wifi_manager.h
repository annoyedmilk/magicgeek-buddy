#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// WiFi provisioning. BLE-to-Claude is the buddy's primary link; WiFi is
// secondary (OTA, future networked features). Flow:
//   boot → creds in NVS?  yes → STA connect (background)
//                          no  → SoftAP "Buddy-XXYY" + captive portal
// The portal page also has a "Forget WiFi" button that wipes creds and
// reboots back into setup. Ported from the proven F1 Dashboard firmware.

/**
 * Initialize the WiFi/netif/event subsystem.
 * Call storage_init_nvs() first.
 */
esp_err_t wifi_manager_init(void);

typedef enum {
    WIFI_STATE_CONNECTING,   // creds present, STA bring-up in progress
    WIFI_STATE_ONLINE,       // joined an AP, got an IP
    WIFI_STATE_PORTAL,       // no creds / connect failed → captive portal
} wifi_state_t;

/**
 * Connect using saved NVS credentials. ESP_OK if connected within
 * timeout_ms, ESP_FAIL if no creds or connection failed. BLOCKING -
 * do not call from app_main (watchdog). Use wifi_manager_autostart().
 */
esp_err_t wifi_manager_connect(int timeout_ms);

/**
 * Non-blocking boot entry point. Creds present → background STA connect
 * (falls back to the captive portal on failure); no creds → portal now.
 * Returns immediately so the render loop is never starved.
 */
void wifi_manager_autostart(void);

/**
 * Coarse connection state for the UI.
 */
wifi_state_t wifi_manager_state(void);

/**
 * True if NVS holds a non-empty SSID.
 */
bool wifi_manager_has_creds(void);

/**
 * Start the SoftAP captive portal ("Buddy-XXYY", 192.168.4.1) and the
 * HTTP config server. Spawns a background task and returns immediately
 * so the BLE/render loop is never blocked. On credential submit the
 * device saves them and reboots; "Forget WiFi" wipes creds and reboots.
 */
esp_err_t wifi_manager_start_portal(void);

/**
 * Erase saved WiFi credentials (SSID + password) from NVS.
 */
esp_err_t wifi_manager_erase_creds(void);

bool   wifi_manager_is_connected(void);
int8_t wifi_manager_get_rssi(void);
void   wifi_manager_get_ip_str(char *out, size_t len);   // "0.0.0.0" if down
void   wifi_manager_get_mac_str(char *out, size_t len);
// SoftAP SSID ("Buddy-XXYY"). Valid after wifi_manager_start_portal();
// derived from the SoftAP MAC so it's stable to call any time.
void   wifi_manager_get_ap_name(char *out, size_t len);

// Shared HTTP server handle - used by ota_server.c and any future module
// that wants to register routes (e.g. /ota). NULL until the portal or
// the STA path has started httpd. Modules should call this AFTER WiFi
// reports state == ONLINE / PORTAL and register their URIs.
typedef struct httpd_req httpd_req_t;  // fwd to avoid pulling esp_http_server.h
void *wifi_manager_get_httpd(void);    // returns httpd_handle_t (void *) or NULL
