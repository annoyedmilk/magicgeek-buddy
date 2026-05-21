// Claude Buddy: device entry point.
//
// Talks to Claude over BLE NUS: bridge.c parses newline-delimited JSON
// heartbeats into tama_state_t, the render loop picks a screen from
// the derived persona state, touch input approves or denies live
// permission prompts and routes to the menu / info overlays.
//
// Boot order (each step's free heap is logged on serial):
//   1. NVS + display + touch
//   2. WiFi (non-blocking; STA if creds saved, captive portal AP otherwise)
//   3. Framebuffer (240x120 banded, two passes per frame)
//   4. BLE NUS (advertises as "Claude-XXXX" with last four MAC hex)
//   5. bridge (RX line buffer, cJSON parser, state machine)
//   6. buddy_task (picks a compose callback by priority, runs fb_frame)
//
// Screen priority (highest first):
//   BLE pairing passkey  >  PROMPT (approval pending)  >  persona state
//   >  BLE paired (waiting for heartbeat)  >  WiFi state

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_http_server.h"

#include "display.h"
#include "gfx.h"
#include "framebuffer.h"
#include "touch_button.h"
#include "storage.h"
#include "wifi_manager.h"
#include "ble_nus.h"
#include "bridge.h"
#include "stats.h"
#include "buddy.h"
#include "ui.h"
#include "ota_server.h"
#include "app_version.h"
#include "debug_log.h"
#include "translog.h"

static const char *TAG = "buddy";

// Transcript scroll on the persona screen. 0 = newest 3 lines shown;
// each TAP (when no prompt is up) bumps this by one to step further
// back into history. Resets to 0 whenever the bridge bumps line_gen
// (= new transcript content arrived). Capped to keep the bottom row
// inside `lines[]` so we never index past n_lines.
static uint8_t  g_transcript_scroll = 0;
static uint16_t g_last_line_gen     = 0;

// Backlight idle-fade. Two-tier:
//   Tier 1 (dim):  no touch AND no new transcript content for 15 min →
//                  drop to 30%. Catches "peer alive but nothing's
//                  happening" - the Claude desktop stays open all day.
//   Tier 2 (off):  P_SLEEP (no peer at all) AND no touch for 5 min →
//                  fade to 0. Burn-in protection on the IPS panel.
// Both timers reset on touch (via on_touch) and on a line_gen bump
// (heartbeat carried a new transcript line). The DIM timer is reset
// only when the peer is alive; SLEEP doesn't keep extending it.
static volatile uint32_t g_last_activity_ms = 0;  // tier-2 (SLEEP) gate
static volatile uint32_t g_last_dim_reset_ms = 0; // tier-1 (idle peer) gate
#define BL_IDLE_FADE_AFTER_MS  (5 * 60 * 1000)   // 5 minutes of P_SLEEP
#define BL_IDLE_DIM_AFTER_MS   (15 * 60 * 1000)  // 15 min idle-with-peer
#define BL_IDLE_FADE_DURATION_MS 2000            // 2-second envelope
#define BL_DIM_PERCENT          0                // fade-to-off target
#define BL_HALF_DIM_PERCENT     30               // tier-1 target

