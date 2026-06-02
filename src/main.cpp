#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <XPowersLib.h>

#include "display.h"
#include "ui.h"
#include "counter.h"

/* =========================================================================
   AXP2101 battery (PMIC on same I2C bus, address 0x34)
   ========================================================================= */
static XPowersAXP2101 PMU;
static bool g_pmu_ok = false;

static void battery_init() {
    /* Wire is already initialised by display_init(); pass -1 to skip re-init */
    g_pmu_ok = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, -1, -1);
    Serial.printf("[battery] AXP2101: %s\n", g_pmu_ok ? "found" : "not found");
}

static int battery_percent() {
    if (!g_pmu_ok) return -1;
    return PMU.getBatteryPercent();
}

/* =========================================================================
   Boot button — GPIO0, active-low, toggles mode
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
   Touch gesture — swipe up = tighten (close), swipe down = loosen (far)
   ========================================================================= */
static void gesture_poll() {
    TouchGesture g = display_pop_gesture();
    if (g == GESTURE_NONE) return;

    int cur  = counter_get_rssi();
    int next = (g == GESTURE_SWIPE_UP)
               ? min(RSSI_MAX, cur + RSSI_STEP)   /* tighter → close */
               : max(RSSI_MIN, cur - RSSI_STEP);  /* looser  → far   */

    if (next != cur) {
        counter_set_rssi(next);
        ui_show_rssi_overlay(next);
        ui_update_rssi_indicator(next);
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
    display_init();      /* initialises I2C */
    battery_init();      /* uses I2C — must come after display_init */
    ui_init();
    counter_init();

    ui_set_footer(counter_mode_label());
    ui_update_rssi_indicator(counter_get_rssi());
    ui_update_battery_indicator(battery_percent());

    Serial.printf("[HAXXcounter] ready  RSSI: %d dBm  battery: %d%%\n",
                  counter_get_rssi(), battery_percent());
}

static uint32_t g_last_battery_ms = 0;

void loop() {
    lv_timer_handler();
    counter_tick();
    button_poll();
    gesture_poll();
    ui_set_count(counter_get());

    /* Refresh battery indicator every 30 s */
    uint32_t now = millis();
    if (now - g_last_battery_ms >= 30'000) {
        g_last_battery_ms = now;
        ui_update_battery_indicator(battery_percent());
    }

    delay(5);
}
