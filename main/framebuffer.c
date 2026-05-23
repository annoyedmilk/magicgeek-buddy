// Banded RGB565 framebuffer with async DMA ping-pong. See framebuffer.h.
//
// Architecture change from the original single-buffer design:
//   Before: one 57 600 B band, composed then flushed synchronously (2 bands).
//   After:  two 11 520 B bands (fb_band_0 / fb_band_1), 10 bands per frame.
//           While the SPI DMA is shipping band N to the panel, the CPU is
//           composing band N+1 into the other buffer. The only wait is the
//           brief window between "compose done" and "previous DMA done" -
//           at 20 MHz, each 11 520 B transfer takes ~4.6 ms, so the stall
//           is zero for all screens where composing > 4.6 ms, and a few
//           hundred µs for sparse screens.
//
// Total framebuffer RAM: 2 × 11 520 = 23 040 B, down from 57 600 B.
#include "framebuffer.h"
#include "display.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "fb";

// Two DMA-capable band buffers for ping-pong.
static uint16_t *fb_band_0  = NULL;
static uint16_t *fb_band_1  = NULL;

// Set by fb_frame before each draw() call; points to whichever of the two
// buffers the CPU should write into this pass. NULL between frames so
// stray fb_set_px calls outside fb_frame are silently dropped.
static uint16_t *active_buf  = NULL;
static int       active_band_y = 0;

// RGB565 is stored big-endian (high byte first) so the panel receives the
// bytes in the right order during the DMA blit without a per-pixel swap.
static inline uint16_t store_be(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

bool fb_init(void)
{
    size_t bytes = (size_t)FB_W * FB_BAND_H * sizeof(uint16_t);
    fb_band_0 = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    fb_band_1 = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!fb_band_0 || !fb_band_1) {
        ESP_LOGE(TAG, "framebuffer alloc %u bytes FAILED (need 2 × %u) - "
                 "likely heap fragmentation", (unsigned)(bytes * 2), (unsigned)bytes);
        heap_caps_free(fb_band_0); fb_band_0 = NULL;
        heap_caps_free(fb_band_1); fb_band_1 = NULL;
        return false;
    }
    ESP_LOGI(TAG, "framebuffer ok: 2x %d x %d bands, %u B each, free heap %u",
             FB_W, FB_BAND_H, (unsigned)bytes, (unsigned)esp_get_free_heap_size());
    return true;
}

uint32_t fb_free_heap(void) { return esp_get_free_heap_size(); }

void fb_clear(uint16_t color)
{
    if (!active_buf) return;
    uint16_t be = store_be(color);
    size_t n = (size_t)FB_W * FB_BAND_H;
    for (size_t i = 0; i < n; i++) active_buf[i] = be;
}

void fb_set_px(int x, int y, uint16_t color)
{
    if (!active_buf) return;
    if ((unsigned)x >= FB_W) return;
    int by = y - active_band_y;
    if ((unsigned)by >= (unsigned)FB_BAND_H) return;
    active_buf[by * FB_W + x] = store_be(color);
}

void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!active_buf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;

    int y_top = y - active_band_y;
    int y_bot = y + h - active_band_y;
    if (y_top < 0)         y_top = 0;
    if (y_bot > FB_BAND_H) y_bot = FB_BAND_H;
    if (y_top >= y_bot) return;

    uint16_t be = store_be(color);
    for (int by = y_top; by < y_bot; by++) {
        uint16_t *row = &active_buf[by * FB_W + x];
        for (int i = 0; i < w; i++) row[i] = be;
    }
}

// Compose and flush all bands using async DMA ping-pong.
//
// Ordering within each iteration:
//   1. Set active_buf to one of the two buffers (alternating).
//   2. Call draw(): composes this band into active_buf.
//   3. Wait for the *previous* band's DMA to finish.
//      (This is the only potential stall point; if composing took longer
//      than 4.6 ms the DMA is already done and this returns immediately.)
//   4. Set the panel window for this band (uses polling SPI, must not
//      race an in-flight DMA, hence the wait above).
//   5. Queue async DMA for active_buf.
//   6. Loop: the CPU is now free to compose the next band into the other
//      buffer while the SPI peripheral ships this one.
// After the last band, a final display_wait_async() drains the pipeline
// before returning to the caller.
void fb_frame(fb_draw_fn draw, void *ctx)
{
    if (!fb_band_0 || !fb_band_1 || !draw) return;

    uint16_t *bufs[2] = { fb_band_0, fb_band_1 };

    for (int band = 0; band < FB_BANDS; band++) {
        active_band_y = band * FB_BAND_H;
        active_buf    = bufs[band & 1];

        // Compose into active_buf (full-screen coords; out-of-band writes
        // are silently dropped by fb_set_px / fb_fill_rect).
        draw(ctx);

        // Drain any in-flight DMA before touching the SPI bus with
        // spi_device_polling_transmit (used inside display_set_window).
        // On the first band there is nothing to drain.
        display_wait_async();

        display_set_window(0, active_band_y,
                           FB_W - 1, active_band_y + FB_BAND_H - 1);
        display_queue_pixels_async((const uint8_t *)active_buf,
                                   FB_W * FB_BAND_H * 2);

        // Next iteration composes into the *other* buffer while DMA runs.
    }

    // Drain the last band's DMA before returning; callers must not read the
    // display contents or start another frame until this returns.
    display_wait_async();

    active_band_y = 0;
    active_buf    = NULL;
}
