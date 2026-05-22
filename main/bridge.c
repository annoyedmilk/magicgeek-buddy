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
#include "translog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
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
static volatile uint32_t g_state_generation = 0;

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

// One-line accumulator for the JSON stream. Heartbeats fit in ~1.5KB,
// but turn events (`{"evt":"turn",...}`) can carry up to 4KB of content
// per the Hardware Buddy spec. Sized to 4608B so we can ingest the full
// event line and decide whether to act on or discard the body, instead
// of silently truncating mid-payload. Costs ~3KB of static BSS.
#define LINE_CAP 4608
static char g_line[LINE_CAP];
static size_t g_line_len = 0;

// One-shot edge for `{"evt":"turn"}`. Set by apply_json on a fresh turn
// event, consumed by bridge_poll_turn (the render loop polls it once per
// tick). Plain bool: read/write is atomic on xtensa for a single byte,
// and the worst-case race is a missed or doubled trigger - benign.
static volatile bool g_turn_pending = false;

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
    // TX trace at DEBUG. Xfer was confirmed end-to-end in v0.1.0 ↔ v0.2.0
    // testing; the line is kept (gated by LOG_LOCAL_LEVEL) so future
    // regressions can be diagnosed without recompiling.
    ESP_LOGD(TAG, "tx: %.120s%s", json, n > 120 ? "..." : "");
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
            // Log on the prompt-id transition only (not every heartbeat
            // that keeps carrying the same waiting prompt). Tool name
            // commas would corrupt CSV; replace before formatting.
            char tool[48];
            strncpy(tool, g_state.prompt_tool[0] ? g_state.prompt_tool : "?",
                    sizeof(tool) - 1);
            tool[sizeof(tool) - 1] = 0;
            for (char *c = tool; *c; c++) if (*c == ',') *c = ' ';
            char ex[80];
            snprintf(ex, sizeof(ex), ",,,,%s", tool);
            translog_append("prompt", ex);
        }
    } else {
        g_state.prompt_id[0] = g_state.prompt_tool[0] = g_state.prompt_hint[0] = 0;
        g_last_seen_prompt_id[0] = 0;
        g_prompt_arrived_ms = 0;
    }

    bool was_disconnected = !g_state.connected;
    g_state.last_updated_ms = millis();
    g_state.connected = true;

    // Snapshot fields needed for the translog row before releasing the
    // mutex - keeps the file write off the critical section.
    uint8_t  tl_total = g_state.sessions_total;
    uint8_t  tl_run   = g_state.sessions_running;
    uint8_t  tl_wait  = g_state.sessions_waiting;
    uint32_t tl_tok   = g_state.tokens_today;
    char     tl_prompt[40]; tl_prompt[0] = 0;
    if (g_state.prompt_id[0]) {
        strncpy(tl_prompt, g_state.prompt_tool, sizeof(tl_prompt) - 1);
        tl_prompt[sizeof(tl_prompt) - 1] = 0;
        // Strip commas from the tool name so the CSV row stays valid.
        for (char *c = tl_prompt; *c; c++) if (*c == ',') *c = ' ';
    }
    xSemaphoreGive(g_state_mux);

    g_state_generation++;

    if (was_disconnected) {
        translog_append("conn", "");
    }
    // Throttle the routine "hb" row to one per 10 s. Events (prompt,
    // approve, deny, conn, disconn, stale) write immediately - heartbeats
    // are only valuable as a sparse "things were OK at this timestamp"
    // sample, and the file fopen/fwrite churn isn't free.
    static uint32_t s_last_hb_log_ms = 0;
    uint32_t now_ms = millis();
    if ((uint32_t)(now_ms - s_last_hb_log_ms) >= 10000) {
        s_last_hb_log_ms = now_ms;
        char extras[128];
        snprintf(extras, sizeof(extras), "%u,%u,%u,%lu,%s",
                 tl_total, tl_run, tl_wait, (unsigned long)tl_tok,
                 tl_prompt[0] ? tl_prompt : "");
        translog_append("hb", extras);
    }
    return;
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
    char buf[112];
    snprintf(buf, sizeof(buf),
             "{\"ack\":\"%s\",\"ok\":false,\"n\":0,"
             "\"error\":\"character packs not supported on this hardware\"}",
             cmd);
    send_line(buf);
}

