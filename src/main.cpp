#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <XPowersLib.h>
#include <esp_log.h>

#include "display.h"
#include "ui.h"
#include "counter.h"
#include "audio.h"
#include "alert.h"

/* =========================================================================
   AXP2101 battery (PMIC on same I2C bus, address 0x34)
   ========================================================================= */
static XPowersAXP2101 PMU;
static bool g_pmu_ok = false;

static int g_bat_pct = -1; /* cached — refreshed every 30 s to avoid I2C contention */
static bool g_bat_charging = false;

static int loop_delay = 500; /* main loop delay in milliseconds */
static int status_delay = 15000; /* status log interval in milliseconds (15 seconds) */

static void battery_init()
{
    /* Wire is already initialised by display_init(); pass -1 to skip re-init */
    g_pmu_ok = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, -1, -1);
    Serial.printf("%06lu %-11s P1  AXP2101: %s\n", millis() / 1000, "[battery]", g_pmu_ok ? "found" : "not found");
}

static void battery_refresh()
{
    if (!g_pmu_ok)
        return;
    g_bat_pct = PMU.getBatteryPercent();
    g_bat_charging = PMU.isCharging();
}

static int battery_percent() { return g_bat_pct; }
static bool battery_charging() { return g_bat_charging; }

/* =========================================================================
   Boot button — GPIO0, active-low, toggles counter mode
   ========================================================================= */
#define PIN_BOOT_BTN 0

static int g_btn_last = HIGH;
static uint32_t g_btn_time_ms = 0;

static void button_poll()
{
    int btn = digitalRead(PIN_BOOT_BTN);
    if (btn == LOW && g_btn_last == HIGH &&
        (millis() - g_btn_time_ms) > 200)
    {
        g_btn_time_ms = millis();
        Serial.printf("%06lu %-11s B2  top button pressed\n", millis() / 1000, "[button]"    );
    }
    g_btn_last = btn;
}

/* =========================================================================
   PWR button — AXP2101 PKEY (not a GPIO; read via PMU IRQ registers)
   Short press → cycle alert mode (OFF → VISUAL → SOUND LOW → SOUND HIGH → OFF)
   Press-and-hold (2 s) → dump MAC table to serial log
   Really long press (10 s) → native power off
   ========================================================================= */
static void pwr_button_poll()
{
    if (!g_pmu_ok)
        return;

    PMU.getIrqStatus();
    const bool short_irq = PMU.isPekeyShortPressIrq();
    const bool long_irq = PMU.isPekeyLongPressIrq();
    PMU.clearIrqStatus();

    if (long_irq)
    {
        counter_dump_table();
    }
    else if (short_irq)
    {
        alert_cycle_mode();
        ui_show_alert_overlay(alert_mode_label());
    }
}

/* =========================================================================
   Touch gesture — swipe up = tighten (close), swipe down = loosen (far)
   ========================================================================= */
static void gesture_poll()
{
    TouchGesture g = display_pop_gesture();
    if (g == GESTURE_NONE)
        return;

    int cur = counter_get_rssi();
    int next = (g == GESTURE_SWIPE_UP)
                   ? min(RSSI_MAX, cur + RSSI_STEP)  /* tighter → close */
                   : max(RSSI_MIN, cur - RSSI_STEP); /* looser  → far   */

    if (next != cur)
    {
        counter_set_rssi(next);
        ui_show_rssi_overlay(next);
        ui_update_rssi_indicator(next);
        ui_set_footer(counter_mode_label()); /* footer text embeds the dBm threshold */
    }
}

/* =========================================================================
   Arduino entry points
   ========================================================================= */
void setup()
{
    Serial.begin(115200);
    delay(200);
    /* FT3168 touch controller NACKs the first I2C read after its ~14s idle
     * low-power timeout. The code handles this gracefully; suppress the log spam. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("esp32-hal-i2c-ng.c", ESP_LOG_NONE);
    esp_log_level_set("Wire.cpp", ESP_LOG_NONE);
    Serial.printf("\n%06lu %-11s S1  HAXXcounter %s booting\n", millis() / 1000, "[status]", HAXXCOUNTER_VERSION);

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
    display_init(); /* initialises I2C */
    battery_init(); /* uses I2C — must come after display_init */
    ui_init();
    counter_init();

    audio_init();
    alert_init();

    /* Enable AXP2101 PKEY IRQs: short press cycles alert mode, 2 s hold dumps
     * the MAC table. Push power-off threshold to 10 s so accidental holds
     * don't shut down. */
    if (g_pmu_ok)
    {
        PMU.setPowerKeyPressOnTime(XPOWERS_POWERON_2S);
        PMU.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
        PMU.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    }

    battery_refresh();
    ui_set_footer(counter_mode_label());
    ui_update_rssi_indicator(counter_get_rssi());
    ui_update_battery_indicator(battery_percent(), battery_charging());

    Serial.printf("%06lu %-11s S2  HAXXcounter %s ready  RSSI: %d dBm  battery: %d%%\n",
                  millis() / 1000, "[status]", HAXXCOUNTER_VERSION, counter_get_rssi(), battery_percent());
}

static uint32_t g_last_battery_ms = 0;
static uint32_t g_last_status_ms = 0;
static uint32_t g_brightness_ms = 0;
static bool g_was_long_press = false;

void loop()
{
    lv_timer_handler();
    counter_tick();
    button_poll();
    pwr_button_poll();
    gesture_poll();
    const uint32_t cur_count = counter_get();
    ui_set_count(cur_count);
    alert_tick(cur_count);
    ui_flash_tick();

    uint32_t now = millis();

    /* Brightness — step every 400 ms while finger held stationary */
    bool lp = display_long_press_active();
    if (lp)
    {
        if (!g_was_long_press || (now - g_brightness_ms) >= 300)
        {
            g_brightness_ms = now;
            display_step_brightness();
        }
    }
    g_was_long_press = lp;

    /* Serial status line every 10 s */
    if (now - g_last_status_ms >= status_delay)
    {
        g_last_status_ms = now;
        CounterStats cs = counter_pop_stats();
        Serial.printf(
            "%06lu %-11s S3  count=%u +wifi=%u/%u +ble=%u/%u phones=%u -evicted=%u ignored=%u"
            " mode=%s ch=%u bat=%d%% %s\n",
            now / 1000, "[status]", counter_get(),
            cs.new_wifi, cs.live_wifi, cs.new_ble, cs.live_ble, cs.live_phones, cs.evicted,
            cs.filtered_wifi + cs.filtered_ble, 
            counter_mode_label(), cs.channel, battery_percent(),
            alert_mode_label());
    }

    /* Refresh battery every 30 s — single I2C read, cached for status line */
    if (now - g_last_battery_ms >= 30'000)
    {
        g_last_battery_ms = now;
        battery_refresh();
        ui_update_battery_indicator(battery_percent(), battery_charging());
    }

    delay(loop_delay);
}
