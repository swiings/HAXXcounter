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

/**
 * Start passive WiFi probe-request sniffing (channel-hopping) and
 * BLE advertisement scanning.  Populates the shared MAC table.
 * Must be called from setup() after Serial is ready.
 */
void counter_init();

/** Return number of unique MACs seen within the last DEDUP_WINDOW_MS ms. */
uint32_t counter_get();

/**
 * Housekeeping — evict stale MACs, hop the WiFi channel, trigger burst scan.
 * Call from loop() on every iteration (cheap; skips work unless due).
 */
void counter_tick();

/**
 * Toggle between MODE_ALL_DEVICES and MODE_PHONE_ESTIMATE.
 * Clears the MAC table and fires an immediate burst scan so the new
 * count reflects the current environment without waiting 60 s.
 */
void counter_toggle_mode();

/** Returns the active CounterMode. */
CounterMode counter_get_mode();

/** Human-readable label for the current mode ("all devices" / "people estimate"). */
const char *counter_mode_label();
