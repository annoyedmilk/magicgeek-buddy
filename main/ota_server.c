// Browser OTA: drag-drop a .bin onto an HTML page, the device flashes
// it to the inactive OTA slot and reboots. Mounts onto wifi_manager's
// shared httpd. After the first manual flash, all subsequent firmware
// updates go through this endpoint.
//
// Bootloader rollback is enabled, so a bad image that never reaches
// ota_mark_valid() (called from buddy_task after WiFi is up) reverts
// on the next boot.
#include "ota_server.h"
#include "wifi_manager.h"
#include "stats.h"
#include "ble_nus.h"
#include "net_trust.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_http_server.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE 4096

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "marking firmware valid (cancel pending rollback)");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Claude-night theme upload page. Single `%`s are intentional CSS units
// - the buffer is sent via httpd_resp_send (NOT printf'd), so they don't
// need escaping.
//
// Two surfaces on this page:
//   1. Firmware upload (drag/drop a .bin → POST /ota)
//   2. Danger zone: factory reset (POST /ota/reset; requires a typed
//      confirm in the input field so it can't be triggered by accident).
// ─────────────────────────────────────────────────────────────────────
static const char OTA_PAGE[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Claude Buddy OTA</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1F1E1D;"
    "color:#F0EEE6;display:flex;justify-content:center;align-items:center;"
    "min-height:100vh;padding:1em}"
    ".card{background:#262625;padding:2em 1.5em;border-radius:16px;"
    "width:100%;max-width:380px;box-shadow:0 8px 32px rgba(0,0,0,.5);"
    "text-align:center}"
    ".logo{margin-bottom:1.5em}"
    ".logo h1{font-size:1.3em;font-weight:700;color:#F0EEE6;margin:0 0 .15em}"
    ".logo p{font-size:.85em;color:#8a8782}"
    ".drop{border:2px dashed #3a3936;border-radius:12px;padding:2em 1em;"
    "margin-bottom:1.2em;transition:border-color .2s,background .2s;cursor:pointer}"
    ".drop.over{border-color:#D97757;background:rgba(217,119,87,.08)}"
    ".drop.has-file{border-color:#22C55E;border-style:solid;background:rgba(34,197,94,.05)}"
    ".drop-icon{font-size:2em;margin-bottom:.4em;display:block;color:#D97757}"
    ".drop-text{font-size:.9em;color:#8a8782}"
    ".fname{font-size:.85em;color:#F0EEE6;margin-top:.5em;word-break:break-all}"
    "input[type=file]{display:none}"
    "button{width:100%;padding:.85em;border:none;border-radius:10px;"
    "background:#D97757;color:#1F1E1D;font-size:1em;font-weight:700;cursor:pointer;"
    "transition:filter .2s}"
    "button:active{filter:brightness(.85)}"
    "button:disabled{opacity:.4;cursor:not-allowed;filter:none}"
    ".bar{width:100%;height:6px;background:#3a3936;border-radius:3px;"
    "margin-top:1em;overflow:hidden;display:none}"
    ".bar>div{height:100%;width:0%;background:#D97757;border-radius:3px;"
    "transition:width .2s}"
    "#status{margin-top:.9em;min-height:1.4em;font-size:.9em;color:#8a8782}"
    ".ok{color:#22C55E}.err{color:#EF4444}"
    ".hint{margin-top:1.4em;font-size:.78em;color:#6a6863;border-top:1px solid #3a3936;padding-top:1em}"
    ".danger{margin-top:2em;border-top:1px solid #3a3936;padding-top:1.4em;text-align:left}"
    ".danger h2{font-size:.95em;color:#EF4444;margin-bottom:.4em;font-weight:700;"
    "letter-spacing:.05em;text-transform:uppercase}"
    ".danger p{font-size:.78em;color:#8a8782;margin-bottom:.8em;line-height:1.45}"
    ".danger input{width:100%;padding:.6em .8em;border:1px solid #3a3936;"
    "border-radius:8px;background:#1F1E1D;color:#F0EEE6;font-size:.9em;"
    "font-family:inherit;margin-bottom:.6em}"
    ".danger input:focus{outline:none;border-color:#EF4444}"
    ".danger button{background:#EF4444;color:#F0EEE6}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='logo'>"
    "<h1>Claude Buddy</h1><p>Firmware Update</p></div>"
    "<div class='drop' id='drop' onclick=\"document.getElementById('file').click()\">"
    "<span class='drop-icon'>&#11014;</span>"
    "<div class='drop-text'>Tap to select or drag a <b>.bin</b> file</div>"
    "<div class='fname' id='fname'></div>"
    "</div>"
    "<input type='file' id='file' accept='.bin'>"
    "<button id='btn' disabled>Upload Firmware</button>"
    "<div class='bar' id='bar'><div id='fill'></div></div>"
    "<div id='status'></div>"
    "<p class='hint'>The buddy will reboot automatically when the upload finishes. "
    "If a bad image bricks the boot, the bootloader rolls back to the last good one.</p>"

    "<div class='danger'>"
    "<h2>Danger zone</h2>"
    "<p>Factory reset wipes stats, settings, paired BLE bonds, and saved "
    "WiFi credentials. The device will reboot into captive-portal mode "
    "and need to be re-paired with Claude desktop.</p>"
    "<p>Type <b>RESET</b> to enable the button.</p>"
    "<input id='confirm' placeholder='type RESET to confirm' autocomplete='off'>"
    "<button id='rbtn' disabled>Factory reset and reboot</button>"
    "<div id='rstatus' style='margin-top:.6em;min-height:1.2em;font-size:.85em;color:#8a8782'></div>"
    "</div>"
    "</div>"

    "<script>"
    "const drop=document.getElementById('drop'),"
    "fi=document.getElementById('file'),"
    "fn=document.getElementById('fname'),"
    "btn=document.getElementById('btn'),"
    "st=document.getElementById('status'),"
    "bar=document.getElementById('bar'),"
    "fill=document.getElementById('fill'),"
    "rconf=document.getElementById('confirm'),"
    "rbtn=document.getElementById('rbtn'),"
    "rst=document.getElementById('rstatus');"
    "function pick(f){if(f){fn.textContent=f.name+' ('+Math.round(f.size/1024)+' KB)';"
    "drop.classList.add('has-file');btn.disabled=false}}"
    "fi.onchange=()=>pick(fi.files[0]);"
    "drop.ondragover=e=>{e.preventDefault();drop.classList.add('over')};"
    "drop.ondragleave=()=>drop.classList.remove('over');"
    "drop.ondrop=e=>{e.preventDefault();drop.classList.remove('over');"
    "fi.files=e.dataTransfer.files;pick(fi.files[0])};"
    "btn.onclick=()=>{"
    "const file=fi.files[0];if(!file)return;"
    "btn.disabled=true;bar.style.display='block';"
    "st.textContent='Uploading...';st.className='';"
    "const xhr=new XMLHttpRequest();"
    "xhr.open('POST','/ota');"
    "xhr.upload.onprogress=e=>{if(e.lengthComputable){"
    "const p=Math.round(e.loaded/e.total*100);"
    "fill.style.width=p+'%';st.textContent='Uploading: '+p+'%'}};"
    "xhr.onload=()=>{"
    "if(xhr.status===200){fill.style.width='100%';"
    "st.className='ok';st.textContent='Success! Rebooting...'}"
    "else{st.className='err';st.textContent='Error: '+xhr.responseText;btn.disabled=false}};"
    "xhr.onerror=()=>{st.className='err';st.textContent='Upload failed';btn.disabled=false};"
    "xhr.send(file)};"
    "rconf.oninput=()=>{rbtn.disabled=(rconf.value.trim()!=='RESET')};"
    "rbtn.onclick=()=>{"
    "if(rconf.value.trim()!=='RESET')return;"
    "if(!confirm('Really wipe everything and reboot?'))return;"
    "rbtn.disabled=true;rst.className='';rst.textContent='Wiping NVS...';"
    "const xhr=new XMLHttpRequest();"
    "xhr.open('POST','/ota/reset');"
    "xhr.onload=()=>{"
    "if(xhr.status===200){rst.className='ok';"
    "rst.textContent='Wiped. Device is rebooting into captive-portal mode.'}"
    "else{rst.className='err';rst.textContent='Reset failed: '+xhr.responseText;rbtn.disabled=false}};"
    "xhr.onerror=()=>{rst.className='err';rst.textContent='Network error';rbtn.disabled=false};"
    "xhr.send()};"
    "</script></body></html>";

// /ota* needs ADMIN trust. On non-admin networks we return 404 so the
// device appears not to exist - the same shape as /debug* on untrusted
// networks. AP/captive-portal mode bypasses the gate.
static esp_err_t ota_gate_or_404(httpd_req_t *req)
{
    if (net_trust_allows(NET_TRUST_ADMIN)) return ESP_OK;
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    if (ota_gate_or_404(req) != ESP_OK) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, OTA_PAGE, sizeof(OTA_PAGE) - 1);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (ota_gate_or_404(req) != ESP_OK) return ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA target partition: %s (offset 0x%lx, size 0x%lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    ESP_LOGI(TAG, "receiving OTA firmware (%d bytes)...", remaining);

    while (remaining > 0) {
        int received = httpd_req_recv(req, buf,
            (remaining < OTA_BUF_SIZE) ? remaining : OTA_BUF_SIZE);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "receive error %d", received);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success - rebooting in 500ms");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

// Defer the reboot so the HTTP response actually goes out before the
// socket dies. 500 ms matches the OTA-success path's reboot delay.
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// POST /ota/reset - factory-reset on confirmation. Same effect as the
// touch-UI factory-reset path (stats wiped, BLE bonds cleared, WiFi
// creds erased, reboot). Confirmation is enforced client-side via the
// "RESET" text input + a JS confirm() dialog; we don't double-check
// here because a clear text-based gate on a LAN-only endpoint is
// already more friction than the touch UI's 5-second arm window.
static esp_err_t ota_reset_handler(httpd_req_t *req)
{
    if (ota_gate_or_404(req) != ESP_OK) return ESP_OK;
    ESP_LOGW(TAG, "factory reset requested via /ota/reset - wiping");
    stats_factory_reset();
    ble_clear_bonds();
    wifi_manager_erase_creds();

    httpd_resp_sendstr(req, "OK");

    // Schedule the reboot on its own task so the response flushes first.
    // priority 5 (same as buddy/bridge) so it preempts the httpd worker
    // promptly after the delay elapses.
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t ota_start(void)
{
    httpd_handle_t server = (httpd_handle_t)wifi_manager_get_httpd();
    if (!server) {
        ESP_LOGW(TAG, "shared httpd not up - OTA route NOT registered");
        return ESP_FAIL;
    }
    httpd_uri_t uri_get = {
        .uri = "/ota", .method = HTTP_GET, .handler = ota_get_handler,
    };
    httpd_uri_t uri_post = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler,
    };
    httpd_uri_t uri_reset = {
        .uri = "/ota/reset", .method = HTTP_POST, .handler = ota_reset_handler,
    };
    esp_err_t r1 = httpd_register_uri_handler(server, &uri_get);
    esp_err_t r2 = httpd_register_uri_handler(server, &uri_post);
    esp_err_t r3 = httpd_register_uri_handler(server, &uri_reset);
    if (r1 == ESP_OK && r2 == ESP_OK && r3 == ESP_OK) {
        ESP_LOGI(TAG, "OTA routes mounted: /ota (GET/POST) + /ota/reset (POST)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "register /ota failed: %s / %s / %s",
             esp_err_to_name(r1), esp_err_to_name(r2), esp_err_to_name(r3));
    return ESP_FAIL;
}
