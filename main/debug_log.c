// In-RAM circular log buffer + /debug HTTP routes.
//
// The board has no JTAG header and the only serial output is the FTDI
// header we need physical access to reach. After the first OTA, we lose
// that path entirely - this module is what replaces it. ESP_LOG* output
// is teed through esp_log_set_vprintf() into a ring buffer; the /debug
// endpoint dumps it over the same httpd the OTA page lives on.
//
// Memory: 8 KB in .bss. Tried 32 KB once and the device hit `min free
// heap 64 B` during boot - too close to OOM with WiFi + BLE resident.
// 8 KB captures ~150 short log lines, plenty for triage of the failure
// modes we care about (BLE reconnect, OTA, WiFi).

#include "debug_log.h"
#include "app_version.h"
#include "bridge.h"
#include "ble_nus.h"
#include "stats.h"
#include "translog.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_RING_CAP 8192

static char         s_ring[LOG_RING_CAP];
static volatile size_t s_head = 0;   // next write
static volatile bool   s_wrapped = false;
static SemaphoreHandle_t s_mux = NULL;
static vprintf_like_t  s_prev_vprintf = NULL;

// Push raw bytes into the ring. Newest at head; on wrap, oldest bytes
// at (head+1)%cap are simply overwritten - we don't move tail. Reads
// reconstruct chronological order.
static void ring_push(const char *p, size_t n)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        s_ring[s_head] = p[i];
        s_head++;
        if (s_head >= LOG_RING_CAP) { s_head = 0; s_wrapped = true; }
    }
    xSemaphoreGive(s_mux);
}

// vprintf replacement: format into a stack buffer, push into ring, then
// also forward to the previous handler so UART output keeps working.
// Stack buf is 256B - long ESP_LOG lines (>256B) truncate in the ring
// but the UART path still gets the full line via s_prev_vprintf.
static int log_vprintf_tap(const char *fmt, va_list ap)
{
    char buf[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t cap = (n >= (int)sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
        ring_push(buf, cap);
    }
    int rv = s_prev_vprintf ? s_prev_vprintf(fmt, ap2) : vprintf(fmt, ap2);
    va_end(ap2);
    return rv;
}

void debug_log_init(void)
{
    if (s_mux) return;  // idempotent
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return;
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf_tap);
}

size_t debug_log_used(void)
{
    return s_wrapped ? LOG_RING_CAP : s_head;
}

size_t debug_log_snapshot(char *out, size_t cap)
{
    return debug_log_snapshot_at(0, out, cap);
}

size_t debug_log_snapshot_at(size_t skip, char *out, size_t cap)
{
    if (!s_mux || !out || cap == 0) return 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t used   = s_wrapped ? LOG_RING_CAP : s_head;
    size_t oldest = s_wrapped ? s_head : 0;   // index of oldest byte
    if (skip >= used) {
        xSemaphoreGive(s_mux);
        return 0;
    }
    size_t avail = used - skip;
    size_t n     = (avail < cap) ? avail : cap;
    size_t start = (oldest + skip) % LOG_RING_CAP;
    // Two slices to handle ring wrap.
    if (start + n <= LOG_RING_CAP) {
        memcpy(out, s_ring + start, n);
    } else {
        size_t first = LOG_RING_CAP - start;
        memcpy(out, s_ring + start, first);
        memcpy(out + first, s_ring, n - first);
    }
    xSemaphoreGive(s_mux);
    return n;
}

// ─────────────────────────────────────────────────────────────────────
// HTTP handlers
// ─────────────────────────────────────────────────────────────────────

static const char DEBUG_PAGE[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Claude Buddy Debug</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1F1E1D;"
    "color:#F0EEE6;padding:1em;max-width:980px;margin:0 auto}"
    "h1{font-size:1.2em;font-weight:700;margin-bottom:.3em}"
    "h2{font-size:.85em;color:#D97757;margin:1.4em 0 .5em;letter-spacing:.06em;"
    "text-transform:uppercase}"
    ".sub{font-size:.78em;color:#8a8782;margin-bottom:1em}"
    ".row{display:flex;gap:.6em;margin-bottom:.6em;flex-wrap:wrap}"
    "button,a.btn{background:#262625;color:#F0EEE6;border:1px solid #3a3936;"
    "padding:.55em 1em;border-radius:8px;cursor:pointer;font-family:inherit;"
    "font-size:.85em;text-decoration:none;display:inline-block}"
    "button:hover,a.btn:hover{background:#2e2d2c;border-color:#D97757}"
    "pre{background:#0f0f0e;border:1px solid #3a3936;border-radius:8px;"
    "padding:.8em;overflow:auto;max-height:60vh;font-family:'SF Mono',"
    "Menlo,Consolas,monospace;font-size:.72em;line-height:1.4;color:#cfcdc7;"
    "white-space:pre-wrap;word-break:break-all}"
    ".pill{display:inline-block;padding:.15em .55em;border-radius:10px;"
    "background:#262625;color:#D97757;font-size:.7em;letter-spacing:.05em;"
    "margin-left:.4em;vertical-align:middle}"
    ".hint{font-size:.72em;color:#6a6863;margin-top:.4em}"
    "</style></head><body>"
    "<h1>Claude Buddy <span class='pill' id='ver'>...</span></h1>"
    "<div class='sub'>Live diagnostics over WiFi.</div>"

    "<h2>Status</h2>"
    "<pre id='status'>loading...</pre>"
    "<div class='row'>"
    "<button onclick='refresh()'>Refresh</button>"
    "<label><input type='checkbox' id='auto' checked> auto-refresh 3s</label>"
    "</div>"

    "<h2>Live log <span class='hint'>(in-RAM ring, ~32 KB)</span></h2>"
    "<pre id='log'>loading...</pre>"
    "<div class='row'>"
    "<button onclick='loadLog()'>Refresh log</button>"
    "<a class='btn' href='/debug/log' target='_blank'>Open raw</a>"
    "</div>"

    "<h2>Transcript log <span class='hint'>(SPIFFS, persists across reboots)</span></h2>"
    "<pre id='tr'>loading...</pre>"
    "<div class='row'>"
    "<a class='btn' href='/debug/transcript' target='_blank'>Open raw CSV</a>"
    "</div>"

    "<h2>Links</h2>"
    "<div class='row'>"
    "<a class='btn' href='/ota'>OTA / factory reset</a>"
    "</div>"

    "<script>"
    "const $=id=>document.getElementById(id);"
    "async function loadStatus(){"
    "  try{const r=await fetch('/debug/status');const t=await r.text();"
    "    $('status').textContent=t;"
    "    const m=t.match(/version\\s+(\\S+)/);if(m)$('ver').textContent=m[1];"
    "  }catch(e){$('status').textContent='error: '+e}"
    "}"
    "async function loadLog(){"
    "  try{const r=await fetch('/debug/log');const t=await r.text();"
    "    $('log').textContent=t||'(empty)';"
    "    $('log').scrollTop=$('log').scrollHeight;"
    "  }catch(e){$('log').textContent='error: '+e}"
    "}"
    "async function loadTr(){"
    "  try{const r=await fetch('/debug/transcript');const t=await r.text();"
    "    $('tr').textContent=t||'(empty)';"
    "    $('tr').scrollTop=$('tr').scrollHeight;"
    "  }catch(e){$('tr').textContent='error: '+e}"
    "}"
    "function refresh(){loadStatus();loadLog();loadTr()}"
    "refresh();"
    "setInterval(()=>{if($('auto').checked)refresh()},3000);"
    "</script></body></html>";

static esp_err_t debug_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DEBUG_PAGE, sizeof(DEBUG_PAGE) - 1);
}

