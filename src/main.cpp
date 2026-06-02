#include <Arduino.h>
#include <driver/gpio.h>
#include <lvgl.h>

#include "display.h"
#include "ui.h"
#include "counter.h"

/* =========================================================================
   GPIO stubs / button
   ========================================================================= */
#define PIN_BOOT_BTN 9   /* user-specified BOOT / user button, active-low */

static bool     g_btn_last    = HIGH;
static uint32_t g_btn_time_ms = 0;

static void gpio_init_all() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PIN_BOOT_BTN);
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

/* Poll the boot button with a 200 ms debounce.
 * On press: toggle the counter mode and update the footer label. */
static void button_poll() {
    bool btn = (bool)gpio_get_level((gpio_num_t)PIN_BOOT_BTN);

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

    gpio_init_all();
    display_init();
    ui_init();
    counter_init();

    /* Sync the footer label to whatever mode counter starts in */
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
