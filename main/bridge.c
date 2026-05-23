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

// 45s: REFERENCE.md says "stale if no heartbeat in 30s", but empirical
// captures (see commit history) show the macOS desktop legitimately
// pauses heartbeats for ~32s on its own (display sleep transitions,
// app backgrounding). A 30s threshold tore down healthy links; 45s
// covers the observed worst-case pause with margin while still firing
// quickly enough to recover the v0.3.1 CoreBluetooth desync scenario
// (which manifests as minutes of silence, not seconds).
#define DATA_TIMEOUT_MS 45000

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
// Set when the in-progress line exceeded LINE_CAP. We then discard every
// subsequent byte until the next '\n' so we don't restart line assembly
// in the middle of a garbage payload (the v0.3.1 path reset g_line_len
// to 0 and accepted the next char as the start of a "new" line, which
// could corrupt the next several reads). Cleared at the newline.
static bool g_line_dropping = false;

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
        }
    } else {
        g_state.prompt_id[0] = g_state.prompt_tool[0] = g_state.prompt_hint[0] = 0;
        g_last_seen_prompt_id[0] = 0;
        g_prompt_arrived_ms = 0;
    }

    g_state.last_updated_ms = millis();
    g_state.connected = true;
    xSemaphoreGive(g_state_mux);

    g_state_generation++;
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

// Spec-defined error tokens (see Espressif esp_desktop_buddy reference).
// The desktop branches on these strings, so they must match exactly.
#define ERR_UNSUPPORTED      "unsupported"
#define ERR_UNKNOWN_COMMAND  "unknown_command"
#define ERR_INVALID_REQUEST  "invalid_request"

static void ack_err(const char *cmd, const char *token)
{
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ack\":\"%s\",\"ok\":false,\"n\":0,\"error\":\"%s\"}",
             cmd, token);
    send_line(buf);
}