static void ack_status(void)
{
    // Manual printf into a fixed buffer - heap-light and shape-stable
    // for the desktop's parser. See REFERENCE.md "Status response".
    // `max_blk` is the largest contiguous free block (MALLOC_CAP_8BIT);
    // alongside `heap` it lets the desktop spot heap fragmentation over
    // long uptimes without us having to expose a separate command.
    const stats_t *s = stats_get();
    char buf[416];
    int len = snprintf(buf, sizeof(buf),
        "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
        "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
        "\"sys\":{\"up\":%lu,\"heap\":%u,\"max_blk\":%u},"
        "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
        "}}",
        g_petname, g_ownername,
        ble_secure() ? "true" : "false",
        (unsigned long)(millis() / 1000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
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

// One-shot time sync the desktop sends right after connect:
// {"time":[epoch_seconds, tz_offset_seconds]}. We have no hardware RTC,
// so we plug the value straight into the ESP32's software clock via
// settimeofday(); subsequent localtime()/gmtime() calls now reflect the
// host's wall clock. Survives until reboot. The TZ offset goes into the
// POSIX TZ env so localtime() rolls midnight correctly on the device.
static void apply_time_sync(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return;
    cJSON *epoch_v  = cJSON_GetArrayItem(arr, 0);
    cJSON *offset_v = cJSON_GetArrayItem(arr, 1);
    if (!cJSON_IsNumber(epoch_v)) return;

    struct timeval tv = {
        .tv_sec  = (time_t)epoch_v->valuedouble,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);

    if (cJSON_IsNumber(offset_v)) {
        // POSIX TZ: sign is reversed (`UTC-7` for an offset of +7h).
        // The spec offset is in seconds east of UTC.
        int off_s = offset_v->valueint;
        int sign  = (off_s <= 0) ? +1 : -1;   // POSIX-flipped
        int abs_s = (off_s < 0) ? -off_s : off_s;
        int h = abs_s / 3600;
        int m = (abs_s % 3600) / 60;
        char tz[16];
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign > 0 ? '+' : '-', h, m);
        setenv("TZ", tz, 1);
        tzset();
    }
    ESP_LOGI(TAG, "time sync: epoch=%lld tz=%s",
             (long long)tv.tv_sec, getenv("TZ") ? getenv("TZ") : "(unset)");
}

static void apply_json(const char *line)
{
    cJSON *doc = cJSON_Parse(line);
    if (!doc) return;

    // 1. Time-sync (top-level "time" array) - no `cmd`, no `evt`.
    cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(doc, "time");
    if (cJSON_IsArray(time_arr)) {
        apply_time_sync(time_arr);
        cJSON_Delete(doc);
        return;
    }

    // 2. Commands ({"cmd":...}). dispatch() owns the ack.
    if (dispatch(doc)) {
        cJSON_Delete(doc);
        return;
    }

    // 3. Events ({"evt":"turn",...}). The 4KB payload is intentionally
    //    not retained - only the edge is forwarded to the render loop,
    //    which fires a one-shot P_DIZZY animation. cJSON_Delete frees
    //    the parsed content array immediately after this block.
    if (cJSON_IsObject(doc)) {
        const char *evt = json_str(doc, "evt");
        if (evt && strcmp(evt, "turn") == 0) {
            g_turn_pending = true;
            ESP_LOGD(TAG, "turn event (line=%uB)", (unsigned)strlen(line));
        } else if (!evt) {
            // 4. Heartbeat - the implicit catch-all.
            apply_heartbeat(doc);
        }
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
                        // RX trace at DEBUG (kept for OTA diagnostics).
                        // Was INFO during v0.1.0/v0.2.0 char-xfer
                        // testing; lowered once xfer was confirmed.
                        ESP_LOGD(TAG, "rx: %.120s%s", g_line,
                                 strlen(g_line) > 120 ? "..." : "");
                        apply_json(g_line);
                    }
                    g_line_len = 0;
                }
            } else if (g_line_len < LINE_CAP - 1) {
                g_line[g_line_len++] = (char)b;
            } else {
                // Overflow - drop the partial line. LINE_CAP is 4608
                // (above the spec's 4 KB cap on turn events), so this
                // path is unreachable for any spec-compliant peer; if
                // it ever fires, the desktop is sending oversized JSON.
                ESP_LOGW(TAG, "rx line overflow, dropped");
                g_line_len = 0;
            }
        }

        // Mark connection stale after 30s with no heartbeat (per spec).
        bool just_went_stale = false;
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        if (g_state.connected &&
            (millis() - g_state.last_updated_ms) > DATA_TIMEOUT_MS) {
            g_state.connected = false;
            g_state_generation++;
            just_went_stale = true;
            ESP_LOGW(TAG, "heartbeat stale (>30s) - marked disconnected");
        }
        xSemaphoreGive(g_state_mux);
        if (just_went_stale) {
            translog_append("stale", "");
            // Recovery: if BLE is up but heartbeats stopped, the Mac's
            // CoreBluetooth client has desynchronized from the Claude
            // desktop app's view of the link (observed: app shows
            // "Disconnected" but OS still has the GATT handle, Connect
            // button does nothing). Force-tearing down the link from
            // our side gets both stacks to resync from scratch.
            if (ble_connected()) {
                ESP_LOGW(TAG, "stale + link still up - forcing BLE disconnect");
                translog_append("force_disc", "");
                ble_disconnect();
            }
        }

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

    // Translog (SPIFFS): non-fatal if mount fails - logging just no-ops.
    // Mounting here keeps the bridge as the sole owner of the translog
    // lifecycle, since it's also the only producer.
    if (translog_init() == ESP_OK) {
        translog_append("boot", "");
    } else {
        ESP_LOGW(TAG, "translog disabled - SPIFFS mount failed");
    }

    ESP_LOGI(TAG, "bridge up (petname='%s', owner='%s')", g_petname, g_ownername);
}

uint32_t bridge_get_generation(void)
{
    return g_state_generation;
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

bool bridge_poll_turn(void)
{
    bool t = g_turn_pending;
    if (t) g_turn_pending = false;
    return t;
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

    char ex[24];
    snprintf(ex, sizeof(ex), ",,,,%lus", (unsigned long)took_s);
    translog_append(allow ? "approve" : "deny", ex);
}
