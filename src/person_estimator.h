#pragma once

#include <stdint.h>

/* Experimental people estimate: groups BLE device records that appear to travel
 * together. Not thread-safe; callers must serialise access. See
 * Docs/person-counting-design.md. Weights and thresholds are uncalibrated. */

constexpr uint32_t PE_WINDOW_MS = 5'000;
/* Slot 0 is the in-progress window; slots 1..PE_HISTORY-1 are scored. */
constexpr uint8_t PE_HISTORY = 25;
constexpr uint8_t PE_MIN_SHARED_WINDOWS = 6;
constexpr uint8_t PE_MIN_RSSI_SAMPLES = 5;
constexpr uint8_t PE_CONFIRM_EVALS = 3;
constexpr uint8_t PE_MAX_GROUP_SIZE = 4;
constexpr float PE_ASSOCIATE_SCORE = 0.80f;
constexpr float PE_SEPARATE_SCORE = 0.60f;
/* Address rotation: old address must go silent within this many windows of the new one appearing. */
constexpr uint8_t PE_ROTATION_SLACK = 2;
constexpr int PE_ROTATION_MAX_RSSI_DELTA = 8;

struct PersonEstimate
{
    uint32_t devices;          /* candidate personal devices */
    uint32_t people;           /* person groups containing a phone, watch or personal device */
    uint32_t associated_pairs; /* confirmed pairs */
    uint32_t unresolved_pairs; /* pairs scoring between separate and associate */
};

typedef void (*pe_log_fn)(const char *line);

void pe_set_logger(pe_log_fn fn);
void pe_reset();
void pe_observe(uint32_t device_id, uint32_t now_ms, int8_t rssi,
                uint8_t device_class, uint8_t confidence, uint8_t vendor);
void pe_forget(uint32_t device_id);

/* Closes finished windows and re-evaluates groups. Returns true when a window closed. */
bool pe_tick(uint32_t now_ms);

PersonEstimate pe_get();

/* Group key (lowest device_id in the group), or 0 if not a candidate. */
uint32_t pe_person_id(uint32_t device_id);

/* Pair score in [0,1] from completed windows, or a negative value if evidence is insufficient. */
float pe_pair_score(uint32_t a, uint32_t b);
