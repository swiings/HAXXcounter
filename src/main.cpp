#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "ui.h"
#include "counter.h"

/* =========================================================================
   Button
   GPIO0 is the physical BOOT button on the Waveshare ESP32-S3-Touch-AMOLED-1.8
   (GPIO9 is I2S BCLK for the audio codec — not a button).
   Active-low; internal pull-up enabled.
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
    Serial.println("[HAXXcounter] ready");
}

void loop() {
    lv_timer_handler();
    counter_tick();
    button_poll();
    ui_set_count(counter_get());
    delay(5);
}
