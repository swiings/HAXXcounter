#pragma once
#include <Arduino.h>

/* Initialise ES8311 codec and I2S.  Must be called after Wire.begin(). */
void    audio_init();

/* Enqueue the 1.5-second three-tone alert pattern asynchronously.
 * No-op if audio is already playing. */
void    audio_play_alert();

/* True while audio is still playing. */
bool    audio_is_playing();

/* Set playback volume 0–100.  Writes to ES8311 DAC volume register.
 * Safe to call from the main loop task (uses Wire, same as touch callback). */
void    audio_set_volume(uint8_t vol);
