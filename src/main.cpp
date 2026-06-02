#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "ui.h"
#include "counter.h"

/* =========================================================================
   Boot button — GPIO0, active-low
   Toggles all-devices / people-estimate mode.
   ========================================================================= */
#define PIN_BOOT_BTN 0

static int      g_btn_last    = HIGH;
static uint32_t g_btn_time_ms = 0;

static void button_poll() {
    int btn = digitalRead(PIN_BOOT_BTN);
    if (btn == LOW && g_btn_last == HIGH &&
        (millis() - g_btn_time_ms) > 200) {
        g_btn_time_ms = millis();
        counter_toggle_mode();
        ui_set_footer(counter_mode_label());
    }
    g_btn_last = btn;
}

/* =========================================================================
   Touch gesture — swipe up/down adjusts RSSI threshold
   ========================================================================= */
static void gesture_poll() {
    TouchGesture g = display_pop_gesture();
    if (g == GESTURE_NONE) return;

    int cur = counter_get_rssi();
    int next;

    if (g == GESTURE_SWIPE_UP) {
        /* Swipe up → tighten (higher threshold = closer devices only) */
        next = min(RSSI_MAX, cur + RSSI_STEP);
    } else {
        /* Swipe down → loosen (lower threshold = farther devices included) */
        next = max(RSSI_MIN, cur - RSSI_STEP);
    }

    if (next != cur) {
        counter_set_rssi(next);
        ui_show_rssi_overlay(next);
    }
}

/* =========================================================================
   Arduino entry points
   ========================================================================= */
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[HAXXcounter] booting");

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
    display_init();
    ui_init();
    counter_init();

    ui_set_footer(counter_mode_label());
    Serial.printf("[HAXXcounter] ready  RSSI threshold: %d dBm\n",
                  counter_get_rssi());
}

void loop() {
    lv_timer_handler();
    counter_tick();
    button_poll();
    gesture_poll();
    ui_set_count(counter_get());
    delay(5);
}
