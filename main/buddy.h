#pragma once
#include <stdint.h>
#include "bridge.h"   // persona_state_t

// Multi-species ASCII buddy renderer. Each species lives in its own
// main/buddies/<name>.c file and exposes 7 state functions matching
// persona_state_t order: SLEEP, IDLE, BUSY, ATTENTION, CELEBRATE, DIZZY,
// HEART. (DIZZY and HEART are reserved one-shot states with no derive()
// trigger yet - they're rendered if main.c sets them via a future
// triggerOneShot hook; today they're cosmetic fallbacks.)
//
// Species port from the source 1:1; the *art* is unchanged, only the
// primitives (buddy_print_sprite, etc.) are retargeted to our gfx.c +
// framebuffer instead of TFT_eSprite.

// Geometry - shared layout for all species. Source used 135-wide
// portrait (X_CENTER=67); we have a 240-wide square. Re-center to 120
// so pets stand mid-screen; everything else maps unchanged.
#define BUDDY_X_CENTER  120
// Top row of the 5-line body. SCALE=2 means body spans y=[Y_BASE*2-14,
// Y_BASE*2-14+80]. With Y_BASE=30 the body sits at y=46..126, leaving
// ~100 px below for the transcript / gauges / stats block (the source
// project's PET-stats page split across two screens; we fit it on one).
#define BUDDY_Y_BASE     30
// Overlay particles (Zzz, exclamation marks, hearts) sit ABOVE the
// body at SCALE * Y_OVERLAY. 15 → particles at y=30, just under the
// "Claude Buddy" title at y=4, with room to swing through y=20..60.
#define BUDDY_Y_OVERLAY  15
#define BUDDY_CHAR_W      8
#define BUDDY_CHAR_H      8

// Common colors species can use freely (RGB565)
#define BUDDY_BG     0x0000
#define BUDDY_HEART  0xF810
#define BUDDY_DIM    0x8410
#define BUDDY_YEL    0xFFE0
#define BUDDY_WHITE  0xFFFF
#define BUDDY_CYAN   0x07FF
#define BUDDY_GREEN  0x07E0
#define BUDDY_PURPLE 0xA01F
#define BUDDY_RED    0xF800
#define BUDDY_BLUE   0x041F

// Per-species state function: takes a tickCount, renders the body +
// overlays for the current state. Called twice per fb_frame() (once
// per band); must be idempotent.
typedef void (*buddy_state_fn)(uint32_t t);

typedef struct {
    const char    *name;
    uint16_t       body_color;
    buddy_state_fn states[7];   // index by persona_state_t (0..6)
} species_t;

// ─── Primitives (used by buddies/*.c) ──────────────────────────────
// Print one ASCII row centered around BUDDY_X_CENTER, optionally x-offset.
void buddy_print_line(const char *line, int y_px, uint16_t color, int x_off);
// Print N-line sprite block. y_offset is added to BUDDY_Y_BASE for top row.
void buddy_print_sprite(const char *const *lines, uint8_t n_lines,
                        int y_offset, uint16_t color, int x_off);
// Ad-hoc cursor/color/print for overlay particles.
void buddy_set_cursor(int x, int y);
void buddy_set_color(uint16_t fg);
void buddy_print(const char *s);

// ─── Registry ─────────────────────────────────────────────────────
void        buddy_init(void);
// Advance the animation frame counter once per render cycle. Call
// OUTSIDE the compose callback so re-running compose per band doesn't
// re-tick the animation.
void        buddy_tick_advance(void);
// Render the current species in `state` into the framebuffer. Called
// inside a compose callback (so the active band has already been set).
void        buddy_render(persona_state_t state);

void        buddy_set_species_idx(uint8_t idx);
void        buddy_next_species(void);
uint8_t     buddy_species_idx(void);
uint8_t     buddy_species_count(void);
const char *buddy_species_name(void);
uint16_t    buddy_species_body_color(void);
