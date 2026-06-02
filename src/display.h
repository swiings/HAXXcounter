#pragma once
#include <Arduino.h>
#include <lvgl.h>

/* Physical panel dimensions for Waveshare ESP32-S3-Touch-AMOLED-1.8 */
constexpr int16_t DISPLAY_WIDTH  = 368;
constexpr int16_t DISPLAY_HEIGHT = 448;

/**
 * Initialise the SH8601 AMOLED over QSPI, then configure LVGL with a
 * PSRAM-backed draw buffer and register the display + touch drivers.
 * Must be called before any lv_* UI calls.
 */
void display_init();
