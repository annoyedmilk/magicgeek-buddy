// Banded RGB565 framebuffer. See framebuffer.h for the model.
// One ~58KB band buffer is composed and flushed twice per frame. Each
// flush is a single contiguous SPI blit via display_send_pixels, so we
// never issue per-primitive blocking transmits that would risk tripping
// the interrupt watchdog.
#include "framebuffer.h"
#include "display.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "fb";

static uint16_t *fb_band = NULL;          // FB_W * FB_BAND_H pixels, RGB565
static int       active_band_y = 0;       // top Y of the currently active band

// RGB565 is stored as native uint16_t; the panel expects big-endian on the
// wire (high byte first). store_be() builds the byte order at write time so
// flush is a straight memcpy through SPI - no per-pixel byte swap on flush.
static inline uint16_t store_be(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

bool fb_init(void)
{
    size_t bytes = (size_t)FB_W * FB_BAND_H * sizeof(uint16_t);
    fb_band = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!fb_band) {
        ESP_LOGE(TAG, "framebuffer alloc %u bytes FAILED - likely heap fragmentation",
                 (unsigned)bytes);
        return false;
    }
    ESP_LOGI(TAG, "framebuffer ok: %d x %d band, %u bytes, free heap %u",
             FB_W, FB_BAND_H, (unsigned)bytes, (unsigned)esp_get_free_heap_size());
    return true;
}

uint32_t fb_free_heap(void) { return esp_get_free_heap_size(); }

void fb_clear(uint16_t color)
{
    if (!fb_band) return;
    uint16_t be = store_be(color);
    size_t n = (size_t)FB_W * FB_BAND_H;
    // 16-bit fill: memset can't do 2-byte patterns, so a short loop. The
    // compiler unrolls this nicely; it runs once per band per frame.
    for (size_t i = 0; i < n; i++) fb_band[i] = be;
}

void fb_set_px(int x, int y, uint16_t color)
{
    if (!fb_band) return;
    if ((unsigned)x >= FB_W) return;
    int by = y - active_band_y;
    if ((unsigned)by >= (unsigned)FB_BAND_H) return;   // outside active band
    fb_band[by * FB_W + x] = store_be(color);
}

void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!fb_band || w <= 0 || h <= 0) return;
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
        uint16_t *row = &fb_band[by * FB_W + x];
        for (int i = 0; i < w; i++) row[i] = be;
    }
}

// Flush the current band to the panel as one contiguous SPI blit, then
// advance to the next band. fb_frame() drives the two-pass loop.
static void fb_flush_band(void)
{
    if (!fb_band) return;
    display_set_window(0, active_band_y, FB_W - 1, active_band_y + FB_BAND_H - 1);
    display_send_pixels((const uint8_t *)fb_band, FB_W * FB_BAND_H * 2);
}

void fb_frame(fb_draw_fn draw, void *ctx)
{
    if (!fb_band || !draw) return;
    for (int band = 0; band < FB_BANDS; band++) {
        active_band_y = band * FB_BAND_H;
        // The callback uses full-screen coords; fb_set_px / fb_fill_rect
        // skip writes outside the active band, so the SAME draw code runs
        // unchanged for each band. Caller doesn't need to know about banding.
        draw(ctx);
        fb_flush_band();
    }
    active_band_y = 0;
}
