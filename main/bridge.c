// Bridge: NUS RX → cJSON → tama_state_t + command dispatch; touch →
// permission reply. See bridge.h for the protocol context.
//
// Structure (top to bottom):
//   1. State + line buffer (mutex-protected; render loop reads in parallel)
//   2. JSON helpers (read/copy string fields safely)
//   3. apply_heartbeat() - populate TamaState from a snapshot
//   4. dispatch() - handle commands ("status"/"name"/"owner"/"unpair"/...)
//   5. apply_json() - top-level: route line to command or heartbeat path
//   6. bridge_task() - drain ble_read() into a line buffer, call apply_json
//   7. Public API: bridge_init / get_state / derive / send_permission
#include "bridge.h"
#include "ble_nus.h"
#include "storage.h"
#include "stats.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bridge";

#define DATA_TIMEOUT_MS 30000   // REFERENCE.md: stale if no heartbeat in 30s

// ─────────────────────────────────────────────────────────────────────
// 1. State + line buffer
// ─────────────────────────────────────────────────────────────────────

static tama_state_t      g_state;
static SemaphoreHandle_t g_state_mux;

// Timestamp the *arrival* of each new prompt id so stats_on_approval can
// record seconds-to-respond into the velocity ring. Cleared (and re-armed)
// in apply_heartbeat whenever prompt.id transitions.
static uint32_t g_prompt_arrived_ms = 0;
static char     g_last_seen_prompt_id[40] = "";

// Owner / device names live in NVS so they survive reboot. Defaults
// match the source. Trimmed of JSON-breaking chars by safe_copy() before
// being committed.
static char g_petname[24]  = "Buddy";
static char g_ownername[32] = "";

// inline millis() - we don't link Arduino. esp_timer is monotonic.
static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// One-line accumulator for the JSON stream. Sized to comfortably hold a
// full heartbeat (entries[] + a transcript snippet + headroom).
#define LINE_CAP 1536
static char g_line[LINE_CAP];
static size_t g_line_len = 0;

// ─────────────────────────────────────────────────────────────────────
// 2. JSON helpers
// ─────────────────────────────────────────────────────────────────────

static void safe_copy(char *dst, size_t dst_len, const char *src)
{
    // The desktop is friendly, but acks printf these into JSON without
    // escaping - strip quotes/backslashes/control chars to be defensive.
    size_t j = 0;
    if (!src || dst_len == 0) { if (dst_len) dst[0] = 0; return; }
    for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\' || c < 0x20) continue;
        dst[j++] = c;
    }
    dst[j] = 0;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

// Send one JSON line (terminated with '\n') over NUS. The bridge is
// the only writer; no mutex needed here.
static void send_line(const char *json)
{
    size_t n = strlen(json);
    // Temporary: log every TX line so xfer ack failures are diagnosable.
    ESP_LOGI(TAG, "tx: %.120s%s", json, n > 120 ? "..." : "");
    ble_write((const uint8_t *)json, n);
    ble_write((const uint8_t *)"\n", 1);
}

// ─────────────────────────────────────────────────────────────────────
// 3. Heartbeat → TamaState
// ─────────────────────────────────────────────────────────────────────

