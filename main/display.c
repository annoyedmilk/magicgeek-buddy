// ST7789V raw-SPI driver for the GeekMagic Pro 240x240 panel.
// Ported verbatim from the proven F1 Dashboard firmware - the init
// sequence and inversion (0x21) are tuned to this exact display.
#include "display.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

// Pin definitions (GeekMagic Pro)
#define PIN_DISPLAY_CLK    18
#define PIN_DISPLAY_MOSI   23
#define PIN_DISPLAY_DC      2
#define PIN_DISPLAY_RST     4
#define PIN_BACKLIGHT      25   // inverted: LOW = on, HIGH = off

// Backlight PWM (LEDC). 5 kHz is above any audible buzz from cheap
// boost circuits and well below the limit where MOSFET gate drive
// starts to slew. 8 bits of resolution gives smooth fades and keeps
// the math trivial (255 = full off, 0 = full on after inversion).
#define BL_LEDC_TIMER     LEDC_TIMER_0
#define BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ   5000
#define BL_LEDC_RES_BITS  LEDC_TIMER_8_BIT
#define BL_LEDC_DUTY_MAX  255

static spi_device_handle_t spi_dev;

static void send_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(PIN_DISPLAY_DC, 0);
    spi_device_polling_transmit(spi_dev, &t);
}

static void send_data(const uint8_t *data, int len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(PIN_DISPLAY_DC, 1);
    spi_device_polling_transmit(spi_dev, &t);
}

static void send_data_byte(uint8_t data)
{
    send_data(&data, 1);
}

void display_init(void)
{
    // DC + RST are plain GPIO outputs. BACKLIGHT (IO25) is driven by
    // LEDC so the host can fade for idle dimming / burn-in protection.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_DISPLAY_DC)
                      | (1ULL << PIN_DISPLAY_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    // LEDC: 5 kHz / 8 bit on IO25. Start at 100% (duty=0 because the
    // pin is inverted - LOW = backlight on) so the panel lights from
    // first frame, same UX as the prior pure-GPIO path.
    ledc_timer_config_t bl_timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES_BITS,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));

    ledc_channel_config_t bl_chan = {
        .gpio_num   = PIN_BACKLIGHT,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,    // duty 0 = pin LOW = backlight ON (inverted)
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_chan));

    // Initialize SPI bus. The framebuffer flushes a whole band
    // (240*120*2 = 57,600 B) in one DMA transfer, so max_transfer_sz
    // is sized for a full band.
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_DISPLAY_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = PIN_DISPLAY_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 120 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode           = 3,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi_dev));

    ESP_LOGI(TAG, "SPI bus initialized (CLK=%d, MOSI=%d)", PIN_DISPLAY_CLK, PIN_DISPLAY_MOSI);

    // Hardware reset
    gpio_set_level(PIN_DISPLAY_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_DISPLAY_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(0x01);  // Software reset
    vTaskDelay(pdMS_TO_TICKS(150));

    send_cmd(0x11);  // Sleep out
    vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(0x3A);  // Color mode
    send_data_byte(0x55);  // 16-bit RGB565

    send_cmd(0x36);  // Memory access control
    send_data_byte(0x00);

    send_cmd(0x21);  // Inversion on (required for correct colors on this panel)

    send_cmd(0x29);  // Display on
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Display initialized (ST7789V %dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    send_cmd(0x2A);  // Column address set
    data[0] = x0 >> 8; data[1] = x0 & 0xFF;
    data[2] = x1 >> 8; data[3] = x1 & 0xFF;
    send_data(data, 4);

    send_cmd(0x2B);  // Row address set
    data[0] = y0 >> 8; data[1] = y0 & 0xFF;
    data[2] = y1 >> 8; data[3] = y1 & 0xFF;
    send_data(data, 4);

    send_cmd(0x2C);  // Memory write
}

void display_fill_color(uint16_t color)
{
    display_set_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);

    uint8_t buf[DISPLAY_WIDTH * 2];
    for (int i = 0; i < DISPLAY_WIDTH; i++) {
        buf[i * 2]     = color >> 8;
        buf[i * 2 + 1] = color & 0xFF;
    }

    gpio_set_level(PIN_DISPLAY_DC, 1);
    for (int row = 0; row < DISPLAY_HEIGHT; row++) {
        spi_transaction_t t = {
            .length = sizeof(buf) * 8,
            .tx_buffer = buf,
        };
        spi_device_polling_transmit(spi_dev, &t);
        // 240 back-to-back polling transmits monopolize the CPU long
        // enough to trip the 300ms interrupt watchdog when several heavy
        // draws run in one go (full fill + header + ~40 glyphs). Yield
        // every 32 rows so the idle task / WDT get serviced. Note: in
        // normal operation we don't reach this path - the banded RAM
        // framebuffer in framebuffer.c flushes via a single DMA blit.
        if ((row & 0x1F) == 0x1F) vTaskDelay(1);
    }
}

void display_send_pixels(const uint8_t *data, int len)
{
    if (len == 0) return;
    gpio_set_level(PIN_DISPLAY_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

void display_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    // Inverted: 100% brightness == duty 0 (pin LOW). Linear map across
    // the 8-bit range; one whole-frame redraw of duty=0 vs full off is
    // visually indistinguishable from the prior GPIO-on path.
    uint32_t duty = BL_LEDC_DUTY_MAX -
                    (uint32_t)((BL_LEDC_DUTY_MAX * (uint32_t)percent) / 100u);
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}
