#include "audio.h"
#include "pin_config.h"
#include <math.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define SAMPLE_RATE 16000
#define TONE_HZ     880
#define TONE_MS     110
#define GAP_MS      90

static I2SClass i2s;
static es8311_handle_t codec = nullptr;
static bool ok = false;
static QueueHandle_t beepQueue = nullptr;
static uint8_t volumePct = 60;

static void play_tone(uint32_t ms, float amp) {
  static int16_t frame[256 * 2];   // stereo
  uint32_t samples = SAMPLE_RATE * ms / 1000;
  float phase = 0, step = 2.0f * 3.14159265f * TONE_HZ / SAMPLE_RATE;
  while (samples) {
    uint32_t n = samples > 256 ? 256 : samples;
    for (uint32_t i = 0; i < n; i++) {
      // short fade in/out on each burst to avoid clicks
      float env = 1.0f;
      uint32_t idx = SAMPLE_RATE * ms / 1000 - samples + i;
      if (idx < 160) env = idx / 160.0f;
      else if (samples - i < 160) env = (samples - i) / 160.0f;
      int16_t v = (int16_t)(sinf(phase) * 12000.0f * amp * env);
      frame[2 * i] = v;
      frame[2 * i + 1] = v;
      phase += step;
      if (phase > 6.2831853f) phase -= 6.2831853f;
    }
    i2s.write((uint8_t*)frame, n * 4);
    samples -= n;
  }
}

static void play_silence(uint32_t ms) {
  static const int16_t zeros[128 * 2] = {0};
  uint32_t samples = SAMPLE_RATE * ms / 1000;
  while (samples) {
    uint32_t n = samples > 128 ? 128 : samples;
    i2s.write((uint8_t*)zeros, n * 4);
    samples -= n;
  }
}

static void audio_task(void*) {
  uint8_t pattern;
  while (true) {
    if (xQueueReceive(beepQueue, &pattern, portMAX_DELAY) != pdTRUE) continue;
    if (volumePct == 0) continue;
    es8311_voice_mute(codec, false);
    digitalWrite(PA, HIGH);
    play_silence(30);
    for (uint8_t i = 0; i < pattern; i++) {
      play_tone(TONE_MS, 0.9f);
      if (i + 1 < pattern) play_silence(GAP_MS);
    }
    play_silence(60);
    digitalWrite(PA, LOW);
    es8311_voice_mute(codec, true);
  }
}

bool audio_init() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);

  i2s.setPins(BCLKPIN, WSPIN, DIPIN /* ESP out -> codec DSDIN (GPIO8) */, DOPIN /* codec -> ESP (GPIO10) */, MCLKPIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S init failed");
    return false;
  }
  codec = es8311_create(0, ES8311_ADDRRES_0);
  if (!codec) { Serial.println("[audio] ES8311 not found"); return false; }
  const es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256, .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK ||
      es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) {
    Serial.println("[audio] ES8311 init failed");
    return false;
  }
  es8311_microphone_config(codec, false);
  es8311_voice_mute(codec, true);
  audio_set_volume(volumePct);

  beepQueue = xQueueCreate(4, sizeof(uint8_t));
  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 2, nullptr, 0);
  ok = true;
  Serial.println("[audio] ES8311 ready");
  return true;
}

bool audio_ok() { return ok; }

void audio_set_volume(uint8_t pct) {
  volumePct = pct > 100 ? 100 : pct;
  if (codec) es8311_voice_volume_set(codec, volumePct, nullptr);
}

I2SClass& audio_i2s() { return i2s; }

void audio_output_enable(bool on) {
  if (!ok) return;
  if (on) { es8311_voice_mute(codec, false); digitalWrite(PA, HIGH); }
  else    { digitalWrite(PA, LOW); es8311_voice_mute(codec, true); }
}

void audio_beep(uint8_t pattern) {
  if (!ok || !beepQueue || pattern == 0) return;
  if (pattern > 3) pattern = 3;
  xQueueSend(beepQueue, &pattern, 0);
}