static void apply_heartbeat(const cJSON *doc)
{
    xSemaphoreTake(g_state_mux, portMAX_DELAY);

    g_state.sessions_total     = (uint8_t)json_int(doc, "total",   g_state.sessions_total);
    g_state.sessions_running   = (uint8_t)json_int(doc, "running", g_state.sessions_running);
    g_state.sessions_waiting   = (uint8_t)json_int(doc, "waiting", g_state.sessions_waiting);
    g_state.recently_completed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "completed"));
    g_state.tokens_today       = (uint32_t)json_int(doc, "tokens_today", (int)g_state.tokens_today);

    // Forward cumulative bridge tokens into stats (delta-tracked with the
    // first-sight latch + bridge-restart resync - see stats.c).
    cJSON *tok = cJSON_GetObjectItemCaseSensitive(doc, "tokens");
    if (cJSON_IsNumber(tok)) {
        stats_on_bridge_tokens((uint32_t)tok->valuedouble);
    }

    const char *m = json_str(doc, "msg");
    if (m) {
        strncpy(g_state.msg, m, sizeof(g_state.msg) - 1);
        g_state.msg[sizeof(g_state.msg) - 1] = 0;
    }

    // entries: newest first per REFERENCE.md. We store same order, up
    // to 6 rows of ~80 chars each; the persona screen renders 3 at a
    // time and scrolls back through history on TAP.
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(doc, "entries");
    if (cJSON_IsArray(entries)) {
        uint8_t n = 0;
        cJSON *line;
        cJSON_ArrayForEach(line, entries) {
            if (n >= 6) break;
            const char *s = cJSON_IsString(line) ? line->valuestring : "";
            strncpy(g_state.lines[n], s, sizeof(g_state.lines[0]) - 1);
            g_state.lines[n][sizeof(g_state.lines[0]) - 1] = 0;
            n++;
        }
        // line_gen bumps if count changed or the newest line differs;
        // good enough to drive a "scroll reset on new content" flag.
        if (n != g_state.n_lines || (n > 0 && strcmp(g_state.lines[0], g_state.msg) != 0)) {
            g_state.line_gen++;
        }
        g_state.n_lines = n;
    }

    // prompt - present iff a permission decision is needed.
    cJSON *prompt = cJSON_GetObjectItemCaseSensitive(doc, "prompt");
    if (cJSON_IsObject(prompt)) {
        const char *id   = json_str(prompt, "id");
        const char *tool = json_str(prompt, "tool");
        const char *hint = json_str(prompt, "hint");
        strncpy(g_state.prompt_id,   id   ? id   : "", sizeof(g_state.prompt_id)   - 1);
        strncpy(g_state.prompt_tool, tool ? tool : "", sizeof(g_state.prompt_tool) - 1);
        strncpy(g_state.prompt_hint, hint ? hint : "", sizeof(g_state.prompt_hint) - 1);
        g_state.prompt_id  [sizeof(g_state.prompt_id)   - 1] = 0;
        g_state.prompt_tool[sizeof(g_state.prompt_tool) - 1] = 0;
        g_state.prompt_hint[sizeof(g_state.prompt_hint) - 1] = 0;
        // Arm the response timer on a NEW prompt id (not on repeat
        // heartbeats carrying the same waiting prompt). Used by
        // stats_on_approval to record seconds-to-respond.
        if (strcmp(g_state.prompt_id, g_last_seen_prompt_id) != 0) {
            g_prompt_arrived_ms = millis();
            strncpy(g_last_seen_prompt_id, g_state.prompt_id,
                    sizeof(g_last_seen_prompt_id) - 1);
            g_last_seen_prompt_id[sizeof(g_last_seen_prompt_id) - 1] = 0;
        }
    } else {
        g_state.prompt_id[0] = g_state.prompt_tool[0] = g_state.prompt_hint[0] = 0;
        g_last_seen_prompt_id[0] = 0;
        g_prompt_arrived_ms = 0;
    }

    g_state.last_updated_ms = millis();
    g_state.connected = true;
    xSemaphoreGive(g_state_mux);
}

// ─────────────────────────────────────────────────────────────────────
// 4. Command dispatch + acks
// ─────────────────────────────────────────────────────────────────────

static void ack_ok(const char *cmd) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ack\":\"%s\",\"ok\":true,\"n\":0}", cmd);
    send_line(buf);
}

// Reject a folder-push command with a clear error. The desktop surfaces
// the `error` string as a toast in the Hardware Buddy window. We no
// longer support GIF character packs on this hardware (the AnimatedGIF
// LZW init wouldn't fit our heap budget without smashing the bridge
// task stack), so every step of the folder-push protocol is rejected
// up-front instead of being silently ignored.
static void ack_unsupported(const char *cmd)
{
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ack\":\"%s\",\"ok\":false,"
             "\"error\":\"character packs not supported on this hardware\"}",
             cmd);
    send_line(buf);
}

