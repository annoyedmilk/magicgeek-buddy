// UI overlay state machine. Owns the menu / info / reset-confirm panels
// and the touch-routing logic when an overlay is open. The home compose
// runs first in fb_frame(); ui_compose_overlay() then layers a panel on
// top.
#include "ui.h"
#include "gfx.h"
#include "display.h"
#include "framebuffer.h"
#include "buddy.h"
#include "bridge.h"
#include "ble_nus.h"
#include "wifi_manager.h"
#include "stats.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ui";

// ─────────────────────────────────────────────────────────────────────
// Menu definition. Single flat list at the top level - submenus would
// need an extra layer of selection state, and the user-visible items
// don't justify it. Items are listed top-to-bottom on screen.
// ─────────────────────────────────────────────────────────────────────
typedef enum {
    M_NEXT_PET,
    M_INFO,
    M_FACTORY_RESET,
    M_CLOSE,
    M_COUNT
} menu_idx_t;

static const char *MENU_LABELS[M_COUNT] = {
    "next pet",
    "info / about",
    "factory reset",
    "close",
};

static ui_state_t g_state    = UI_NORMAL;
static uint8_t    g_sel      = 0;       // highlighted menu item
static uint8_t    g_arm_idx  = 0xFF;    // armed confirm target (M_FACTORY_RESET)
static uint32_t   g_arm_until_ms = 0;   // confirm window deadline
static bool       g_dirty    = false;   // forces redraw on state change

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ─────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────

void ui_init(void)
{
    g_state = UI_NORMAL;
    g_sel = 0;
    g_arm_idx = 0xFF;
    g_arm_until_ms = 0;
    g_dirty = true;
}

ui_state_t ui_get_state(void) { return g_state; }

bool ui_overlay_active(void)  { return g_state != UI_NORMAL; }

bool ui_poll_dirty(void)
{
    bool d = g_dirty;
    g_dirty = false;
    return d;
}

// ─────────────────────────────────────────────────────────────────────
// Drawing helpers - Claude-night panel, centered, with a coral border.
// ─────────────────────────────────────────────────────────────────────

#define PANEL_COLOR  0x2104   // dark slate (~#1F1F1F shade above the bg)
#define BORDER_COLOR COLOR_CLAUDE_CORAL

static void draw_panel_frame(int x, int y, int w, int h, uint16_t border)
{
    gfx_fill_rect(x, y, w, h, PANEL_COLOR);
    // 2-px border via thin rects (no gfx_drawRect primitive yet - cheap).
    gfx_fill_rect(x,         y,         w, 2, border);
    gfx_fill_rect(x,         y + h - 2, w, 2, border);
    gfx_fill_rect(x,         y,         2, h, border);
    gfx_fill_rect(x + w - 2, y,         2, h, border);
}

