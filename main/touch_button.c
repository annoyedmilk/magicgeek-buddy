// Capacitive touch gesture detection for the GeekMagic Pro's single pad.
// Ported verbatim from the proven F1 Dashboard firmware (ESP-IDF v6
// touch_sens API, calibration + drift compensation tuned to this device).
#include "touch_button.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define TOUCH_CHANNEL       9   // GPIO32 = Touch channel 9
#define MIN_PRESS_MS       50   // Reject releases shorter than this (noise)
#define TAP_MAX_MS        500   // Max press duration for a tap
#define DOUBLE_TAP_MS     300   // Max gap between taps for double-tap
#define LONG_PRESS_MS     800   // Hold duration for long press
#define POLL_INTERVAL_MS   20   // Polling rate
#define RECAL_INTERVAL_MS 30000 // Recalibrate baseline every 30s when idle
#define RECAL_ALPHA       0.05f // EMA smoothing factor for drift compensation

static touch_sensor_handle_t  touch_sens_handle;
static touch_channel_handle_t touch_chan_handle;
static touch_event_cb_t       event_callback;
static uint32_t               touch_benchmark;

// Gesture state machine
typedef enum {
    STATE_IDLE,
    STATE_PRESSED,
    STATE_WAIT_SECOND_TAP,
    STATE_LONG_PRESS_FIRED,
} gesture_state_t;

void touch_button_init(void)
{
    // Create controller with sample configuration
    touch_sensor_sample_config_t sample_cfg[] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &touch_sens_handle));

    // Create touch channel on GPIO32 (channel 9)
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = {0},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    ESP_ERROR_CHECK(touch_sensor_new_channel(touch_sens_handle, TOUCH_CHANNEL, &chan_cfg, &touch_chan_handle));

    // Configure software filter
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(touch_sens_handle, &filter_cfg));

    // Calibration - run initial scans to establish baseline
    ESP_ERROR_CHECK(touch_sensor_enable(touch_sens_handle));
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(touch_sens_handle, 2000));
    }
    ESP_ERROR_CHECK(touch_sensor_disable(touch_sens_handle));

    // Read baseline and set threshold (98% of benchmark)
    ESP_ERROR_CHECK(touch_channel_read_data(touch_chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &touch_benchmark));
    chan_cfg.abs_active_thresh[0] = (uint32_t)(touch_benchmark * 0.98f);
    ESP_ERROR_CHECK(touch_sensor_reconfig_channel(touch_chan_handle, &chan_cfg));
    ESP_LOGI(TAG, "Touch calibrated: benchmark=%lu, threshold=%lu", touch_benchmark, chan_cfg.abs_active_thresh[0]);

    // Re-enable and start continuous scanning
    ESP_ERROR_CHECK(touch_sensor_enable(touch_sens_handle));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(touch_sens_handle));

    ESP_LOGI(TAG, "Touch sensor initialized (GPIO32 / Channel %d)", TOUCH_CHANNEL);
}

static bool is_touched(void)
{
    uint32_t smooth_val = 0;
    touch_channel_read_data(touch_chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &smooth_val);
    return (smooth_val < touch_benchmark * 98 / 100);
}

static void touch_task(void *arg)
{
    gesture_state_t state = STATE_IDLE;
    TickType_t press_start = 0;
    TickType_t release_time = 0;
    TickType_t last_recal = xTaskGetTickCount();

    while (1) {
        bool touched = is_touched();
        TickType_t now = xTaskGetTickCount();

        switch (state) {
        case STATE_IDLE:
            if (touched) {
                state = STATE_PRESSED;
                press_start = now;
            } else {
                // Periodic drift compensation: slowly update baseline when idle
                uint32_t idle_ms = (now - last_recal) * portTICK_PERIOD_MS;
                if (idle_ms >= RECAL_INTERVAL_MS) {
                    uint32_t current_val = 0;
                    touch_channel_read_data(touch_chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &current_val);
                    // EMA: blend current reading into benchmark
                    touch_benchmark = (uint32_t)((1.0f - RECAL_ALPHA) * touch_benchmark + RECAL_ALPHA * current_val);
                    last_recal = now;
                    ESP_LOGD(TAG, "Touch recal: benchmark=%lu", touch_benchmark);
                }
            }
            break;

        case STATE_PRESSED:
            if (!touched) {
                uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;
                if (held_ms < MIN_PRESS_MS) {
                    // Too short - noise, ignore
                    state = STATE_IDLE;
                } else if (held_ms < TAP_MAX_MS) {
                    // Short tap - wait for possible second tap
                    state = STATE_WAIT_SECOND_TAP;
                    release_time = now;
                } else {
                    // Released after long hold but before threshold - ignore
                    state = STATE_IDLE;
                }
            } else {
                uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;
                if (held_ms >= LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "Long press detected");
                    if (event_callback) event_callback(TOUCH_EVENT_LONG_PRESS);
                    state = STATE_LONG_PRESS_FIRED;
                }
            }
            break;

        case STATE_WAIT_SECOND_TAP:
            if (touched) {
                // Second tap started
                ESP_LOGI(TAG, "Double tap detected");
                if (event_callback) event_callback(TOUCH_EVENT_DOUBLE_TAP);
                // Wait for release
                while (is_touched()) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                }
                state = STATE_IDLE;
            } else {
                uint32_t gap_ms = (now - release_time) * portTICK_PERIOD_MS;
                if (gap_ms >= DOUBLE_TAP_MS) {
                    // No second tap - it's a single tap
                    ESP_LOGI(TAG, "Single tap detected");
                    if (event_callback) event_callback(TOUCH_EVENT_TAP);
                    state = STATE_IDLE;
                }
            }
            break;

        case STATE_LONG_PRESS_FIRED:
            if (!touched) {
                state = STATE_IDLE;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void touch_button_start(touch_event_cb_t callback)
{
    event_callback = callback;
    xTaskCreate(touch_task, "touch_btn", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Touch button task started");
}
