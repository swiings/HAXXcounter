#pragma once
#include <Arduino.h>

enum AlertMode {
    ALERT_OFF        = 0,  /* alert disabled */
    ALERT_VISUAL     = 1,  /* screen flash only */
    ALERT_SOUND_LOW  = 2,  /* flash + beep at low volume */
    ALERT_SOUND_HIGH = 3,  /* flash + beep at high volume */
};

void        alert_init();

/* Call every loop pass with the current unique-device count. */
void        alert_tick(uint32_t count);

AlertMode   alert_get_mode();

/* Cycle OFF → VISUAL → FULL → OFF. */
void        alert_cycle_mode();

/* Short plain-text label for the current mode (serial log / UI overlay). */
const char *alert_mode_label();
