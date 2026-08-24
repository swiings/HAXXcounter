#pragma once
#include <Arduino.h>

/** HAXXcounter Build Version */
constexpr const char *HAXXCOUNTER_VERSION = "v0.13.0";

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
constexpr int RSSI_DEFAULT =  -60;
constexpr int RSSI_MIN     =  -120;
constexpr int RSSI_MAX     =  -0;  
constexpr int RSSI_STEP    =   10;

/** Per-interval discovery stats, consumed by counter_pop_stats(). */
struct CounterStats {
    uint32_t    table_size;    /* current unique MACs in dedup window */
    uint32_t    new_wifi;      /* newly-seen WiFi probe MACs this interval */
    uint32_t    new_ble;       /* newly-seen BLE MACs this interval */
    uint32_t    evicted;       /* MACs aged out of the dedup window */
    uint32_t    filtered_wifi; /* WiFi MACs seen but skipped by mode filter */
    uint32_t    filtered_ble;  /* BLE MACs seen but skipped by mode filter */
    uint8_t     channel;       /* current WiFi channel */
    int         rssi;          /* current RSSI threshold */
    CounterMode mode;
};

void counter_init();
uint32_t counter_get();
void counter_tick();
void counter_toggle_mode();
CounterMode counter_get_mode();
const char *counter_mode_label();

/** Snapshot and reset per-interval discovery counters. */
CounterStats counter_pop_stats();

/** Set the RSSI threshold (clamped to RSSI_MIN..RSSI_MAX). */
void counter_set_rssi(int dbm);

/** Returns the current RSSI threshold in dBm. */
int  counter_get_rssi();

/** Print every MAC-table entry and its age to the serial log (PWR long press). */
void counter_dump_table();