static void ack_status(void)
{
    // Manual printf into a fixed buffer - heap-light and shape-stable
    // for the desktop's parser. See REFERENCE.md "Status response".
    const stats_t *s = stats_get();
    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
        "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
        "\"sys\":{\"up\":%lu,\"heap\":%u},"
        "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
        "}}",
        g_petname, g_ownername,
        ble_secure() ? "true" : "false",
        (unsigned long)(millis() / 1000),
        (unsigned)esp_get_free_heap_size(),
        s->approvals, s->denials,
        stats_median_velocity(),
        (unsigned long)s->nap_seconds,
        s->level);
    (void)len;
    send_line(buf);
}

// Returns true if the doc was a command (caller should not also run
// heartbeat parsing on it).
static bool dispatch(const cJSON *doc)
{
    const char *cmd = json_str(doc, "cmd");
    if (!cmd) return false;

    if (strcmp(cmd, "status") == 0) {
        ack_status();
        return true;
    }
    if (strcmp(cmd, "name") == 0) {
        const char *n = json_str(doc, "name");
        if (n) {
            safe_copy(g_petname, sizeof(g_petname), n);
            storage_set_str("petname", g_petname);
        }
        ack_ok("name");
        return true;
    }
    if (strcmp(cmd, "owner") == 0) {
        const char *n = json_str(doc, "name");
        if (n) {
            safe_copy(g_ownername, sizeof(g_ownername), n);
            storage_set_str("owner", g_ownername);
        }
        ack_ok("owner");
        return true;
    }
    if (strcmp(cmd, "unpair") == 0) {
        ble_clear_bonds();
        ack_ok("unpair");
        return true;
    }

    // ─── Folder push protocol (REFERENCE.md "Folder push") ───────────
    // GIF character packs are NOT supported on this hardware. We reject
    // char_begin up-front with a clean error so the desktop's Hardware
    // Buddy window surfaces a useful toast instead of timing out. The
    // subsequent file/chunk/file_end/char_end can't happen if char_begin
    // is rejected, but ack them defensively in case a future desktop
    // build sends them anyway.
    if (strcmp(cmd, "char_begin") == 0 ||
        strcmp(cmd, "file")       == 0 ||
        strcmp(cmd, "chunk")      == 0 ||
        strcmp(cmd, "file_end")   == 0 ||
        strcmp(cmd, "char_end")   == 0) {
        ack_unsupported(cmd);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────
// 5. Top-level JSON router
// ─────────────────────────────────────────────────────────────────────

static void apply_json(const char *line)
{
    cJSON *doc = cJSON_Parse(line);
    if (!doc) return;

    // Commands first (they carry "cmd"); otherwise treat as heartbeat.
    // Turn events ({"evt":"turn",...}) are not rendered on this hardware;
    // ignore them quietly.
    if (!dispatch(doc) && cJSON_IsObject(doc)) {
        const char *evt = json_str(doc, "evt");
        if (!evt) apply_heartbeat(doc);
    }
    cJSON_Delete(doc);
}

// ─────────────────────────────────────────────────────────────────────
// 6. Poll task: drain BLE RX → line buffer → apply_json
// ─────────────────────────────────────────────────────────────────────

static void bridge_task(void *arg)
{
    while (1) {
        // Drain everything the BLE stack has buffered. Most heartbeats
        // arrive as several MTU-sized fragments - accumulate until '\n'.
        while (ble_available()) {
            int b = ble_read();
            if (b < 0) break;
            if (b == '\n' || b == '\r') {
                if (g_line_len > 0) {
                    g_line[g_line_len] = 0;
                    if (g_line[0] == '{') {
                        // Temporary: log every received line at INFO so
                        // failures like "Stick did not respond to
                        // char_begin" are diagnosable without a logic
                        // analyzer. Drop back to LOGD once xfer is
                        // confirmed working end to end.
                        ESP_LOGI(TAG, "rx: %.120s%s", g_line,
                                 strlen(g_line) > 120 ? "..." : "");
                        apply_json(g_line);
                    }
                    g_line_len = 0;
                }
            } else if (g_line_len < LINE_CAP - 1) {
                g_line[g_line_len++] = (char)b;
            } else {
                // Overflow - drop the partial line. Should be rare; the
                // 1.5KB buffer covers heartbeats with full entries[].
                ESP_LOGW(TAG, "rx line overflow, dropped");
                g_line_len = 0;
            }
        }

        // Mark connection stale after 30s with no heartbeat (per spec).
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        if (g_state.connected &&
            (millis() - g_state.last_updated_ms) > DATA_TIMEOUT_MS) {
            g_state.connected = false;
            ESP_LOGW(TAG, "heartbeat stale (>30s) - marked disconnected");
        }
        xSemaphoreGive(g_state_mux);

        vTaskDelay(pdMS_TO_TICKS(50));   // 20Hz drain - plenty for NUS bandwidth
    }
}

// ─────────────────────────────────────────────────────────────────────
// 7. Public API
// ─────────────────────────────────────────────────────────────────────

void bridge_init(void)
{
    g_state_mux = xSemaphoreCreateMutex();

    // Restore persisted names. Missing keys are not errors.
    storage_get_str("petname", g_petname, sizeof(g_petname));
    storage_get_str("owner",   g_ownername, sizeof(g_ownername));

    // 8KB stack: 6KB was tight when a heartbeat arrived while character.cpp
    // was inside cJSON parse + AnimatedGIF init on the same core. Bumped
    // after a stack-corruption panic loop right after the bufo upload -
    // the canary bytes (0xa5a5a5a5) showed up in main_task's backtrace,
    // which only happens when another task overflows into the TCB region.
    xTaskCreate(bridge_task, "bridge", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "bridge up (petname='%s', owner='%s')", g_petname, g_ownername);
}

void bridge_get_state(tama_state_t *out)
{
    if (!out) return;
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    *out = g_state;
    xSemaphoreGive(g_state_mux);
}

persona_state_t bridge_derive(const tama_state_t *s)
{
    if (!s->connected)             return P_SLEEP;
    if (s->sessions_waiting > 0)   return P_ATTENTION;
    if (s->recently_completed)     return P_CELEBRATE;
    if (s->sessions_running >= 3)  return P_BUSY;
    return P_IDLE;
}

bool bridge_data_alive(void)
{
    bool alive;
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    alive = g_state.connected;
    xSemaphoreGive(g_state_mux);
    return alive;
}

void bridge_send_permission(bool allow)
{
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    if (g_state.prompt_id[0] == 0) {
        xSemaphoreGive(g_state_mux);
        return;
    }
    char id[40];
    strncpy(id, g_state.prompt_id, sizeof(id) - 1);
    id[sizeof(id) - 1] = 0;
    // Clear locally so the UI returns to persona view immediately; the
    // next heartbeat will reflect the same. Touch-driven approve/deny is
    // idempotent - even if the prompt re-appears we just won't re-fire.
    g_state.prompt_id[0] = g_state.prompt_tool[0] = g_state.prompt_hint[0] = 0;
    xSemaphoreGive(g_state_mux);

    // Capture response time BEFORE clearing prompt_arrived; logs the
    // velocity that statsMoodTier feeds on.
    uint32_t took_s = g_prompt_arrived_ms
        ? (millis() - g_prompt_arrived_ms) / 1000
        : 0;
    g_prompt_arrived_ms = 0;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
             id, allow ? "once" : "deny");
    send_line(buf);

    if (allow) stats_on_approval(took_s);
    else       stats_on_denial();
    ESP_LOGI(TAG, "permission %s for %s (took %lus)",
             allow ? "approved" : "denied", id, (unsigned long)took_s);
}
