// ASCII buddy renderer. Each species in buddies/<name>.c is data + a 7-
// state function table - they compose into the framebuffer via the
// primitives here (buddy_print_sprite / buddy_set_cursor / etc.) instead
// of the source project's TFT_eSprite calls. The art is unchanged.
//
// Rendering scale: 2 (each ASCII char is 16x16 px on our 240x240 panel).
// Source used scale 2 on the home screen and scale 1 in peek mode; we
// only ever render at the home size for now, so the path is fixed.
#include "buddy.h"
#include "display.h"
#include "framebuffer.h"
#include "gfx.h"
#include "storage.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "buddy";

// ─────────────────────────────────────────────────────────────────────
// Scale-aware drawing primitives. Buddies pass full-screen-style coords
// that assume scale 1 around (BUDDY_X_CENTER=120, BUDDY_Y_BASE=60); we
// apply the scale here so species files stay scale-agnostic.
// ─────────────────────────────────────────────────────────────────────
static const int   SCALE = 2;
static uint16_t    g_text_color = 0xFFFF;
static int         g_cursor_x = 0, g_cursor_y = 0;

void buddy_set_color(uint16_t fg) { g_text_color = fg; }
void buddy_set_cursor(int x, int y)
{
    // Transform the species' 1x coord around the geometric center, then
    // apply scale. (x - X_CENTER) * SCALE + X_CENTER keeps the body
    // visually centered when scale changes. Y just scales from 0.
    g_cursor_x = BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * SCALE;
    g_cursor_y = y * SCALE;
}

void buddy_print(const char *s)
{
    int x = g_cursor_x, y = g_cursor_y;
    for (const char *p = s; *p; p++) {
        gfx_char(x, y, *p, g_text_color, BUDDY_BG, SCALE);
        x += BUDDY_CHAR_W * SCALE;
    }
    g_cursor_x = x;
}

void buddy_print_line(const char *line, int y_px, uint16_t color, int x_off)
{
    int len = (int)strlen(line);
    // At scale > 1 the fixed-width art pads with spaces that would push
    // ink off-center / off-screen; trim leading/trailing space.
    if (SCALE > 1) {
        while (len && line[len - 1] == ' ') len--;
        while (len && *line == ' ') { line++; len--; }
    }
    int w = len * BUDDY_CHAR_W * SCALE;
    int x = BUDDY_X_CENTER - w / 2 + x_off * SCALE;
    for (int i = 0; i < len; i++) {
        gfx_char(x + i * BUDDY_CHAR_W * SCALE, y_px,
                 line[i], color, BUDDY_BG, SCALE);
    }
}

void buddy_print_sprite(const char *const *lines, uint8_t n_lines,
                        int y_offset, uint16_t color, int x_off)
{
    // Source: yBase = BUDDY_Y_BASE * scale - (scale-1)*14. The -14 lift
    // keeps the body in the same visual region as scale 1 art that
    // assumed y=BUDDY_Y_BASE was already mid-screen. We carry it for
    // scale 2 (yields -14 px), matching source behavior precisely.
    int y_base = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
    for (uint8_t i = 0; i < n_lines; i++) {
        buddy_print_line(lines[i],
                         y_base + (y_offset + i * BUDDY_CHAR_H) * SCALE,
                         color, x_off);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Species table. Every species file declares one `extern const species_t
// FOO_SPECIES`; we list them all here. Order is the cycle order (Menu →
// "next pet" walks this).
// ─────────────────────────────────────────────────────────────────────
extern const species_t CAPYBARA_SPECIES;
extern const species_t DUCK_SPECIES;
extern const species_t GOOSE_SPECIES;
extern const species_t BLOB_SPECIES;
extern const species_t CAT_SPECIES;
extern const species_t DRAGON_SPECIES;
extern const species_t OCTOPUS_SPECIES;
extern const species_t OWL_SPECIES;
extern const species_t PENGUIN_SPECIES;
extern const species_t TURTLE_SPECIES;
extern const species_t SNAIL_SPECIES;
extern const species_t GHOST_SPECIES;
extern const species_t AXOLOTL_SPECIES;
extern const species_t CACTUS_SPECIES;
extern const species_t ROBOT_SPECIES;
extern const species_t RABBIT_SPECIES;
extern const species_t MUSHROOM_SPECIES;
extern const species_t CHONK_SPECIES;

static const species_t *const SPECIES_TABLE[] = {
    &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES,
    &CAT_SPECIES, &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES,
    &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
    &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES,
    &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES =
    sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);

static uint8_t  g_current_idx = 0;
static uint32_t g_tick_count  = 0;     // bumped externally per frame batch

// ─────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────

void buddy_init(void)
{
    uint32_t saved;
    if (storage_get_u32("species", &saved, 0) == 0 && saved < N_SPECIES) {
        g_current_idx = (uint8_t)saved;
    }
    ESP_LOGI(TAG, "ready: %u species, current = '%s' (idx %u)",
             N_SPECIES, SPECIES_TABLE[g_current_idx]->name, g_current_idx);
}

void buddy_set_species_idx(uint8_t idx)
{
    if (idx >= N_SPECIES) return;
    g_current_idx = idx;
    storage_set_u32("species", idx);
}

void buddy_next_species(void)
{
    buddy_set_species_idx((g_current_idx + 1) % N_SPECIES);
}

uint8_t     buddy_species_idx(void)     { return g_current_idx; }
uint8_t     buddy_species_count(void)   { return N_SPECIES; }
const char *buddy_species_name(void)    { return SPECIES_TABLE[g_current_idx]->name; }
uint16_t    buddy_species_body_color(void) { return SPECIES_TABLE[g_current_idx]->body_color; }

void buddy_render(persona_state_t state)
{
    if ((unsigned)state >= 7) state = P_IDLE;
    const species_t *sp = SPECIES_TABLE[g_current_idx];
    if (!sp->states[state]) {
        // Species lacks this state - fall back to IDLE so we still draw
        // *something*. Should never happen for the ported species (all
        // 7 states are required) but keeps the path total.
        if (sp->states[P_IDLE]) sp->states[P_IDLE](g_tick_count);
        return;
    }
    sp->states[state](g_tick_count);
}

// Advance the animation tick. Called by main.c on its own cadence so
// per-band re-render in fb_frame doesn't re-tick (idempotent compose).
void buddy_tick_advance(void)
{
    g_tick_count++;
}
