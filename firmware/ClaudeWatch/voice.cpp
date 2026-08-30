#include "voice.h"
#include "audio.h"
#include "status.h"      // cw_json_get_str
#include "pin_config.h"
#include "es7210_reg.h"
#include <Wire.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include "ESP_I2S.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ES7210_ADDR 0x40
#define REC_MAX_SAMPLES (VOICE_SAMPLE_RATE * VOICE_MAX_SECONDS)

static bool micOk = false;
static VoiceInfo info = { VS_NOSERVER, 0, "", "", "", 0 };
static char serverUrl[64] = "";
static int16_t* rec = nullptr;          // PSRAM, mono
static volatile size_t recSamples = 0;
static volatile bool recording = false;
static volatile bool wantStop = false;
static TaskHandle_t voiceTask = nullptr;

static void set_state(VoiceState s) { info.state = s; info.seq++; }

// ---------- ES7210 (4-ch ADC) over I2C, slave mode, I2S 16-bit, MIC1+MIC2 ----------
static bool es_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}
static int es_read(uint8_t reg) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)ES7210_ADDR, 1) != 1) return -1;
  return Wire.read();
}
static bool es_update(uint8_t reg, uint8_t mask, uint8_t val) {
  int v = es_read(reg);
  if (v < 0) return false;
  return es_write(reg, (v & ~mask) | (val & mask));
}

static bool es7210_init(uint8_t gainCode /* 0..13: 0,3,..30,34.5,36,37.5 dB */) {
  if (es_read(ES7210_RESET_REG00) < 0) return false;
  bool ok = true;
  // sequence from esp_codec_dev es7210.c (open + start), slave mode
  ok &= es_write(ES7210_RESET_REG00, 0xff);
  ok &= es_write(ES7210_RESET_REG00, 0x41);
  ok &= es_write(ES7210_CLOCK_OFF_REG01, 0x3f);
  ok &= es_write(ES7210_TIME_CONTROL0_REG09, 0x30);
  ok &= es_write(ES7210_TIME_CONTROL1_REG0A, 0x30);
  ok &= es_write(ES7210_ADC12_HPF2_REG23, 0x2a);
  ok &= es_write(ES7210_ADC12_HPF1_REG22, 0x0a);
  ok &= es_write(ES7210_ADC34_HPF2_REG20, 0x0a);
  ok &= es_write(ES7210_ADC34_HPF1_REG21, 0x2a);
  ok &= es_update(ES7210_MODE_CONFIG_REG08, 0x01, 0x00);          // slave
  ok &= es_write(ES7210_ANALOG_REG40, 0x43);
  ok &= es_write(ES7210_MIC12_BIAS_REG41, 0x70);
  ok &= es_write(ES7210_MIC34_BIAS_REG42, 0x70);
  ok &= es_write(ES7210_OSR_REG07, 0x20);
  ok &= es_write(ES7210_MAINCLK_REG02, 0xc1);
  // mic select: MIC1 + MIC2 (2-channel I2S, no TDM)
  for (int i = 0; i < 4; i++) ok &= es_update(ES7210_MIC1_GAIN_REG43 + i, 0x10, 0x00);
  ok &= es_write(ES7210_MIC12_POWER_REG4B, 0xff);
  ok &= es_write(ES7210_MIC34_POWER_REG4C, 0xff);
  ok &= es_update(ES7210_CLOCK_OFF_REG01, 0x0b, 0x00);
  ok &= es_write(ES7210_MIC12_POWER_REG4B, 0x00);
  ok &= es_update(ES7210_MIC1_GAIN_REG43, 0x10, 0x10);
  ok &= es_update(ES7210_MIC1_GAIN_REG43, 0x0f, gainCode);
  ok &= es_update(ES7210_MIC2_GAIN_REG44, 0x10, 0x10);
  ok &= es_update(ES7210_MIC2_GAIN_REG44, 0x0f, gainCode);
  ok &= es_write(ES7210_SDP_INTERFACE2_REG12, 0x00);              // no TDM
  // format: I2S normal, 16 bit
  int iface = es_read(ES7210_SDP_INTERFACE1_REG11);
  if (iface < 0) return false;
  iface = (iface & 0xfc) | 0x00;         // I2S
  iface = (iface & 0x1f) | 0x60;         // 16-bit
  ok &= es_write(ES7210_SDP_INTERFACE1_REG11, iface);
  // start
  int off = es_read(ES7210_CLOCK_OFF_REG01);
  ok &= es_write(ES7210_CLOCK_OFF_REG01, off < 0 ? 0x00 : off);
  ok &= es_write(ES7210_POWER_DOWN_REG06, 0x00);
  ok &= es_write(ES7210_ANALOG_REG40, 0x43);
  ok &= es_write(ES7210_MIC1_POWER_REG47, 0x08);
  ok &= es_write(ES7210_MIC2_POWER_REG48, 0x08);
  ok &= es_write(ES7210_MIC3_POWER_REG49, 0x08);
  ok &= es_write(ES7210_MIC4_POWER_REG4A, 0x08);
  ok &= es_write(ES7210_ANALOG_REG40, 0x43);
  ok &= es_write(ES7210_RESET_REG00, 0x71);
  ok &= es_write(ES7210_RESET_REG00, 0x41);
  // unmute ADC1/2
  ok &= es_update(0x14, 0x03, 0x00);
  ok &= es_update(0x15, 0x03, 0x00);
  return ok;
}

