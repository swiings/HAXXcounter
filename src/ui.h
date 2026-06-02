#pragma once
#include <Arduino.h>

/**
 * Build the full-screen LVGL layout:
 *   - sci-fi background image filling the display
 *   - "HAXXcounter" label with semi-transparent backdrop at top
 *   - large white count (120 px custom font) centred
 *   - "nearby devices" label with semi-transparent backdrop at bottom
 * Must be called after display_init().
 */
void ui_init();

/** Refresh the centre counter label to show the new value. */
void ui_set_count(uint32_t count);

/** Replace the bottom label text (e.g. "all devices" / "people estimate"). */
void ui_set_footer(const char *text);
