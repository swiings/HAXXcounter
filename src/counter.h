#pragma once
#include <Arduino.h>

/** Sliding-window duration in milliseconds (120 s) */
constexpr uint32_t DEDUP_WINDOW_MS   = 120'000;

/** How often the burst scan fires to refresh nearby device timestamps (60 s) */
constexpr uint32_t BURST_INTERVAL_MS =  60'000;

/** Filter modes toggled by the boot button */
enum CounterMode {
    MODE_ALL_DEVICES,    /* count every visible MAC */
    MODE_PHONE_ESTIMATE  /* count only randomised MACs (phones / tablets) */
};

/** RSSI threshold — frames weaker than this value are ignored.
 *  -60 = same room only  |  -80 = medium (default)  |  -95 = through walls */
constexpr int RSSI_DEFAULT =  -80;
constexpr int RSSI_MIN     =  -95;
constexpr int RSSI_MAX     =  -60;
constexpr int RSSI_STEP    =    5;

void counter_init();
uint32_t counter_get();
void counter_tick();
void counter_toggle_mode();
CounterMode counter_get_mode();
const char *counter_mode_label();

/** Set the RSSI threshold (clamped to RSSI_MIN..RSSI_MAX). */
void counter_set_rssi(int dbm);

/** Returns the current RSSI threshold in dBm. */
int  counter_get_rssi();