// ─────────────────────────────────────────────────────────────────────
// Touch routing. Priority (highest first):
//   1. UI overlay (menu / info / confirm) - owns gestures when open
//   2. Active permission prompt - TAP approves, DOUBLE denies
//   3. Idle home - TAP scrolls transcript, LONG opens the menu
// (1) is enforced by giving ui_on_* first refusal; if it returns true
// the gesture is "consumed" and (2)/(3) don't see it. LONG with no
// overlay falls through to (3) which calls ui_on_long_press() anyway -
// it just lives at lower priority once the overlay is up.
// ─────────────────────────────────────────────────────────────────────
static void on_touch(touch_event_t ev)
{
    // Any touch resets BOTH idle timers and snaps the backlight back on.
    // Even gestures the UI consumes count as activity - the user is
    // clearly looking at the screen.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    g_last_activity_ms = now;
    g_last_dim_reset_ms = now;
    display_set_backlight(100);

    switch (ev) {
    case TOUCH_EVENT_TAP:
        if (ui_on_tap()) return;
        {
            tama_state_t s; bridge_get_state(&s);
            if (s.prompt_id[0]) {
                ESP_LOGI(TAG, "[touch] TAP -> APPROVE (%s)", s.prompt_tool);
                bridge_send_permission(true);
            } else if (s.n_lines > 3) {
                // No prompt: scroll back through transcript history.
                // Cap so the visible window stays inside `lines[]`;
                // wrap to 0 (newest) once we've hit the oldest entry.
                uint8_t max_off = (uint8_t)(s.n_lines - 3);
                g_transcript_scroll = (g_transcript_scroll >= max_off)
                                       ? 0
                                       : g_transcript_scroll + 1;
                ESP_LOGI(TAG, "[touch] TAP -> transcript scroll %u/%u",
                         g_transcript_scroll, max_off);
            } else {
                ESP_LOGI(TAG, "[touch] TAP -> idle (no prompt, no scrollable transcript)");
            }
        }
        break;
    case TOUCH_EVENT_DOUBLE_TAP:
        if (ui_on_double_tap()) return;
        {
            tama_state_t s; bridge_get_state(&s);
            if (s.prompt_id[0]) {
                ESP_LOGI(TAG, "[touch] DOUBLE -> DENY (%s)", s.prompt_tool);
                bridge_send_permission(false);
            } else {
                ESP_LOGI(TAG, "[touch] DOUBLE -> idle (no overlay, no prompt)");
            }
        }
        break;
    case TOUCH_EVENT_LONG_PRESS:
        // LONG always means "open menu / go back" - always consumed by UI.
        ui_on_long_press();
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Compose callbacks. Run twice per fb_frame() (once per band); must be
// idempotent. Use full-screen Y coords (0..239).
// ─────────────────────────────────────────────────────────────────────

// `bg` is both the clear color AND the glyph-cell background under the
// title text - pass COLOR_BLACK on the persona screen so the ASCII pet
// sits on a uniform black field (the species' overlay particles use
// BUDDY_BG=black, and a charcoal screen leaves a visible halo around
// each glyph). Other screens stay on the warm Claude charcoal.
static void compose_header(uint16_t band, const char *title, uint16_t bg)
{
    fb_clear(bg);
    gfx_fill_rect(0, 0, DISPLAY_WIDTH, 6, band);
    gfx_text_center(16, title, COLOR_CLAUDE_PAPER, bg, 1);
}

static void compose_passkey(void *ctx)
{
    uint32_t pk = (uint32_t)(uintptr_t)ctx;
    char buf[8];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)pk);
    compose_header(COLOR_CLAUDE_CORAL, "Claude Buddy", COLOR_CLAUDE_BG);
    gfx_text_center(48, "PAIR ON DESKTOP", COLOR_CLAUDE_CORAL, COLOR_CLAUDE_BG, 1);
    gfx_text_center(76, buf, COLOR_CLAUDE_PAPER, COLOR_CLAUDE_BG, 4);
    gfx_text_center(150, "enter this code on", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(166, "the Claude desktop app", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    ui_compose_overlay();
}

// PROMPT screen - highest priority once paired. tool + truncated hint;
// big colored buttons signaling TAP=approve / DOUBLE=deny. The user
// should be able to act without reading the rest of the UI.
static void compose_prompt(void *ctx)
{
    const tama_state_t *s = (const tama_state_t *)ctx;
    compose_header(COLOR_CLAUDE_CORAL, "APPROVAL", COLOR_CLAUDE_BG);

    gfx_text_center(40, "Claude wants to run:", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    // tool at scale 2 if short enough, else scale 1 - same logic as the
    // source's drawApproval for readability.
    int tool_scale = (strlen(s->prompt_tool) <= 14) ? 2 : 1;
    gfx_text_center(62, s->prompt_tool, COLOR_CLAUDE_PAPER, COLOR_CLAUDE_BG, tool_scale);

    // hint truncated to 26 chars/line, two-line wrap
    char l1[32], l2[32];
    snprintf(l1, sizeof(l1), "%.26s", s->prompt_hint);
    snprintf(l2, sizeof(l2), "%.26s", strlen(s->prompt_hint) > 26 ? s->prompt_hint + 26 : "");
    gfx_text_center(98,  l1, COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(112, l2, COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);

    // Action buttons. Each rect is 104 px wide. gfx_text_center spans
    // the full 240 px screen, so naive use lands all four labels in
    // the dark gap between buttons. Compute per-button centers manually.
    //   green:  x=8..112,  center=60
    //   red:    x=128..232, center=180
    // 8x8 glyph cell, so per-glyph width = 8 * scale.
    const int GBTN_CX = 60, RBTN_CX = 180;
    #define BTN_LABEL(cx, y, s, scale, fg, bg) do { \
        int _w = (int)strlen(s) * 8 * (scale); \
        gfx_text((cx) - _w / 2, (y), (s), (fg), (bg), (scale)); \
    } while (0)

    gfx_fill_rect(8, 170, 104, 50, COLOR_GREEN);
    BTN_LABEL(GBTN_CX, 180, "TAP",     2, COLOR_CLAUDE_BG, COLOR_GREEN);
    BTN_LABEL(GBTN_CX, 202, "approve", 1, COLOR_CLAUDE_BG, COLOR_GREEN);

    gfx_fill_rect(128, 170, 104, 50, COLOR_RED);
    BTN_LABEL(RBTN_CX, 180, "DOUBLE",  2, COLOR_CLAUDE_PAPER, COLOR_RED);
    BTN_LABEL(RBTN_CX, 202, "deny",    1, COLOR_CLAUDE_PAPER, COLOR_RED);

    #undef BTN_LABEL
    ui_compose_overlay();
}

// Per-state visual descriptor: short label, accent color (used by the
// persona-screen title), and a one-line subtext shown when the desktop
// hasn't sent its own `msg` for the current state.
typedef struct { const char *label; uint16_t color; const char *sub; } persona_view_t;

static persona_view_t persona_view(persona_state_t p, const tama_state_t *s)
{
    (void)s;
    switch (p) {
    case P_SLEEP:     return (persona_view_t){ "SLEEP",     COLOR_CLAUDE_DIM,   "no claude connected" };
    case P_IDLE:      return (persona_view_t){ "IDLE",      COLOR_CLAUDE_PAPER, "watching..." };
    case P_BUSY:      return (persona_view_t){ "BUSY",      COLOR_YELLOW,       "sessions running" };
    case P_ATTENTION: return (persona_view_t){ "ATTENTION", COLOR_CLAUDE_CORAL, "approval pending" };
    case P_CELEBRATE: return (persona_view_t){ "CELEBRATE", COLOR_GREEN,        "task complete!" };
    case P_DIZZY:     return (persona_view_t){ "TURN",      COLOR_CYAN,         "thought delivered" };
    }
    return (persona_view_t){ "?", COLOR_CLAUDE_DIM, "" };
}

// Composite for the persona screen. Layout (y, top-to-bottom):
//   4       "Claude Buddy" title (persona color, scale 1)
//   30      overlay-particle band (Zzz / ! / hearts above the body)
//   46-126  ASCII pet body (scale 2, 5 rows × 16 px)
//   130     transcript line 1 (scroll position offset)
//   140     transcript line 2
//   150     transcript line 3
//   162     one-line msg from desktop
//   174     session counts (total/running/waiting)
//   186     gauges row: mood hearts + energy bars + Lv N badge
//   198     approvals / denials counter
//   210     tokens today
//   222-228 fed-progress bar (10 pips, filled = coral, hollow = dim)
static void compose_persona(void *ctx)
{
    const tama_state_t *s = (const tama_state_t *)ctx;
    persona_state_t p = bridge_derive(s);
    persona_view_t v = persona_view(p, s);

    // Reset scroll back to newest if the bridge dropped a new transcript
    // line. Keeps the user from being stuck in history when fresh
    // content arrives.
    if (s->line_gen != g_last_line_gen) {
        g_last_line_gen = s->line_gen;
        g_transcript_scroll = 0;
    }

    // Pure-black canvas (no header band) so the pet's BUDDY_BG=0x0000
    // glyphs and overlay particles blend seamlessly. Title sits in the
    // persona's accent color as the only top-of-screen chrome.
    fb_clear(COLOR_BLACK);
    gfx_text_center(4, "Claude Buddy", v.color, COLOR_BLACK, 1);

    // ASCII pet (GIF pack support was removed - see CLAUDE.md).
    buddy_render(p);

    // ─── Transcript (up to 3 rows, newest at bottom, scrollable via TAP) ─
    // The bridge keeps the most-recent `n_lines` of entries[] in
    // s->lines[], newest at index 0 per REFERENCE.md. We render bottom-
    // up so the eye lands on the freshest line; scrolling steps further
    // back into history. Entries longer than the 30-column width word-
    // wrap onto a second row (consuming one of the 3 vertical slots).
    // The newest visible line gets full PAPER; older lines dim.
    {
        const int row_y[3] = { 130, 140, 150 };  // bottom-up: [2]=newest
        const int max_cols = 30;                  // 240px / 8px glyph
        int rows_left = 3;                        // available row slots
        int row = 2;                              // bottom row first
        int idx = (int)g_transcript_scroll;       // newest visible

        while (rows_left > 0 && idx < (int)s->n_lines) {
            const char *src = s->lines[idx];
            int n = (int)strlen(src);

            // Find an optional break point if this entry overflows one row.
            // Prefer the last space before column 30; fall back to a hard
            // break if the entry has no spaces (rare; e.g. a long token).
            int line_len = (n > max_cols) ? max_cols : n;
            int wrap_at  = 0;     // bytes consumed from src on this entry
            int part2_len = 0;
            char part1[max_cols + 1];
            char part2[max_cols + 1];
            part2[0] = 0;

            if (n <= max_cols) {
                memcpy(part1, src, (size_t)n);
                part1[n] = 0;
                wrap_at = n;
            } else {
                // Look back from col 30 for a space to wrap on.
                int br = max_cols;
                while (br > 8 && src[br] != ' ') br--;
                if (src[br] != ' ') br = max_cols;  // no space found
                memcpy(part1, src, (size_t)br);
                part1[br] = 0;
                wrap_at = br;
                // Skip the space we wrapped on; clip second row to 30.
                int s2 = (src[br] == ' ') ? br + 1 : br;
                part2_len = n - s2;
                if (part2_len > max_cols) part2_len = max_cols;
                memcpy(part2, src + s2, (size_t)part2_len);
                part2[part2_len] = 0;
            }
            (void)line_len;
            (void)wrap_at;

            // Color: only the newest entry (its bottom row) is PAPER.
            // Everything else is DIM regardless of wrap state.
            bool newest = (idx == (int)g_transcript_scroll);
            uint16_t col_new = COLOR_CLAUDE_PAPER;
            uint16_t col_old = COLOR_CLAUDE_DIM;

            if (part2_len > 0 && rows_left >= 2) {
                // 2-row entry: continuation drawn below, head above.
                gfx_text_center(row_y[row],     part2, newest ? col_new : col_old, COLOR_BLACK, 1);
                gfx_text_center(row_y[row - 1], part1, col_old,                    COLOR_BLACK, 1);
                row       -= 2;
                rows_left -= 2;
            } else {
                // Single row: either the entry fit, OR we only had 1 slot
                // left and we drop the continuation (newest still wins).
                gfx_text_center(row_y[row], part1, newest ? col_new : col_old, COLOR_BLACK, 1);
                row--;
                rows_left--;
            }
            idx++;
        }

        // Scroll indicator "-N" in the corner when not at newest.
        if (g_transcript_scroll > 0) {
            char ind[6];
            snprintf(ind, sizeof(ind), "-%u", g_transcript_scroll);
            gfx_text(DISPLAY_WIDTH - 24, 150, ind,
                     COLOR_CLAUDE_CORAL, COLOR_BLACK, 1);
        }
    }

    // ─── One-line msg (persona-derived: "watching..." / "approval pending" / etc.)
    if (s->msg[0]) {
        gfx_text_center(162, s->msg, COLOR_CLAUDE_PAPER, COLOR_BLACK, 1);
    }

    // ─── Session counts ────────────────────────────────────────────
    char counts[32];
    snprintf(counts, sizeof(counts), "%u total  %u run  %u wait",
             s->sessions_total, s->sessions_running, s->sessions_waiting);
    gfx_text_center(174, counts, COLOR_CLAUDE_DIM, COLOR_BLACK, 1);

    // ─── Gauges row: mood ♥♥♥♡  energy ▍▍▍▌▌  Lv N ─────────────────
    // No circle/heart primitives in gfx.h, so each pip is a 6×6 filled
    // rect. Mood (0-4) uses 4 heart-coral pips; energy (0-5) uses 5
    // taller cyan pips; level renders as text on the right.
    {
        const int gy = 186;
        // Mood: 4 pips at x=20..62
        uint8_t mood = stats_mood_tier();
        uint16_t mood_col = (mood >= 3) ? COLOR_CLAUDE_CORAL
                          : (mood >= 2) ? COLOR_YELLOW
                          : COLOR_CLAUDE_DIM;
        for (int i = 0; i < 4; i++) {
            int x = 20 + i * 10;
            if (i < mood) gfx_fill_rect(x, gy, 6, 6, mood_col);
            else          gfx_fill_rect(x, gy, 6, 6, COLOR_CLAUDE_DIM / 4);
        }
        // Energy: 5 pips at x=80..136
        uint8_t energy = stats_energy_tier();
        uint16_t en_col = (energy >= 4) ? COLOR_CYAN
                       : (energy >= 2) ? COLOR_YELLOW
                       : COLOR_CLAUDE_CORAL;
        for (int i = 0; i < 5; i++) {
            int x = 80 + i * 12;
            if (i < energy) gfx_fill_rect(x, gy - 1, 8, 8, en_col);
            else            gfx_fill_rect(x, gy - 1, 8, 8, COLOR_CLAUDE_DIM / 4);
        }
        // Lv badge: right-justified text
        const stats_t *st = stats_get();
        char lvbuf[12];
        snprintf(lvbuf, sizeof(lvbuf), "Lv %u", st->level);
        gfx_text(DISPLAY_WIDTH - 56, gy, lvbuf,
                 COLOR_CLAUDE_PAPER, COLOR_BLACK, 1);
    }

    // ─── Approvals / denials ───────────────────────────────────────
    {
        const stats_t *st = stats_get();
        char line[32];
        snprintf(line, sizeof(line), "%u OK  /  %u no",
                 st->approvals, st->denials);
        gfx_text_center(198, line, COLOR_CLAUDE_PAPER, COLOR_BLACK, 1);
    }

    // ─── Tokens today ──────────────────────────────────────────────
    {
        char tk[24];
        if (s->tokens_today >= 1000000) {
            snprintf(tk, sizeof(tk), "%lu.%luM tokens today",
                (unsigned long)(s->tokens_today/1000000),
                (unsigned long)((s->tokens_today/100000)%10));
        } else if (s->tokens_today >= 1000) {
            snprintf(tk, sizeof(tk), "%lu.%luK tokens today",
                (unsigned long)(s->tokens_today/1000),
                (unsigned long)((s->tokens_today/100)%10));
        } else {
            snprintf(tk, sizeof(tk), "%lu tokens today",
                (unsigned long)s->tokens_today);
        }
        gfx_text_center(210, tk, COLOR_CLAUDE_DIM, COLOR_BLACK, 1);
    }

    // ─── Fed-progress bar (10 pips at the very bottom) ─────────────
    uint8_t fed = stats_fed_progress();
    int barW = 100, barX = (DISPLAY_WIDTH - barW) / 2, barY = 222;
    for (int i = 0; i < 10; i++) {
        int px = barX + i * 10;
        uint16_t c = (i < fed) ? COLOR_CLAUDE_CORAL : COLOR_CLAUDE_DIM;
        gfx_fill_rect(px + 1, barY, 8, 6, c);
    }

    ui_compose_overlay();
}

// BLE paired but no heartbeat yet (or stale) - bridge in a transitional
// state. Tells the user to open the Hardware Buddy window.
static void compose_ble_waiting(void *ctx)
{
    (void)ctx;
    compose_header(COLOR_GREEN, "Claude Buddy", COLOR_CLAUDE_BG);
    gfx_text_center(58,  "PAIRED", COLOR_GREEN, COLOR_CLAUDE_BG, 3);
    gfx_text_center(108, "Claude desktop linked", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(124, "waiting for heartbeat", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(160, "open Claude desktop:", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(176, "Developer > Hardware", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(190, "Buddy > Connect", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    ui_compose_overlay();
}

static void compose_wifi_online(void *ctx)
{
    const char *ip = (const char *)ctx;
    compose_header(COLOR_CLAUDE_CORAL, "Claude Buddy", COLOR_CLAUDE_BG);
    gfx_text_center(48, "WAITING FOR PAIR", COLOR_CLAUDE_CORAL, COLOR_CLAUDE_BG, 1);
    gfx_text_center(74, "pair via Bluetooth", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(96, "device IP", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(112, ip, COLOR_CLAUDE_PAPER, COLOR_CLAUDE_BG, 2);
    gfx_text_center(150, "in Claude desktop:", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(166, "Developer > Hardware", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(180, "Buddy > Connect", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    ui_compose_overlay();
}

static void compose_setup(void *ctx)
{
    const char *ap = (const char *)ctx;
    compose_header(COLOR_CLAUDE_CORAL, "Claude Buddy", COLOR_CLAUDE_BG);
    gfx_text_center(64, "WIFI SETUP", COLOR_CLAUDE_CORAL, COLOR_CLAUDE_BG, 2);
    gfx_text_center(96, "join wifi network", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(112, ap, COLOR_CLAUDE_PAPER, COLOR_CLAUDE_BG, 2);
    gfx_text_center(150, "then open", COLOR_CLAUDE_DIM, COLOR_CLAUDE_BG, 1);
    gfx_text_center(166, "192.168.4.1", COLOR_CLAUDE_PAPER, COLOR_CLAUDE_BG, 2);
    ui_compose_overlay();
}

// ─────────────────────────────────────────────────────────────────────
// Screen selection by priority.
// ─────────────────────────────────────────────────────────────────────
typedef enum {
    SCR_PASSKEY,
    SCR_PROMPT,
    SCR_PERSONA,
    SCR_BLE_WAITING,
    SCR_WIFI_ONLINE,
    SCR_WIFI_PORTAL,
} screen_t;

static void buddy_task(void *arg)
{
    // OTA /ota gets registered once the shared httpd is up (either AP
    // captive portal OR STA-online). This task already polls WiFi state
    // and is the natural place to do one-shot post-WiFi setup.
    bool ota_mounted = false;
    // Rollback safety: cancel the pending verify mark only after the
    // first ~3 seconds of running so a build that boot-loops gets
    // rolled back by the bootloader. We mark valid once we've crossed
    // that threshold AND WiFi is up (proof the network stack works).
    uint32_t boot_t0_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool     marked_valid = false;
    // Seed both idle clocks so we don't dim during the first 5/15 min of
    // uptime (the user just plugged it in and is watching it).
    g_last_activity_ms = boot_t0_ms;
    g_last_dim_reset_ms = boot_t0_ms;

    screen_t last = (screen_t)-1;
    uint32_t last_pk = 0;
    uint16_t last_line_gen = 0xFFFF;
    persona_state_t last_persona = (persona_state_t)-1;
    char last_prompt_id[40] = "";

    // One-shot CELEBRATE override on level-up. Holds for ~3s like the
    // source's triggerOneShot(P_CELEBRATE, 3000).
    uint32_t celebrate_until_ms = 0;
    // One-shot DIZZY override when the desktop fires a turn event.
    // 1.5s is long enough for the user to notice (a couple of animation
    // ticks at 5fps) but short enough not to mask the underlying persona.
    uint32_t dizzy_until_ms = 0;

    // Animation cadence: 5 fps on the persona screen, matching the
    // source's TICK_MS=200. Other screens redraw only on state change.
    uint32_t next_anim_ms = 0;

    while (1) {
        // Snapshot the bridge state ONCE per tick so we render a coherent
        // frame (otherwise compose() could see partly-updated fields).
        tama_state_t s;
        bridge_get_state(&s);

        // Edge-trigger the celebrate one-shot on a level-up event.
        if (stats_poll_level_up()) {
            celebrate_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
            ESP_LOGI(TAG, "level up! → CELEBRATE for 3s");
        }
        bool celebrating = (int32_t)(celebrate_until_ms
            - (uint32_t)(esp_timer_get_time() / 1000)) > 0;

        // Edge-trigger the dizzy one-shot on a turn event from the bridge.
        // Lower priority than CELEBRATE: if both are active we keep the
        // level-up animation since it's the rarer, more meaningful signal.
        if (bridge_poll_turn()) {
            dizzy_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 1500;
            ESP_LOGI(TAG, "turn event → DIZZY for 1.5s");
        }
        bool dizzy = !celebrating && (int32_t)(dizzy_until_ms
            - (uint32_t)(esp_timer_get_time() / 1000)) > 0;

        uint32_t pk = ble_passkey();
        screen_t scr;
        if (pk != 0)                        scr = SCR_PASSKEY;
        else if (s.prompt_id[0])            scr = SCR_PROMPT;
        else if (bridge_data_alive())       scr = SCR_PERSONA;
        else if (ble_connected())           scr = SCR_BLE_WAITING;
        else {
            wifi_state_t wst = wifi_manager_state();
            scr = (wst == WIFI_STATE_ONLINE) ? SCR_WIFI_ONLINE : SCR_WIFI_PORTAL;
        }

        // Redraw triggers: screen changed, passkey digits changed, new
        // prompt id, persona changed, transcript content changed (for
        // future entries[] rendering - kept now so msg/counts refresh).
        // Overrides on the derived persona (highest priority first):
        //   CELEBRATE (level-up)  >  DIZZY (turn event)  >  derived
        persona_state_t persona = celebrating ? P_CELEBRATE
                                : dizzy       ? P_DIZZY
                                              : bridge_derive(&s);

        // Animation tick on the persona screen: re-render every 200ms
        // so the pet actually moves. buddy_tick_advance() runs OUTSIDE
        // fb_frame so per-band re-compose stays idempotent.
        // ALSO: the reset-confirm overlay shows a 1-Hz countdown; force
        // a periodic redraw while it's up so the seconds tick down.
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool anim_due = ((scr == SCR_PERSONA) || (ui_get_state() == UI_RESET_CONFIRM))
            && (int32_t)(now_ms - next_anim_ms) >= 0;

        bool need_redraw = (scr != last)
            || (scr == SCR_PASSKEY && pk != last_pk)
            || (scr == SCR_PROMPT  && strcmp(s.prompt_id, last_prompt_id) != 0)
            || (scr == SCR_PERSONA && (persona != last_persona || s.line_gen != last_line_gen))
            // When the celebrate/dizzy timers expire while on the persona
            // screen, the derived persona may equal the previous (e.g.
            // IDLE → IDLE) so the normal trigger misses. Force a redraw
            // when either override *transitions* (poll edge).
            || (scr == SCR_PERSONA && (last_persona == P_CELEBRATE) != celebrating)
            || (scr == SCR_PERSONA && (last_persona == P_DIZZY)     != dizzy)
            || anim_due
            // The overlay state changed (menu opened, item moved, etc.).
            // ui_poll_dirty() consumes the flag so it edge-triggers once.
            || ui_poll_dirty();

        if (anim_due) {
            // Only advance pet animation when the persona screen is up;
            // the reset-confirm tick just wants a redraw for its
            // countdown text.
            if (scr == SCR_PERSONA) buddy_tick_advance();
            next_anim_ms = now_ms + 200;
        }

        if (need_redraw) {
            switch (scr) {
            case SCR_PASSKEY:
                fb_frame(compose_passkey, (void *)(uintptr_t)pk);
                break;
            case SCR_PROMPT:
                fb_frame(compose_prompt, &s);
                break;
            case SCR_PERSONA:
                fb_frame(compose_persona, &s);
                break;
            case SCR_BLE_WAITING:
                fb_frame(compose_ble_waiting, NULL);
                break;
            case SCR_WIFI_ONLINE: {
                char ip[16];
                wifi_manager_get_ip_str(ip, sizeof(ip));
                fb_frame(compose_wifi_online, ip);
                break;
            }
            case SCR_WIFI_PORTAL: {
                char ap[20];
                wifi_manager_get_ap_name(ap, sizeof(ap));
                fb_frame(compose_setup, ap);
                break;
            }
            }
            last = scr;
            last_pk = pk;
            last_persona = persona;
            last_line_gen = s.line_gen;
            strncpy(last_prompt_id, s.prompt_id, sizeof(last_prompt_id) - 1);
            last_prompt_id[sizeof(last_prompt_id) - 1] = 0;
        }

        // One-shot OTA + /debug mount: as soon as the shared httpd
        // exists, attach the routes. Works in both AP (captive portal
        // still running) and STA mode - recovery via captive AP if a
        // flashed build never gets STA online.
        if (!ota_mounted && wifi_manager_get_httpd() != NULL) {
            if (ota_start() == ESP_OK) {
                ota_mounted = true;
                ESP_LOGI(TAG, "OTA endpoint ready: http://<device-ip>/ota");
                if (debug_log_register_routes(
                        (httpd_handle_t)wifi_manager_get_httpd()) == ESP_OK) {
                    ESP_LOGI(TAG, "debug endpoint ready: http://<device-ip>/debug");
                } else {
                    ESP_LOGW(TAG, "debug endpoint registration failed");
                }
            }
        }

        // Rollback cancel: after 3 s of running with WiFi up, the build
        // is healthy enough that we let it commit. Earlier and a hang
        // here wouldn't roll back; later and a clean boot looks unsafe.
        if (!marked_valid
            && (now_ms - boot_t0_ms) >= 3000
            && wifi_manager_get_httpd() != NULL) {
            ota_mark_valid();
            marked_valid = true;
        }

        // ─── Idle backlight fade (burn-in protection) ─────────────────
        // The ST7789V IPS panel is prone to image retention on long
        // static fields. Two tiers:
        //   tier 1: peer alive but nothing happening for 15 min → 30%
        //   tier 2: P_SLEEP (no peer) for 5 min → 0%
        // Both reset on touch (via on_touch) and on a new transcript
        // line (line_gen bump). Tier 2 also resets on any non-SLEEP
        // persona state since "running sessions" is real activity.
        {
            static uint16_t last_bl_line_gen = 0;
            bool active_ui   = (ui_get_state() != UI_NORMAL);
            bool peer_alive  = (scr != SCR_PERSONA) || (persona != P_SLEEP);
            bool new_content = (s.line_gen != last_bl_line_gen);
            last_bl_line_gen = s.line_gen;

            // Tier 2 (SLEEP→off): resets on any UI activity OR any
            // non-SLEEP persona. Unchanged semantics from v0.2.0.
            if (active_ui || peer_alive) {
                g_last_activity_ms = now_ms;
            }
            // Tier 1 (idle-with-peer→dim): resets only on REAL activity
            // - new transcript content, touch, or active overlay.
            // Steady-state "BUSY but the same heartbeat" doesn't count.
            if (active_ui || new_content) {
                g_last_dim_reset_ms = now_ms;
            }

            uint32_t idle_for_fade = now_ms - g_last_activity_ms;
            uint32_t idle_for_dim  = now_ms - g_last_dim_reset_ms;
            uint8_t  target;

            if (idle_for_fade >= BL_IDLE_FADE_AFTER_MS) {
                // Tier 2 wins: ramp 100 → BL_DIM_PERCENT (off).
                uint32_t into_fade = idle_for_fade - BL_IDLE_FADE_AFTER_MS;
                if (into_fade >= BL_IDLE_FADE_DURATION_MS) {
                    target = BL_DIM_PERCENT;
                } else {
                    uint32_t span = 100u - BL_DIM_PERCENT;
                    uint32_t drop = (span * into_fade) / BL_IDLE_FADE_DURATION_MS;
                    target = (uint8_t)(100u - drop);
                }
            } else if (idle_for_dim >= BL_IDLE_DIM_AFTER_MS) {
                // Tier 1: ramp 100 → BL_HALF_DIM_PERCENT (30%).
                uint32_t into_dim = idle_for_dim - BL_IDLE_DIM_AFTER_MS;
                if (into_dim >= BL_IDLE_FADE_DURATION_MS) {
                    target = BL_HALF_DIM_PERCENT;
                } else {
                    uint32_t span = 100u - BL_HALF_DIM_PERCENT;
                    uint32_t drop = (span * into_dim) / BL_IDLE_FADE_DURATION_MS;
                    target = (uint8_t)(100u - drop);
                }
            } else {
                target = 100;
            }
            static uint8_t s_last_target = 100;
            if (target != s_last_target) {
                display_set_backlight(target);
                s_last_target = target;
            }
        }

        // 100 ms tick - gives the persona screen ~5 fps animation room
        // and keeps state-change response under a quarter-second. The
        // watchdog/idle tasks get plenty of yield time at this cadence.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    // Build fingerprint - bumped on each OTA so we can tell from the
    // boot log (or /debug after WiFi is up) which build is running.
    ESP_LOGI(TAG, "Claude Buddy boot, firmware %s", APP_VERSION);

    // Start the in-RAM log ring BEFORE everything else so we capture
    // the boot-log lines that follow. ESP_LOGI still goes to UART; the
    // ring just tees it. Safe to call before NVS/WiFi/etc.
    debug_log_init();

    ESP_ERROR_CHECK(storage_init_nvs());
    stats_load();
    settings_load();

    display_init();
    display_fill_color(COLOR_CLAUDE_BG);

    touch_button_init();
    touch_button_start(on_touch);

    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_autostart();

    ESP_LOGI(TAG, "free heap before FB: %u", (unsigned)esp_get_free_heap_size());
    if (!fb_init()) {
        ESP_LOGE(TAG, "framebuffer init failed");
        return;
    }
    ESP_LOGI(TAG, "free heap after  FB: %u", (unsigned)esp_get_free_heap_size());

    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BT);
        char name[20];
        snprintf(name, sizeof(name), "Claude-%02X%02X", mac[4], mac[5]);
        ble_init(name);
    }
    ESP_LOGI(TAG, "free heap after BLE: %u", (unsigned)esp_get_free_heap_size());

    // Bridge AFTER BLE - it sends acks via ble_write().
    bridge_init();
    ESP_LOGI(TAG, "free heap after bridge: %u", (unsigned)esp_get_free_heap_size());

    // ASCII pets: registry + restore last-used species from NVS.
    buddy_init();

    // UI overlays (menu/info/factory-reset confirm). No I/O at init -
    // just sets the overlay state to UI_NORMAL.
    ui_init();

    xTaskCreate(buddy_task, "buddy", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Boot complete - display + FB + touch + WiFi + BLE + bridge up.");
}
