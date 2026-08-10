#include "alert.h"
#include "audio.h"
#include "ui.h"

static AlertMode g_mode      = ALERT_OFF;
static uint32_t  g_threshold = 0;

void alert_init() {
    g_mode      = ALERT_OFF;
    g_threshold = 0;
}

void alert_tick(uint32_t count) {
    if (g_mode == ALERT_OFF) {
        /* Keep threshold in sync while off so there is no phantom fire
         * the moment the user enables the alert. */
        g_threshold = count;
        return;
    }

    if (count < g_threshold) {
        /* Count fell — lower the threshold to match.
         * We don't fire an alert on decrease, just track down. */
        g_threshold = count;
        return;
    }

    if (count > g_threshold) {
        g_threshold = count;
        ui_trigger_flash();
        if ((g_mode == ALERT_SOUND_LOW || g_mode == ALERT_SOUND_HIGH)
            && !audio_is_playing()) {
            audio_set_volume(g_mode == ALERT_SOUND_LOW ? 45u : 85u);
            audio_play_alert();
        }
    }
}

AlertMode alert_get_mode() { return g_mode; }

void alert_cycle_mode() {
    g_mode = (AlertMode)(((int)g_mode + 1) % 4);
    Serial.printf("[alert] mode → %s\n", alert_mode_label());
}

const char *alert_mode_label() {
    switch (g_mode) {
        case ALERT_OFF:        return "ALERT: OFF";
        case ALERT_VISUAL:     return "ALERT: VISUAL";
        case ALERT_SOUND_LOW:  return "ALERT: SOUND LOW";
        case ALERT_SOUND_HIGH: return "ALERT: SOUND HIGH";
    }
    return "";
}