static void compose_menu(void)
{
    // Layout budget (all inside the panel):
    //   8           title baseline
    //   22..23      hairline divider
    //   32 + i*20   items (20 px per row gives proper breathing room
    //               at scale 1; 16 px was too tight with the divider)
    //   end - 44    footer line 1  (TAP Single next)
    //   end - 30    footer line 2  (TAP Double select)
    //   end - 16    footer line 3  (LONG close)
    //   end - 8     bottom inner padding
    // Total H = 32 (header) + M_COUNT*20 (items) + 8 (gap) + 42
    //          (footer 3 × 14) + 8 (inner pad) = M_COUNT*20 + 90
    const int ROW_H  = 20;
    const int W = 220;
    const int H = 32 + (int)M_COUNT * ROW_H + 8 + 42 + 8;
    const int X = (DISPLAY_WIDTH - W) / 2;
    const int Y = (DISPLAY_HEIGHT - H) / 2;

    draw_panel_frame(X, Y, W, H, BORDER_COLOR);

    // Title + hairline divider
    gfx_text_center(Y + 10, "MENU", COLOR_CLAUDE_CORAL, PANEL_COLOR, 1);
    gfx_fill_rect(X + 12, Y + 24, W - 24, 1, COLOR_CLAUDE_DIM);

    // Items. Every row reserves the same chevron column at X+16 so the
    // labels stay on a single vertical line whether or not the row is
    // selected; the chevron is only painted for the selected row.
    for (uint8_t i = 0; i < M_COUNT; i++) {
        int y = Y + 32 + i * ROW_H;
        bool sel = (i == g_sel);
        uint16_t fg = sel ? COLOR_CLAUDE_PAPER : COLOR_CLAUDE_DIM;
        gfx_text(X + 16, y, sel ? ">" : " ", COLOR_CLAUDE_CORAL, PANEL_COLOR, 1);
        gfx_text(X + 32, y, MENU_LABELS[i], fg, PANEL_COLOR, 1);
    }

    // Footer hint, three lines - one gesture per row so the action
    // mapping reads at a glance. 14 px line spacing matches the rest.
    gfx_text_center(Y + H - 44, "TAP Single next",
                    COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
    gfx_text_center(Y + H - 30, "TAP Double select",
                    COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
    gfx_text_center(Y + H - 16, "LONG close",
                    COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
}

// One-page "info / about" with everything useful: device identity,
// Claude link status, BLE pair status, WiFi/IP, current pet, uptime.
static void compose_info(void)
{
    const int W = 220, H = 200;
    const int X = (DISPLAY_WIDTH - W) / 2;
    const int Y = (DISPLAY_HEIGHT - H) / 2;
    draw_panel_frame(X, Y, W, H, BORDER_COLOR);

    int y = Y + 8;
    gfx_text_center(y, "INFO", COLOR_CLAUDE_CORAL, PANEL_COLOR, 1); y += 12;
    gfx_fill_rect(X + 12, y, W - 24, 1, COLOR_CLAUDE_DIM);          y += 6;

    char line[40];

    // device
    gfx_text(X + 10, y, "device", COLOR_CLAUDE_PAPER, PANEL_COLOR, 1);  y += 12;
    snprintf(line, sizeof(line), "  pet: %s", buddy_species_name());
    gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);        y += 10;
    snprintf(line, sizeof(line), "  Lv %u  %lu free heap KB",
             stats_get()->level,
             (unsigned long)(esp_get_free_heap_size() / 1024));
    gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);        y += 10;
    {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(line, sizeof(line), "  up %luh %02lum",
                 (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
        gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);    y += 14;
    }

    // claude link
    gfx_text(X + 10, y, "claude link", COLOR_CLAUDE_PAPER, PANEL_COLOR, 1); y += 12;
    {
        const char *bs;
        if (!ble_connected())   bs = "no peer";
        else if (!ble_secure()) bs = "open (unbonded)";
        else                    bs = "encrypted";
        snprintf(line, sizeof(line), "  BLE: %s", bs);
        gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);    y += 10;
        snprintf(line, sizeof(line), "  heartbeat: %s",
                 bridge_data_alive() ? "live" : "stale");
        gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);    y += 14;
    }

    // wifi
    gfx_text(X + 10, y, "wifi", COLOR_CLAUDE_PAPER, PANEL_COLOR, 1);    y += 12;
    if (wifi_manager_is_connected()) {
        char ip[16]; wifi_manager_get_ip_str(ip, sizeof(ip));
        snprintf(line, sizeof(line), "  online  %s", ip);
    } else {
        snprintf(line, sizeof(line), "  setup AP active");
    }
    gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);        y += 10;
    {
        int8_t r = wifi_manager_get_rssi();
        if (r) snprintf(line, sizeof(line), "  RSSI: %d dBm", r);
        else   snprintf(line, sizeof(line), " ");
        gfx_text(X + 10, y, line, COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
    }

    gfx_text_center(Y + H - 14, "DOUBLE or LONG to close",
                    COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
}

static void compose_reset_confirm(void)
{
    const int W = 220, H = 120;
    const int X = (DISPLAY_WIDTH - W) / 2;
    const int Y = (DISPLAY_HEIGHT - H) / 2;
    draw_panel_frame(X, Y, W, H, COLOR_RED);

    gfx_text_center(Y + 12, "FACTORY RESET", COLOR_RED, PANEL_COLOR, 1);
    gfx_fill_rect(X + 12, Y + 26, W - 24, 1, COLOR_CLAUDE_DIM);

    gfx_text_center(Y + 36, "wipe stats, settings,", COLOR_CLAUDE_PAPER, PANEL_COLOR, 1);
    gfx_text_center(Y + 50, "wifi creds, BLE bonds?", COLOR_CLAUDE_PAPER, PANEL_COLOR, 1);

    // Show the countdown so the user knows the arm is active.
    int32_t remain = (int32_t)(g_arm_until_ms - now_ms());
    if (remain < 0) remain = 0;
    char buf[24];
    snprintf(buf, sizeof(buf), "DOUBLE within %lus", (unsigned long)((remain + 999) / 1000));
    gfx_text_center(Y + 72, buf, COLOR_RED, PANEL_COLOR, 1);

    gfx_text_center(Y + H - 14, "LONG cancel",
                    COLOR_CLAUDE_DIM, PANEL_COLOR, 1);
}

void ui_compose_overlay(void)
{
    switch (g_state) {
    case UI_NORMAL:        break;
    case UI_MENU:          compose_menu();          break;
    case UI_INFO:          compose_info();          break;
    case UI_RESET_CONFIRM: compose_reset_confirm(); break;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Gesture handlers - return true if consumed by the UI.
// ─────────────────────────────────────────────────────────────────────

// Reboot helper - used after factory reset to flush NVS-erase and let
// the user see the fresh boot.
static void schedule_reboot(uint32_t after_ms)
{
    ESP_LOGW(TAG, "reboot in %lu ms", (unsigned long)after_ms);
    // Block briefly so the log lands; the caller is the touch task which
    // doesn't render, so this short delay is fine.
    for (uint32_t i = 0; i < after_ms / 100; i++) {
        // intentional busy-wait (no FreeRTOS dep here keeps the module
        // header-light); 100ms granularity is enough.
        for (volatile int j = 0; j < 1000000; j++) { __asm__ __volatile__("nop"); }
    }
    esp_restart();
}

static void open_menu(void)
{
    g_state = UI_MENU;
    g_sel = 0;
    g_arm_idx = 0xFF;
    g_dirty = true;
}

static void close_overlay(void)
{
    g_state = UI_NORMAL;
    g_arm_idx = 0xFF;
    g_dirty = true;
}

static void menu_activate_current(void)
{
    switch ((menu_idx_t)g_sel) {
    case M_NEXT_PET:
        buddy_next_species();
        ESP_LOGI(TAG, "pet → %s", buddy_species_name());
        // Stay in menu so user can keep cycling.
        g_dirty = true;
        break;
    case M_INFO:
        g_state = UI_INFO;
        g_dirty = true;
        break;
    case M_FACTORY_RESET:
        g_state = UI_RESET_CONFIRM;
        g_arm_until_ms = now_ms() + 5000;   // 5s confirm window
        g_dirty = true;
        break;
    case M_CLOSE:
    default:
        close_overlay();
        break;
    }
}

bool ui_on_tap(void)
{
    if (g_state == UI_MENU) {
        g_sel = (uint8_t)((g_sel + 1) % M_COUNT);
        g_dirty = true;
        return true;
    }
    // INFO and RESET_CONFIRM don't react to TAP (DOUBLE/LONG only).
    if (g_state == UI_INFO || g_state == UI_RESET_CONFIRM) return true;
    return false;
}

bool ui_on_double_tap(void)
{
    if (g_state == UI_MENU) {
        menu_activate_current();
        return true;
    }
    if (g_state == UI_INFO) {
        // Back to menu (not all the way to home).
        g_state = UI_MENU;
        g_dirty = true;
        return true;
    }
    if (g_state == UI_RESET_CONFIRM) {
        if ((int32_t)(g_arm_until_ms - now_ms()) > 0) {
            ESP_LOGW(TAG, "FACTORY RESET confirmed");
            stats_factory_reset();
            ble_clear_bonds();
            wifi_manager_erase_creds();
            schedule_reboot(800);
            // unreachable
        }
        // Arm expired - treat double-tap as cancel.
        close_overlay();
        return true;
    }
    return false;
}

bool ui_on_long_press(void)
{
    // LONG is the universal "back / open menu" gesture.
    if (g_state == UI_NORMAL) {
        open_menu();
        return true;
    }
    if (g_state == UI_MENU) {
        close_overlay();
        return true;
    }
    if (g_state == UI_INFO || g_state == UI_RESET_CONFIRM) {
        // Back to menu - one level up.
        g_state = UI_MENU;
        g_arm_idx = 0xFF;
        g_dirty = true;
        return true;
    }
    return false;
}
