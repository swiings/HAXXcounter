#include <Arduino.h>
#include <driver/gpio.h>

#include "display.h"
#include "ui.h"
#include "counter.h"

/* =========================================================================
   GPIO stubs
   ========================================================================= */
#define PIN_BOOT_BTN 9   /* User-specified BOOT / user button */

static void gpio_stubs_init() {
    /* BOOT button — input with internal pull-up; active-low */
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PIN_BOOT_BTN);
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    /* TODO: attach an interrupt or poll in loop() to handle button press */
}

/* =========================================================================
   Arduino entry points
   ========================================================================= */
void setup() {
    Serial.begin(115200);
    delay(200);   /* allow serial monitor to attach */
    Serial.println("\n[HAXXcounter] booting");

    gpio_stubs_init();
    display_init();
    ui_init();
    counter_init();

    Serial.println("[HAXXcounter] ready");
}

void loop() {
    /* LVGL timer handler — drives animations, label redraws, etc. */
    lv_timer_handler();

    /* WiFi channel hop + MAC table eviction */
    counter_tick();

    /* Refresh the displayed count */
    ui_set_count(counter_get());

    delay(5);   /* yield ~5 ms so FreeRTOS tasks (WiFi, BLE) can run */
}
