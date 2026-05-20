#pragma once
#include <stdint.h>
#include <stdbool.h>

// Banded RGB565 framebuffer. A full 240x240x2 = 115KB contiguous buffer
// does not fit the largest free DRAM block (~96KB with WiFi/NimBLE up),
// so we use ONE half-height band (240x120x2 = ~58KB) composed and
// flushed twice per frame.
//
// Drawing model: callers always use full-screen Y coordinates (0..239).
// fb_set_px() maps into whichever band is active and ignores writes
// outside it, so the same compose code runs unchanged for both bands.
//
// Frame:
//   fb_begin();                       // band 0, cleared to bg
//   <draw using full-screen coords>   // band-0 pixels land, rest ignored
//   fb_flush_band();                  // blit band 0, advance to band 1,
//   <SAME draw calls again>           //   re-run identical compose
//   fb_flush_band();                  // blit band 1 → frame complete
// fb_frame() wraps this with a callback so callers don't double-write.

#define FB_W       240
#define FB_H       240
#define FB_BAND_H  120          // 240 / 2 bands
#define FB_BANDS   (FB_H / FB_BAND_H)

// Allocate the band buffer (heap_caps, internal DRAM). Once at boot,
// after WiFi/NimBLE init so the measured-heap assumption holds.
// Returns false if the ~58KB allocation fails.
bool fb_init(void);

// Free DRAM remaining after init, exposed so boot logs can report the
// post-framebuffer heap budget.
uint32_t fb_free_heap(void);

// Compose-and-flush a whole frame. `draw` is invoked once per band
// (FB_BANDS times); it must be idempotent and use full-screen coords.
typedef void (*fb_draw_fn)(void *ctx);
void fb_frame(fb_draw_fn draw, void *ctx);

// Low-level primitives (used by gfx.c). Coords are full-screen; writes
// outside the active band are silently skipped.
void fb_clear(uint16_t color);                 // clears active band only
void fb_set_px(int x, int y, uint16_t color);
void fb_fill_rect(int x, int y, int w, int h, uint16_t color);
