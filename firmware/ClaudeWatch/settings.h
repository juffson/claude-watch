#pragma once
#include <Arduino.h>

// User settings, persisted in NVS (Preferences namespace "cw").
// Edited from the on-device Settings page and from the web console (http://claude-watch.local/).
struct Settings {
  // display
  uint8_t  brightness   = 220;      // 10..255 (panel brightness command)
  bool     autoDim      = true;     // dim to BRIGHTNESS_NIGHT between NIGHT_START_HOUR..NIGHT_END_HOUR
  uint32_t bgColor      = 0x000000; // watch face / page background
  uint32_t accentColor  = 0xD97757; // seconds ticks + accents (Claude orange)
  bool     showTicks    = true;     // seconds ticks on the watch face
  bool     hour24       = true;     // 24h clock (false = 12h)
  // wallpaper (watch face background from the uploaded image set) + gallery page
  uint8_t  wallMode     = 0;        // 0 off, 1 single image (wallName), 2 rotate through all
  char     wallName[32] = "";       // file name for wallMode 1
  uint16_t wallRotateMin = 30;      // minutes per image in rotate mode
  uint8_t  wallDim      = 50;       // 0..90 % darkening so the time stays readable
  uint16_t galleryIntervalSec = 0;  // gallery auto-advance seconds (0 = swipe only)
  // power
  uint16_t screenOffSec = 0;        // no touch/button for this long -> display off (0 = never); wakes on touch/button/WAITING
  uint16_t sleepSec     = 0;        // no touch/button for this long -> deep sleep (0 = never); wake with BOOT/touch
  bool     usbAlwaysOn  = true;     // ignore both timeouts while USB power is present (battery-only power saving)
  // alerts
  uint8_t  volume       = 60;       // 0..100 (ES8311 DAC volume for alert tones)
  bool     beep         = true;     // tone when Claude starts WAITING / ERROR
  // Claude behaviour
  bool     autoJump     = true;     // jump to the Claude page when WAITING / ERROR
  uint16_t autoReturnSec = 90;      // idle/offline this long on the Claude page -> back to the clock
  uint16_t offlineMin   = 10;       // no message for this long -> OFFLINE
};

extern Settings g_settings;

void settings_load();
void settings_save();
// Serialise to JSON for the web console.
void settings_to_json(char* buf, size_t len);
// Apply a (partial) JSON object; returns true if anything was accepted.
bool settings_apply_json(const char* json);
