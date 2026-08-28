#pragma once
#include <Arduino.h>
#include <atomic>

/** Auto-incremented by scripts/bump_build_number.py on every PlatformIO build */
#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
constexpr uint32_t HAXXCOUNTER_BUILD_NUMBER = BUILD_NUMBER;

#define HAXXCOUNTER_STRINGIFY_(x) #x
#define HAXXCOUNTER_STRINGIFY(x) HAXXCOUNTER_STRINGIFY_(x)

/** HAXXcounter Build Version — trailing component is HAXXCOUNTER_BUILD_NUMBER */
constexpr const char *HAXXCOUNTER_VERSION = "v0.13." HAXXCOUNTER_STRINGIFY(BUILD_NUMBER);

/** Sliding-window duration in milliseconds (60 s) */
constexpr uint32_t DEDUP_WINDOW_MS   = 60'000;

/** How often the burst scan fires to refresh nearby device timestamps (60 s) */
constexpr uint32_t BURST_INTERVAL_MS =  60'000;

/** WiFi/BLE scanning mode — All devices or just people devices */
enum class ScanMode {
    ALL_DEVICES,
    PEOPLE_DEVICES
};
extern ScanMode CURRENT_SCAN_MODE;

/** RSSI threshold — frames weaker than this value are ignored.
 *  -50 = same room only  |  -70 = medium (default)  |  -90 = through walls */
constexpr int RSSI_DEFAULT =  -110;
constexpr int RSSI_MIN     =  -110;
constexpr int RSSI_MAX     =  -50;  
constexpr int RSSI_STEP    =   10;

/** show/hide the duplicate messages in the serial log */
constexpr bool SHOW_DUPLICATES = false;

/** Per-interval discovery stats, consumed by counter_pop_stats(). */
struct CounterStats {
    uint32_t    table_size;    /* current unique MACs in dedup window */
    uint32_t    new_wifi;      /* newly-seen WiFi probe MACs this interval */
    uint32_t    new_ble;       /* newly-seen BLE MACs this interval */
    uint32_t    live_wifi;     /* WiFi rows currently in the MAC table */
    uint32_t    live_ble;      /* BLE rows currently in the MAC table */
    uint32_t    live_phones;   /* phone rows currently in the MAC table */
    uint32_t    evicted;       /* MACs aged out of the dedup window */
    uint32_t    filtered_wifi; /* WiFi MACs seen but skipped by mode filter */
    uint32_t    filtered_ble;  /* BLE MACs seen but skipped by mode filter */
    uint8_t     channel;       /* current WiFi channel */
    int         rssi;          /* current RSSI threshold */
};

void counter_init();
uint32_t counter_get();
void counter_tick();
const char *counter_mode_label();

/** Snapshot and reset per-interval discovery counters. */
CounterStats counter_pop_stats();

/** Set the RSSI threshold (clamped to RSSI_MIN..RSSI_MAX). */
void counter_set_rssi(int dbm);

/** Returns the current RSSI threshold in dBm. */
int  counter_get_rssi();

/** Print every MAC-table entry and its age to the serial log (PWR long press). */
void counter_dump_table();