// Reject a folder-push step. GIF character packs are not supported on
// this hardware (AnimatedGIF LZW init wouldn't fit the heap budget
// without smashing the bridge task stack). Uses the spec "unsupported"
// token so the desktop's Hardware Buddy toast can branch on it.
static void ack_unsupported(const char *cmd) { ack_err(cmd, ERR_UNSUPPORTED); }

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
        if (!n) { ack_err("name", ERR_INVALID_REQUEST); return true; }
        safe_copy(g_petname, sizeof(g_petname), n);
        storage_set_str("petname", g_petname);
        ack_ok("name");
        return true;
    }
    if (strcmp(cmd, "owner") == 0) {
        const char *n = json_str(doc, "name");
        if (!n) { ack_err("owner", ERR_INVALID_REQUEST); return true; }
        safe_copy(g_ownername, sizeof(g_ownername), n);
        storage_set_str("owner", g_ownername);
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
    // char_begin up-front with the spec "unsupported" token so the
    // desktop's Hardware Buddy toast can branch on it. The subsequent
    // file/chunk/file_end/char_end can't happen if char_begin is
    // rejected, but ack them defensively in case a future desktop build
    // sends them anyway.
    if (strcmp(cmd, "char_begin") == 0 ||
        strcmp(cmd, "file")       == 0 ||
        strcmp(cmd, "chunk")      == 0 ||
        strcmp(cmd, "file_end")   == 0 ||
        strcmp(cmd, "char_end")   == 0) {
        ack_unsupported(cmd);
        return true;
    }

    // Inbound `permission` command from the desktop: the protocol uses
    // the same token for our outbound permission reply. Match the
    // Espressif reference and swallow it silently rather than letting
    // it fall through to apply_heartbeat (which would corrupt
    // tama_state_t by treating the command shape as a malformed hb).
    if (strcmp(cmd, "permission") == 0) return true;

    // Any other command: spec says ack with the unknown_command token
    // so the desktop sees explicit rejection instead of timing out.
    // Without this, the line falls through to apply_heartbeat as a
    // {cmd:...} object and silently zeros tama_state_t.
    ack_err(cmd, ERR_UNKNOWN_COMMAND);
    return true;
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

static void apply_json(const char *line, size_t line_len)
{
    // cJSON_ParseWithLength avoids a second strlen pass over a 4 KB turn
    // event and tolerates embedded NULs in malformed input.
    cJSON *doc = cJSON_ParseWithLength(line, line_len);
    if (!doc) return;

    // 1. Time-sync: any top-level "time" key means it's not a heartbeat.
    // Guard on key existence, not cJSON_IsArray: if the desktop sends
    // {"time": <integer>} (non-array), the array check fails silently
    // and the message falls through to apply_heartbeat, which sets
    // connected=true and resets last_updated_ms — a false 45 s reprieve
    // that creates an exact stale-fire cycle on every reconnect.
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(doc, "time");
    if (time_item) {
        if (cJSON_IsArray(time_item)) apply_time_sync(time_item);
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
    bool was_ble_connected = false;

    while (1) {
        // Reset the stale window on every fresh BLE connection so the
        // desktop gets a clean 45 s slot to send its first heartbeat.
        // Without this, a reconnect after a long idle period would find
        // last_updated_ms already > 45 s old and fire the stale ~50 ms
        // after connect — before the passkey screen has time to show.
        bool now_ble_connected = ble_connected();
        if (!was_ble_connected && now_ble_connected) {
            xSemaphoreTake(g_state_mux, portMAX_DELAY);
            g_state.last_updated_ms = millis();
            xSemaphoreGive(g_state_mux);
            ESP_LOGI(TAG, "BLE reconnect - stale window reset");
        }
        was_ble_connected = now_ble_connected;

        // Drain everything the BLE stack has buffered. Most heartbeats
        // arrive as several MTU-sized fragments - accumulate until '\n'.
        while (ble_available()) {
            int b = ble_read();
            if (b < 0) break;
            if (b == '\n' || b == '\r') {
                if (!g_line_dropping && g_line_len > 0) {
                    g_line[g_line_len] = 0;
                    if (g_line[0] == '{') {
                        // RX trace at DEBUG (compiled out at INFO level,
                        // so never reaches /debug/log). Re-enabled to
                        // INFO during diagnosis only; the stale-loop bug
                        // was the macOS desktop pausing heartbeats for
                        // ~32s, fixed by raising DATA_TIMEOUT_MS above.
                        ESP_LOGD(TAG, "rx: %.120s%s", g_line,
                                 g_line_len > 120 ? "..." : "");
                        apply_json(g_line, g_line_len);
                    }
                }
                g_line_len = 0;
                g_line_dropping = false;
            } else if (g_line_dropping) {
                // Already dropping this oversized line - keep eating
                // bytes until '\n' without resuming line assembly.
            } else if (g_line_len < LINE_CAP - 1) {
                g_line[g_line_len++] = (char)b;
            } else {
                // Overflow - LINE_CAP is 4608 (above the spec's 4 KB
                // cap on turn events), so this path is unreachable for
                // any spec-compliant peer. Latch dropping=true; the
                // rest of the line is discarded so we don't restart
                // line assembly mid-garbage.
                ESP_LOGW(TAG, "rx line overflow, dropping until newline");
                g_line_dropping = true;
            }
        }

        // Mark connection stale after 45s with no heartbeat.
        bool just_went_stale = false;
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        if (g_state.connected &&
            (millis() - g_state.last_updated_ms) > DATA_TIMEOUT_MS) {
            g_state.connected = false;
            g_state_generation++;
            just_went_stale = true;
            ESP_LOGW(TAG, "heartbeat stale (>45s) - marked disconnected");
        }
        xSemaphoreGive(g_state_mux);
        if (just_went_stale) {
            // Recovery: if BLE is up but heartbeats stopped, the Mac's
            // CoreBluetooth client has desynchronized from the Claude
            // desktop app's view of the link (observed: app shows
            // "Disconnected" but OS still has the GATT handle, Connect
            // button does nothing). Force-tearing down the link from
            // our side gets both stacks to resync from scratch.
            // Guard on ble_secure(): if the link is not yet encrypted
            // the device is in the middle of a pairing exchange (passkey
            // on screen). Disconnecting here would kill the pairing —
            // the passkey screen would vanish before the user can read it.
            if (ble_connected() && ble_secure()) {
                ESP_LOGW(TAG, "stale + link still up - forcing BLE disconnect");
                ble_disconnect();
            } else if (ble_connected()) {
                ESP_LOGI(TAG, "stale while pairing in progress - skip disconnect");
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
}
