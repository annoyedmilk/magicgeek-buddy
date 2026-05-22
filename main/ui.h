#pragma once
#include <stdint.h>
#include <stdbool.h>

// UI overlay state machine. The home screen (persona/setup/etc.) is
// composed by main.c; ui.* layers menu and info panels on top, plus
// owns the touch routing for navigation when an overlay is open.
//
// Hardware reality vs source: we have ONE touch pad (TAP/DOUBLE/LONG),
// no buttons, no beeper, no backlight pin. The menu items are pruned
// to what actually applies: pet cycle, info, factory reset, close.

typedef enum {
    UI_NORMAL,           // no overlay - home screen is fully visible
    UI_MENU,             // top-level menu panel
    UI_INFO,             // info / about page (no submenu items)
    UI_RESET_CONFIRM,    // "really?" two-step before factory reset
    UI_TRUST_PROMPT,     // new WiFi joined - user picks trust level
} ui_state_t;

// Initialize internal state. Call once at boot, after stats_load().
void ui_init(void);

ui_state_t ui_get_state(void);

// True iff the home composer should suppress the lower stats strip /
// transcript to give the overlay more room (panels center in the body).
bool ui_overlay_active(void);

// Compose the overlay into the framebuffer. Called inside fb_frame()
// AFTER the home compose; idempotent (must be safe to run per band).
// No-op when UI_NORMAL.
void ui_compose_overlay(void);

// Touch gesture handlers - return true if the gesture was consumed by
// the UI (so main.c knows NOT to also treat it as a home-screen gesture
// like approve/deny). LONG always opens the menu when no overlay is up
// and is consumed even in NORMAL.
bool ui_on_tap(void);
bool ui_on_double_tap(void);
bool ui_on_long_press(void);

// True if any per-tick state (e.g. RESET_CONFIRM countdown) has changed
// since the last poll. The render loop reads this to know whether the
// overlay needs a redraw without us bumping a counter.
bool ui_poll_dirty(void);

// Open the trust prompt for a newly-joined network. Called from the
// render loop when net_trust_prompt_pending() goes true. The captured
// bssid/ssid are stored so the user's gesture commits against the AP
// they actually saw on the screen, even if a roam happens before they
// respond. 60s timeout auto-commits NET_TRUST_DENY.
void ui_open_trust_prompt(const uint8_t bssid[6], const char *ssid);
