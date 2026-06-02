#pragma once
#include <Arduino.h>

/* Physical panel dimensions for Waveshare ESP32-S3-Touch-AMOLED-1.8 */
constexpr int16_t DISPLAY_WIDTH  = 368;
constexpr int16_t DISPLAY_HEIGHT = 448;

/** Swipe direction reported by the FT3168 touch controller. */
enum TouchGesture {
    GESTURE_NONE,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN
};

/**
 * Initialise the SH8601 AMOLED over QSPI, then configure LVGL with a
 * PSRAM-backed draw buffer and register the display + touch drivers.
 * Must be called before any lv_* UI calls.
 */
void display_init();

/**
 * Returns and clears the most recent swipe gesture detected by the touch
 * driver. Call once per loop after lv_timer_handler().
 */
TouchGesture display_pop_gesture();
