#pragma once

// Single capacitive touch pad (GPIO32, channel 9) is the only input.
// Gesture → action mapping is context-dependent and lives in main.c /
// ui.c; this module only detects the raw gestures:
//
//   on a prompt :  TAP = approve   DOUBLE = deny     LONG = menu
//   normal      :  TAP = next scr  DOUBLE = back      LONG = menu
//   in a menu   :  TAP = next item DOUBLE = select    LONG = close
typedef enum {
    TOUCH_EVENT_TAP,         // Single tap (<500ms, no second tap within 300ms)
    TOUCH_EVENT_DOUBLE_TAP,  // Two taps within 300ms
    TOUCH_EVENT_LONG_PRESS,  // Held >800ms
} touch_event_t;

/**
 * Callback type for touch events. Invoked from the touch task context.
 */
typedef void (*touch_event_cb_t)(touch_event_t event);

/**
 * Initialize touch sensor on GPIO32 (channel 9) with calibration.
 */
void touch_button_init(void);

/**
 * Start the touch button task that monitors for gestures.
 * Calls the provided callback from the task context when events occur.
 */
void touch_button_start(touch_event_cb_t callback);
