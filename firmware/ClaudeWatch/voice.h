#pragma once
#include <Arduino.h>

// Voice chat: hold the button -> record from the on-board mics (ES7210) -> POST PCM to the Mac
// voice server (whisper.cpp -> claude -> say) -> play the reply through the speaker (ES8311).
// The Mac server announces its URL to the board via POST /api/voice_server (stored in NVS).

enum VoiceState : uint8_t { VS_NOSERVER = 0, VS_IDLE, VS_RECORDING, VS_SENDING, VS_PLAYING, VS_ERROR };

struct VoiceInfo {
  VoiceState state;
  float seconds;        // recording length so far / total
  char text[240];       // what whisper heard
  char reply[400];      // Claude's answer
  char error[64];
  uint32_t seq;         // bumps on every change
};

#define VOICE_SAMPLE_RATE 16000
#define VOICE_MAX_SECONDS 15

bool voice_init();                 // ES7210 setup; requires audio_init() first (shared I2S clocks)
bool voice_mic_ok();
void voice_press();                // start recording (no-op if busy / no server)
void voice_release();              // stop recording and send
void voice_cancel();
void voice_get(VoiceInfo* out);
void voice_set_server(const char* url);   // e.g. "http://192.168.31.225:8765" ("" = none)
const char* voice_server();
