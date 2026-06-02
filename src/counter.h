#pragma once
#include <Arduino.h>

/** Sliding-window duration in milliseconds (120 s) */
constexpr uint32_t DEDUP_WINDOW_MS    = 120'000;

/** How often the burst scan fires to refresh nearby device timestamps (60 s) */
constexpr uint32_t BURST_INTERVAL_MS  =  60'000;

/**
 * Start passive WiFi probe-request sniffing (channel-hopping) and
 * BLE advertisement scanning.  Populates the shared MAC table.
 * Must be called from setup() after Serial is ready.
 */
void counter_init();

/** Return number of unique MACs seen within the last DEDUP_WINDOW_MS ms. */
uint32_t counter_get();

/**
 * Housekeeping — evict stale MACs and update the channel.
 * Call from loop() on every iteration (cheap; skips work unless due).
 */
void counter_tick();