// Stream the ring out in 1 KB stack chunks. A naive "alloc LOG_RING_CAP
// up front" approach OOMs once WiFi + httpd are resident (steady-state
// free heap is on the order of single-digit KB).
//
// Trade-off: a long-running dump can race with new log writes, so a
// chunk boundary might split a line. Fine for a debug endpoint.
static esp_err_t debug_log_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    size_t total = debug_log_used();
    if (total == 0) {
        httpd_resp_sendstr(req, "(no log captured yet)\n");
        return ESP_OK;
    }

    char chunk[1024];
    size_t sent = 0;
    while (sent < total) {
        size_t got = debug_log_snapshot_at(sent, chunk, sizeof(chunk));
        if (got == 0) break;
        if (httpd_resp_send_chunk(req, chunk, got) != ESP_OK) break;
        sent += got;
    }
    httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked response
    return ESP_OK;
}

static esp_err_t debug_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    tama_state_t st;
    bridge_get_state(&st);
    const stats_t *sg = stats_get();

    uint64_t up_us = esp_timer_get_time();
    uint32_t up_s  = (uint32_t)(up_us / 1000000ULL);
    uint32_t hb_age = (uint32_t)((esp_timer_get_time() / 1000) - st.last_updated_ms) / 1000;

    char body[1024];
    int n = snprintf(body, sizeof(body),
        "version       %s\n"
        "uptime        %luh %02lum %02lus\n"
        "free heap     %u B\n"
        "min free heap %u B\n"
        "largest block %u B\n"
        "\n"
        "ble connected %s\n"
        "ble encrypted %s\n"
        "ble passkey   %s\n"
        "\n"
        "data alive    %s\n"
        "last hb age   %lus\n"
        "sessions      %u total / %u run / %u wait\n"
        "tokens today  %lu\n"
        "transcript    %u lines (gen %u)\n"
        "msg           %s\n"
        "\n"
        "stats lvl     %u\n"
        "approvals     %u\n"
        "denials       %u\n"
        "\n"
        "translog      %s\n",
        APP_VERSION,
        (unsigned long)(up_s / 3600), (unsigned long)((up_s / 60) % 60), (unsigned long)(up_s % 60),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        ble_connected() ? "yes" : "no",
        ble_secure() ? "yes" : "no",
        ble_passkey() ? "showing" : "no",
        st.connected ? "yes" : "no",
        (unsigned long)hb_age,
        st.sessions_total, st.sessions_running, st.sessions_waiting,
        (unsigned long)st.tokens_today,
        st.n_lines, st.line_gen,
        st.msg[0] ? st.msg : "(none)",
        sg->level,
        sg->approvals,
        sg->denials,
        translog_status_str()
    );
    if (n < 0) n = 0;
    return httpd_resp_send(req, body, n);
}

static esp_err_t debug_transcript_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=transcript.csv");
    return translog_dump_http(req);
}

esp_err_t debug_log_register_routes(httpd_handle_t server)
{
    if (!server) return ESP_FAIL;
    httpd_uri_t r1 = { .uri = "/debug",            .method = HTTP_GET, .handler = debug_page_handler };
    httpd_uri_t r2 = { .uri = "/debug/log",        .method = HTTP_GET, .handler = debug_log_handler };
    httpd_uri_t r3 = { .uri = "/debug/status",     .method = HTTP_GET, .handler = debug_status_handler };
    httpd_uri_t r4 = { .uri = "/debug/transcript", .method = HTTP_GET, .handler = debug_transcript_handler };
    esp_err_t e1 = httpd_register_uri_handler(server, &r1);
    esp_err_t e2 = httpd_register_uri_handler(server, &r2);
    esp_err_t e3 = httpd_register_uri_handler(server, &r3);
    esp_err_t e4 = httpd_register_uri_handler(server, &r4);
    if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}
