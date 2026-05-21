// WiFi STA + SoftAP captive portal.
//
// Boot flow:
//   - If NVS has saved creds, try STA connect in a background task
//     (so the render loop and BLE host stay live). On success, the
//     shared httpd starts in STA mode.
//   - With no creds (or after a connect timeout), fall back to a
//     SoftAP captive portal named Buddy-XXYY (last MAC bytes). The
//     user enters creds via a Claude-themed HTML page at 192.168.4.1.
//   - The "Forget WiFi" button POSTs to /forget, wipes creds, reboots
//     back into captive-portal mode.
#include "wifi_manager.h"
#include "storage.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "lwip/inet.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"
#define MAX_RETRY           5

static EventGroupHandle_t wifi_events;
static int retry_count;
static bool connected;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static esp_ip4_addr_t sta_ip;

// Shared HTTP server: one httpd hosts the captive portal AND the OTA
// route. It's created lazily by whichever path comes up first
// (portal_task in AP mode, or wifi_connect_task on STA-online). Modules
// that want to register routes call wifi_manager_get_httpd() and add
// their URIs onto it.
static httpd_handle_t shared_httpd = NULL;
static bool           routes_registered = false;
// Forward decl - start_shared_httpd is called from wifi_connect_task
// (defined above start_shared_httpd in this file).
static void start_shared_httpd(bool ap_mode);

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            connected = false;
            if (retry_count < MAX_RETRY) {
                retry_count++;
                ESP_LOGW(TAG, "Retry %d/%d...", retry_count, MAX_RETRY);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        sta_ip = evt->ip_info.ip;
        connected = true;
        retry_count = 0;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "WiFi subsystem initialized");
    return ESP_OK;
}

bool wifi_manager_has_creds(void)
{
    char ssid[33] = {0};
    return storage_get_str(NVS_KEY_SSID, ssid, sizeof(ssid)) == ESP_OK
        && strlen(ssid) > 0;
}

esp_err_t wifi_manager_connect(int timeout_ms)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (storage_get_str(NVS_KEY_SSID, ssid, sizeof(ssid)) != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "No saved WiFi credentials");
        return ESP_FAIL;
    }
    storage_get_str(NVS_KEY_PASS, pass, sizeof(pass));

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    retry_count = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    esp_wifi_stop();
    return ESP_FAIL;
}

// Background WiFi bring-up. app_main must NEVER block on the connect:
// esp_wifi auth/retry can take 20-30s and starves the main task, which
// trips the Task Watchdog (TG1WDT_SYS_RESET) into a boot loop. Instead
// we start STA, return immediately, and let this task watch the result -
// falling back to the captive portal on failure. The render loop keeps
// running (and yielding) the entire time.
static volatile bool sta_giveup = false;

static void wifi_connect_task(void *arg)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    storage_get_str(NVS_KEY_SSID, ssid, sizeof(ssid));
    storage_get_str(NVS_KEY_PASS, pass, sizeof(pass));
    ESP_LOGI(TAG, "Connecting to '%s' (background)...", ssid);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    retry_count = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 30s overall budget. The handler's MAX_RETRY logic sets WIFI_FAIL_BIT
    // when it exhausts retries; we also cap by time in case it stalls.
    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected - portal not needed");
        // Bring up the shared httpd so /ota (and the rest of the
        // /-served pages) are reachable at http://<device-ip>/. In STA
        // mode we DON'T register the captive wildcard catch-all.
        start_shared_httpd(/*ap_mode=*/false);
    } else {
        ESP_LOGW(TAG, "STA connect failed/timed out - starting captive portal");
        sta_giveup = true;
        esp_wifi_stop();
        wifi_manager_start_portal();   // spawns its own task
    }
    vTaskDelete(NULL);
}

// Non-blocking entry point used at boot. Creds present → background
// STA connect (portal fallback on failure). No creds → portal now.
// Returns immediately either way.
void wifi_manager_autostart(void)
{
    if (wifi_manager_has_creds()) {
        xTaskCreate(wifi_connect_task, "wifi_conn", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "No WiFi creds - starting captive portal");
        wifi_manager_start_portal();
    }
}

// Coarse state for the scaffold status screen.
wifi_state_t wifi_manager_state(void)
{
    if (connected)   return WIFI_STATE_ONLINE;
    if (sta_giveup)  return WIFI_STATE_PORTAL;
    return wifi_manager_has_creds() ? WIFI_STATE_CONNECTING : WIFI_STATE_PORTAL;
}

