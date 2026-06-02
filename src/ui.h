#pragma once
#include <Arduino.h>

void ui_init();
void ui_set_count(uint32_t count);
void ui_set_footer(const char *text);

/** Brief centred overlay: threshold on a far↔close bar, auto-hides 2 s. */
void ui_show_rssi_overlay(int dbm);

/** Brief centred overlay: battery % on a 0–100 bar, auto-hides 2 s. */
void ui_show_battery_overlay(int pct);

/** Update the left-edge RSSI indicator strip. */
void ui_update_rssi_indicator(int dbm);

/** Update the right-edge battery indicator strip (0–100 %). */
void ui_update_battery_indicator(int pct);