// ---------- persistence ----------
static void load_server() {
  Preferences p;
  p.begin("cw", true);
  p.getString("voice", serverUrl, sizeof(serverUrl));
  p.end();
}

void voice_set_server(const char* url) {
  strlcpy(serverUrl, url ? url : "", sizeof(serverUrl));
  size_t n = strlen(serverUrl);
  while (n && serverUrl[n - 1] == '/') serverUrl[--n] = 0;
  Preferences p;
  p.begin("cw", false);
  p.putString("voice", serverUrl);
  p.end();
  if (info.state == VS_NOSERVER && serverUrl[0]) set_state(VS_IDLE);
  if (!serverUrl[0] && info.state == VS_IDLE) set_state(VS_NOSERVER);
  Serial.printf("[voice] server = %s\n", serverUrl[0] ? serverUrl : "(none)");
}

const char* voice_server() { return serverUrl; }

// ---------- the worker: record -> upload -> play ----------
static void voice_worker(void*) {
  I2SClass& i2s = audio_i2s();
  static int16_t frame[512 * 2];   // stereo
  recSamples = 0;
  recording = true;
  set_state(VS_RECORDING);
  // drain whatever is sitting in the RX DMA buffers so we start fresh
  for (int i = 0; i < 4; i++) i2s.readBytes((char*)frame, sizeof(frame));
  uint32_t t0 = millis();
  while (!wantStop && recSamples < REC_MAX_SAMPLES) {
    size_t got = i2s.readBytes((char*)frame, sizeof(frame));
    size_t n = got / 4;                                  // stereo frames
    for (size_t i = 0; i < n && recSamples < REC_MAX_SAMPLES; i++) rec[recSamples++] = frame[2 * i];  // MIC1 (left)
    info.seconds = (millis() - t0) / 1000.0f;
  }
  recording = false;
  info.seconds = recSamples / (float)VOICE_SAMPLE_RATE;

  if (recSamples < VOICE_SAMPLE_RATE / 2) {              // < 0.5 s: ignore
    set_state(VS_IDLE);
    voiceTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // remove DC offset + normalise a little so whisper gets a healthy signal
  int64_t sum = 0; int peak = 1;
  for (size_t i = 0; i < recSamples; i++) sum += rec[i];
  int16_t dc = (int16_t)(sum / (int64_t)recSamples);
  for (size_t i = 0; i < recSamples; i++) { int v = rec[i] - dc; rec[i] = v; if (abs(v) > peak) peak = abs(v); }
  if (peak < 8000) {
    float g = 8000.0f / peak; if (g > 8) g = 8;
    for (size_t i = 0; i < recSamples; i++) rec[i] = (int16_t)(rec[i] * g);
  }

  set_state(VS_SENDING);
  HTTPClient http;
  http.setTimeout(90000);                                // whisper + claude can take a while
  String url = String(serverUrl) + "/voice";
  if (!http.begin(url)) { strlcpy(info.error, "bad server url", sizeof(info.error)); set_state(VS_ERROR); voiceTask = nullptr; vTaskDelete(nullptr); return; }
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.POST((uint8_t*)rec, recSamples * 2);
  if (code != 200) {
    snprintf(info.error, sizeof(info.error), "server %d", code);
    http.end();
    set_state(VS_ERROR);
    voiceTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  String body = http.getString();
  http.end();
  cw_json_get_str(body.c_str(), "text", info.text, sizeof(info.text));
  cw_json_get_str(body.c_str(), "reply", info.reply, sizeof(info.reply));
  char audioPath[96] = "";
  cw_json_get_str(body.c_str(), "audio", audioPath, sizeof(audioPath));
  Serial.printf("[voice] heard: %s\n[voice] reply: %s\n", info.text, info.reply);
  info.seq++;

  if (audioPath[0]) {
    set_state(VS_PLAYING);
    HTTPClient ah;
    ah.setTimeout(20000);
    if (ah.begin(String(serverUrl) + audioPath) && ah.GET() == 200) {
      WiFiClient* s = ah.getStreamPtr();
      int remaining = ah.getSize();                      // -1 if chunked
      static int16_t mono[1024];
      static int16_t stereo[1024 * 2];
      audio_output_enable(true);
      uint32_t idle = millis();
      while (ah.connected() && (remaining > 0 || remaining == -1)) {
        int avail = s->available();
        if (avail <= 0) { if (millis() - idle > 3000) break; delay(2); continue; }
        idle = millis();
        int want = min(avail, (int)sizeof(mono));
        if (remaining > 0) want = min(want, remaining);
        int got = s->readBytes((char*)mono, want);
        if (got <= 0) break;
        int n = got / 2;
        for (int i = 0; i < n; i++) { stereo[2 * i] = mono[i]; stereo[2 * i + 1] = mono[i]; }
        i2s.write((uint8_t*)stereo, n * 4);
        if (remaining > 0) remaining -= got;
      }
      delay(80);
      audio_output_enable(false);
    }
    ah.end();
  }
  set_state(VS_IDLE);
  voiceTask = nullptr;
  vTaskDelete(nullptr);
}

// ---------- public ----------
bool voice_init() {
  load_server();
  if (!audio_ok()) { Serial.println("[voice] audio not ready, mic disabled"); return false; }
  micOk = es7210_init(10 /* 30 dB */);
  Serial.printf("[voice] ES7210 %s\n", micOk ? "ready (MIC1+MIC2, 16k/16bit)" : "not found");
  if (micOk && !rec) rec = (int16_t*)heap_caps_malloc(REC_MAX_SAMPLES * 2, MALLOC_CAP_SPIRAM);
  if (!rec) micOk = false;
  set_state(serverUrl[0] ? VS_IDLE : VS_NOSERVER);
  return micOk;
}

bool voice_mic_ok() { return micOk; }

void voice_press() {
  if (!micOk || voiceTask || !serverUrl[0] || WiFi.status() != WL_CONNECTED) return;
  wantStop = false;
  info.error[0] = 0;
  xTaskCreatePinnedToCore(voice_worker, "voice", 8192, nullptr, 3, &voiceTask, 0);
}

void voice_release() { if (recording) wantStop = true; }

void voice_cancel() { wantStop = true; }

void voice_get(VoiceInfo* out) { *out = info; }