// --- Captive Portal (Claude-night theme) ---
//
// %% escapes are required: this string is passed through code that does
// not printf it, but kept doubled for parity with the F1 source so a
// future printf-based variant stays correct.

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Claude Buddy Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1F1E1D;"
    "color:#F0EEE6;display:flex;justify-content:center;align-items:center;"
    "min-height:100vh;padding:1em}"
    ".card{background:#262625;padding:2em 1.5em;border-radius:16px;"
    "width:100%;max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,.5)}"
    ".logo{text-align:center;margin-bottom:1.5em}"
    ".logo h1{font-size:1.3em;font-weight:700;color:#F0EEE6;margin:0 0 .15em}"
    ".logo p{font-size:.85em;color:#8a8782}"
    ".field{margin-bottom:1.2em}"
    ".field label{display:block;font-size:.8em;font-weight:600;color:#8a8782;"
    "text-transform:uppercase;letter-spacing:.05em;margin-bottom:.4em}"
    ".field input{width:100%;padding:.75em 1em;border:1px solid #3a3936;"
    "border-radius:10px;background:#1F1E1D;color:#F0EEE6;font-size:1em;"
    "outline:none;transition:border .2s}"
    ".field input:focus{border-color:#D97757}"
    ".field input::placeholder{color:#6a6863}"
    ".pw-wrap{position:relative}"
    ".pw-wrap input{padding-right:3.5em}"
    ".pw-toggle{position:absolute;right:.75em;top:50%;transform:translateY(-50%);"
    "background:none;border:none;color:#8a8782;cursor:pointer;font-size:.85em;padding:0}"
    "button[type=submit]{width:100%;padding:.85em;border:none;border-radius:10px;"
    "background:#D97757;color:#1F1E1D;font-size:1em;font-weight:700;cursor:pointer;"
    "transition:filter .2s;margin-top:.5em}"
    "button[type=submit]:active{filter:brightness(.85)}"
    ".forget{margin-top:1.4em;border-top:1px solid #3a3936;padding-top:1.2em;"
    "text-align:center}"
    ".forget button{background:none;border:1px solid #5a3a32;color:#D97757;"
    "padding:.6em 1.2em;border-radius:8px;font-size:.85em;cursor:pointer}"
    ".forget button:active{background:rgba(217,119,87,.1)}"
    ".note{margin-top:1.5em;font-size:.78em;color:#6a6863;text-align:center}"
    ".note a{color:#8a8782}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='logo'>"
    "<h1>Claude Buddy</h1><p>WiFi Setup</p></div>"
    "<form method='POST' action='/save'>"
    "<div class='field'><label>WiFi Network</label>"
    "<input name='ssid' placeholder='Enter SSID' required autocomplete='off'></div>"
    "<div class='field'><label>WiFi Password</label>"
    "<div class='pw-wrap'><input name='pass' type='password' id='pw' placeholder='Enter password' autocomplete='off'>"
    "<button type='button' class='pw-toggle' onclick=\"var p=document.getElementById('pw');"
    "p.type=p.type==='password'?'text':'password';this.textContent=p.type==='password'?'Show':'Hide'\">Show</button>"
    "</div></div>"
    "<button type='submit'>Connect</button>"
    "</form>"
    "<div class='forget'>"
    "<form method='POST' action='/forget' onsubmit=\"return confirm('Erase saved WiFi and restart setup?')\">"
    "<button type='submit'>Forget WiFi</button>"
    "</form></div>"
    "<p class='note'>After connecting, OTA firmware updates are at "
    "<a href='/ota'>http://&lt;device-ip&gt;/ota</a></p>"
    "</div></body></html>";

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
    return ESP_OK;
}

// Simple URL-decode in place
static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// x-www-form-urlencoded field extractor. Anchors the key to a field
// boundary (start of buffer or just after '&') so a value containing
// the key substring can't false-match - e.g. ssid="mypass=net" must
// not satisfy a lookup for "pass". Const-correct throughout.
static void parse_form_field(const char *buf, const char *key,
                             char *out, int out_max)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = buf;
    while (p && *p) {
        const char *eq = p + klen;
        if (strncmp(p, key, klen) == 0 && *eq == '=') {
            const char *val = eq + 1;
            const char *end = strchr(val, '&');
            int len = end ? (int)(end - val) : (int)strlen(val);
            if (len > out_max - 1) len = out_max - 1;
            memcpy(out, val, len);
            out[len] = '\0';
            url_decode(out);
            return;
        }
        const char *amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;   // advance to next field boundary
    }
}

static const char RESP_REBOOT[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0;padding:0}body{font-family:-apple-system,system-ui,sans-serif;"
    "background:#1F1E1D;color:#F0EEE6;display:flex;justify-content:center;"
    "align-items:center;min-height:100vh;text-align:center;padding:1em}"
    ".card{background:#262625;padding:2.5em 2em;border-radius:16px;"
    "box-shadow:0 8px 32px rgba(0,0,0,.5);max-width:360px;width:100%%}"
    "h2{font-size:1.3em;margin-bottom:.5em;color:#F0EEE6}"
    "p{color:#8a8782;font-size:.9em}"
    ".spin{display:inline-block;width:28px;height:28px;border:3px solid #3a3936;"
    "border-top-color:#D97757;border-radius:50%%;animation:s .8s linear infinite;margin-bottom:1em}"
    "@keyframes s{to{transform:rotate(360deg)}}"
    "</style></head><body><div class='card'>"
    "<div class='spin'></div>"
    "<h2>%s</h2><p>%s</p>"
    "</div></body></html>";

static void reboot_after(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_restart();
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char buf[384] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[33], pass[65];
    parse_form_field(buf, "ssid", ssid, sizeof(ssid));
    parse_form_field(buf, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    storage_set_str(NVS_KEY_SSID, ssid);
    storage_set_str(NVS_KEY_PASS, pass);
    ESP_LOGI(TAG, "Portal saved SSID='%s' - rebooting to connect", ssid);

    char page[sizeof(RESP_REBOOT) + 96];
    snprintf(page, sizeof(page), RESP_REBOOT,
             "Saved", "The buddy will restart and join your network.");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, strlen(page));

    reboot_after(1200);
    return ESP_OK;   // unreachable
}

static esp_err_t portal_forget_handler(httpd_req_t *req)
{
    wifi_manager_erase_creds();
    ESP_LOGW(TAG, "Portal: WiFi creds forgotten - rebooting to setup");

    char page[sizeof(RESP_REBOOT) + 96];
    snprintf(page, sizeof(page), RESP_REBOOT,
             "WiFi Forgotten", "Restarting back into setup mode.");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, strlen(page));

    reboot_after(1200);
    return ESP_OK;   // unreachable
}

