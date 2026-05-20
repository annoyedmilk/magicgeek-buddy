#pragma once
#include <stdint.h>

// Minimal drawing primitives: full ASCII 32-126 8x8 bitmap font plus a
// filled rect. Output goes to the active framebuffer band (writes
// outside the band are silently dropped), so the same calls compose
// safely twice per frame.

// Filled rectangle, RGB565.
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);

// One 8x8 glyph (scale 1 = 8px, 2 = 16px ...). Background painted so
// glyph cells overwrite cleanly without a separate clear.
void gfx_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);

// Left-aligned string from (x,y). Returns the x just past the last glyph.
int  gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);

// Horizontally centered string across the full 240px width.
void gfx_text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale);
