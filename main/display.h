#pragma once

#include <stdint.h>

// GeekMagic Pro: 1.54" 240x240 IPS, ST7789V, SPI mode 3 @ 20MHz.
// Full 240x240 is usable - the case bezel is ignored per design decision.
#define DISPLAY_WIDTH      240
#define DISPLAY_HEIGHT     240

// RGB565 colors
#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0
#define COLOR_BLUE   0x001F
#define COLOR_WHITE  0xFFFF
#define COLOR_BLACK  0x0000
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN   0x07FF

// Anthropic / Claude palette (RGB565, from #1F1E1D / #F0EEE6 / #D97757)
#define COLOR_CLAUDE_BG    0x18E3   // #1F1E1D charcoal
#define COLOR_CLAUDE_PAPER 0xF775   // #F0EEE6 warm paper
#define COLOR_CLAUDE_CORAL 0xDBAA   // #D97757 clay/coral accent
#define COLOR_CLAUDE_DIM   0x8410   // mid grey for secondary text

/**
 * Initialize SPI bus and ST7789V display.
 * Must be called before any other display function.
 */
void display_init(void);

/**
 * Set the drawing window for subsequent pixel data.
 */
void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * Fill the entire display with a single RGB565 color.
 */
void display_fill_color(uint16_t color);

/**
 * Send raw pixel data (RGB565, big-endian) to the display.
 * Blocking polling transmit. Must call display_set_window() first.
 * Not used by the normal render path (which goes through the async pair
 * below), but kept for callers that need a synchronous one-shot send.
 */
void display_send_pixels(const uint8_t *data, int len);

/**
 * Queue an async DMA pixel transfer. Returns immediately; the SPI
 * peripheral ships the data in the background. The `data` pointer must
 * remain valid until display_wait_async() returns.
 * Must call display_set_window() first, and display_wait_async() must
 * have been called to drain any previous in-flight transfer before
 * calling display_set_window() again.
 */
void display_queue_pixels_async(const uint8_t *data, int len);

/**
 * Block until the most recently queued async transfer completes.
 * No-op if no transfer is in flight. Must be called before issuing
 * any spi_device_polling_transmit (commands, set_window, fill_color).
 */
void display_wait_async(void);

/**
 * Set the backlight brightness (0..100, percent). 0 = off, 100 = full.
 * Internally drives IO25 via LEDC (PWM). The pin is inverted on this
 * board (LOW = bright), so the driver maps the percent linearly onto
 * the inverted duty cycle.
 */
void display_set_backlight(uint8_t percent);