// Captive-portal OS probe handler. iOS/macOS GET captive.apple.com paths,
// Android /generate_204, Windows /connecttest.txt|/ncsi.txt. Each only
// pops the "Sign in to network" sheet if the probe gets back something
// OTHER than the OS's expected success response. Modern iOS is also
// finicky about bare 302s, so we serve the full portal page with a
// 200 - the captive browser renders it directly. Registered as the
// catch-all (wildcard URI) so ANY unknown host/path lands on setup.
static esp_err_t portal_captive_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
    return ESP_OK;
}

void wifi_manager_get_ap_name(char *out, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "Buddy-%02X%02X", mac[4], mac[5]);
}

// Start the shared httpd if it isn't running, then register the WiFi-
// related routes (portal pages + Forget). The wildcard captive catch-all
// is registered ONLY in AP mode - in STA mode it would hijack every
// route including any future /ota.
//
// Modules that own additional routes (ota_server.c) call
// wifi_manager_get_httpd() and register their URIs onto the same
// handle. Order matters with wildcard matching: specifics first, then
// the wildcard. We add the wildcard last here in AP mode so OTA-side
// /ota (registered later) still gets dispatched correctly because it
// has its own /ota literal URI that wins over /*.
static void start_shared_httpd(bool ap_mode)
{
    if (!shared_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 12;
        cfg.lru_purge_enable = true;
        if (ap_mode) cfg.uri_match_fn = httpd_uri_match_wildcard;
        // OTA upload bodies are ~1.1MB - bump the recv timeout from 5s
        // default so the trickle doesn't drop the connection mid-flash.
        cfg.recv_wait_timeout = 30;
        cfg.send_wait_timeout = 30;
        // Stack: httpd's worker decodes the upload body in chunks; we
        // need room for our 4 KB OTA buffer + cJSON heartbeat parsing
        // doesn't go through here, so default is fine.
        ESP_ERROR_CHECK(httpd_start(&shared_httpd, &cfg));
        ESP_LOGI(TAG, "HTTP server up (mode=%s)", ap_mode ? "AP" : "STA");
    }

    if (routes_registered) return;
    routes_registered = true;

    httpd_uri_t uri_save    = { .uri = "/save",   .method = HTTP_POST, .handler = portal_save_handler };
    httpd_uri_t uri_forget  = { .uri = "/forget", .method = HTTP_POST, .handler = portal_forget_handler };
    httpd_uri_t uri_root    = { .uri = "/",       .method = HTTP_GET,  .handler = portal_get_handler };
    httpd_register_uri_handler(shared_httpd, &uri_save);
    httpd_register_uri_handler(shared_httpd, &uri_forget);
    httpd_register_uri_handler(shared_httpd, &uri_root);
    if (ap_mode) {
        httpd_uri_t uri_any = { .uri = "/*", .method = HTTP_GET, .handler = portal_captive_handler };
        httpd_register_uri_handler(shared_httpd, &uri_any);
    }
}

void *wifi_manager_get_httpd(void) { return (void *)shared_httpd; }

static void portal_task(void *arg)
{
    char ap_name[20];
    wifi_manager_get_ap_name(ap_name, sizeof(ap_name));

    ESP_LOGI(TAG, "Captive portal AP: '%s' - connect, browse http://192.168.4.1", ap_name);

    esp_wifi_stop();
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ap_name, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ap_name);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_shared_httpd(/*ap_mode=*/true);

    // Server runs on its own httpd task; this task is done. Handlers
    // reboot the device on save/forget so there's nothing to wait for.
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_portal(void)
{
    return xTaskCreate(portal_task, "wifi_portal", 4096, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_manager_erase_creds(void)
{
    storage_erase_key(NVS_KEY_SSID);
    storage_erase_key(NVS_KEY_PASS);
    ESP_LOGI(TAG, "WiFi credentials erased");
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return connected; }

int8_t wifi_manager_get_rssi(void)
{
    if (!connected) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

void wifi_manager_get_ip_str(char *out, size_t len)
{
    if (connected) snprintf(out, len, IPSTR, IP2STR(&sta_ip));
    else           snprintf(out, len, "0.0.0.0");
}

void wifi_manager_get_mac_str(char *out, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
