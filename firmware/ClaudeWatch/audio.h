#pragma once
#include <Arduino.h>

// Alert tones through the on-board ES8311 codec + NS4150B amplifier (needs a speaker on the
// MX1.25 pads). All functions are no-ops when the codec is not found.
bool audio_init();                    // I2S + codec; call after Wire.begin()
bool audio_ok();
void audio_set_volume(uint8_t pct);   // 0..100
// Non-blocking: queues a tone pattern. 1 = single blip, 2 = double (Claude needs you), 3 = triple (error)
void audio_beep(uint8_t pattern);

// Shared I2S port (full duplex: ES8311 playback + ES7210 capture) for the voice module.
class I2SClass;
I2SClass& audio_i2s();
// Speaker path on/off (codec unmute + amplifier enable) for streaming playback.
void audio_output_enable(bool on);
