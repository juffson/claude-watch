// ClaudeWatch — Claude Code status display + watch face
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI AMOLED, CST9217 touch,
//        PCF85063 RTC, AXP2101 PMU)
// Toolchain: arduino-esp32 3.3.10 + libraries shipped in ../libraries (from Waveshare)
//
// Serial (USB) commands:
//   wifi <ssid> <password>     store Wi-Fi credentials and connect
//   time <unix_epoch>          set clock (also written to the RTC)
//   {"state":"working",...}    push a Claude status (same JSON as HTTP POST /status)
//   status | info | bright <0-255> | reboot | help

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include <driver/spi_master.h>
#include <soc/gpio_struct.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <WiFi.h>

#include "pin_config.h"          // Waveshare board pins (defines XPOWERS_CHIP_AXP2101)
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorPCF85063.hpp"
#include "XPowersLib.h"

#include "config.h"
#include "status.h"
#include "settings.h"
#include "audio.h"
#include "images.h"
#include "voice.h"
#include "ui.h"
#include "net.h"

#define BTN_BOOT_PIN 0   // side BOOT key, active low

// ---------------- display ----------------
// Arduino_GFX initialises the CO5300 and sends commands (polling, 40 MHz). Pixel data
// goes through our own SPI device on the same bus: 80 MHz, queued DMA transfers with a
// completion ISR, so LVGL renders the next buffer while the previous one is in flight.
// Requires LV_COLOR_16_SWAP=1 (panel byte order) so buffers can be DMA'd untouched.
static Arduino_DataBus* bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, true /*shared bus*/);
static Arduino_CO5300* gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

#define LCD_PIXEL_CLOCK_HZ 80000000
#define LVGL_BUF_LINES 40
#define PIX_CHUNK_BYTES 16384   // <= max_transfer_sz configured by Arduino_GFX (16392)
#define PIX_MAX_CHUNKS ((LCD_WIDTH * LVGL_BUF_LINES * 2 + PIX_CHUNK_BYTES - 1) / PIX_CHUNK_BYTES)

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t draw_buf;
alignas(16) static lv_color_t lvbuf1[LCD_WIDTH * LVGL_BUF_LINES];  // internal DRAM = DMA source
alignas(16) static lv_color_t lvbuf2[LCD_WIDTH * LVGL_BUF_LINES];

static spi_device_handle_t pixDev = nullptr;
static spi_transaction_ext_t pixTrans[PIX_MAX_CHUNKS];
static volatile int pixPending = 0;
static volatile bool pixBusy = false;
static uint32_t pixStartUs = 0;

// perf counters (serial command `perf`)
static volatile uint32_t perfFlushUs = 0, perfFlushPx = 0, perfFlushes = 0;

static void IRAM_ATTR pix_post_cb(spi_transaction_t*) {
  if (--pixPending <= 0) {
    GPIO.out_w1ts = (1UL << LCD_CS);          // CS high: burst finished
    perfFlushUs += (uint32_t)esp_timer_get_time() - pixStartUs;
    pixBusy = false;
    lv_disp_flush_ready(&disp_drv);           // LVGL may now reuse this buffer
  }
}

// Block until the in-flight DMA burst has finished (any command to the panel must wait).
static void pix_wait_idle() {
  uint32_t t0 = millis();
  while (pixBusy) {
    if (millis() - t0 > 200) {                // should never happen; don't hang the UI
      GPIO.out_w1ts = (1UL << LCD_CS);
      pixBusy = false;
      lv_disp_flush_ready(&disp_drv);
      Serial.println("[lcd] DMA timeout");
      break;
    }
  }
}

static void pix_init() {
  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 8;
  devcfg.address_bits = 24;
  devcfg.dummy_bits = 0;
  devcfg.mode = 0;
  devcfg.clock_source = SPI_CLK_SRC_DEFAULT;
  devcfg.clock_speed_hz = LCD_PIXEL_CLOCK_HZ;
  devcfg.spics_io_num = -1;                   // CS driven manually (same as Arduino_GFX)
  devcfg.flags = SPI_DEVICE_HALFDUPLEX;
  devcfg.queue_size = PIX_MAX_CHUNKS + 1;
  devcfg.post_cb = pix_post_cb;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &pixDev));
}

static void disp_rounder(lv_disp_drv_t*, lv_area_t* area) {
  // CO5300 needs even-aligned windows
  if (area->x1 & 1) area->x1--;
  if (area->y1 & 1) area->y1--;
  if (!(area->x2 & 1)) area->x2++;
  if (!(area->y2 & 1)) area->y2++;
}

static void disp_flush(lv_disp_drv_t*, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  uint32_t len = w * h * 2;

  pix_wait_idle();                            // previous buffer fully sent, CS already high
  gfx->startWrite();
  gfx->writeAddrWindow(area->x1, area->y1, w, h);   // CASET/PASET via Arduino_GFX (polling)
  gfx->endWrite();

  int n = (len + PIX_CHUNK_BYTES - 1) / PIX_CHUNK_BYTES;
  pixPending = n;
  pixBusy = true;
  pixStartUs = (uint32_t)esp_timer_get_time();
  perfFlushPx += w * h;
  perfFlushes++;

  GPIO.out_w1tc = (1UL << LCD_CS);            // CS low for the whole burst
  uint8_t* p = (uint8_t*)color_p;
  for (int i = 0; i < n; i++) {
    uint32_t l = len > PIX_CHUNK_BYTES ? PIX_CHUNK_BYTES : len;
    spi_transaction_ext_t* t = &pixTrans[i];
    memset(t, 0, sizeof(*t));
    t->base.cmd = 0x32;                       // RAMWR over QSPI: 1-line cmd+addr, 4-line data
    t->base.addr = 0x003C00;
    t->base.flags = SPI_TRANS_MODE_QIO |
                    (i ? (SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY) : 0);
    t->base.tx_buffer = p;
    t->base.length = l * 8;
    ESP_ERROR_CHECK(spi_device_queue_trans(pixDev, &t->base, portMAX_DELAY));
    p += l;
    len -= l;
  }
  // lv_disp_flush_ready() is called from pix_post_cb when the last chunk completes
}

static void lvgl_tick_cb(void*) { lv_tick_inc(2); }

// ---------------- touch ----------------
static TouchDrvCST92xx touch;
static bool touchOk = false;
static volatile bool touchIrq = false;
static void IRAM_ATTR touch_isr() { touchIrq = true; }

static bool touchLog = false;          // serial cmd `touchlog`
static uint32_t touchPresses = 0, touchIrqs = 0;

static void touch_read(lv_indev_drv_t*, lv_indev_data_t* data) {
  static bool pressed = false;
  static int16_t lx = 0, ly = 0;
  if (touchOk && (touchIrq || pressed)) {
    if (touchIrq) touchIrqs++;
    touchIrq = false;
    int16_t x[1], y[1];
    uint8_t n = touch.getPoint(x, y, 1);
    if (n > 0) {
      if (!pressed) { touchPresses++; activity_touch(); if (touchLog) Serial.printf("[touch] press  x=%d y=%d\n", x[0], y[0]); }
      pressed = true; lx = x[0]; ly = y[0];
    } else {
      if (pressed && touchLog) Serial.printf("[touch] release x=%d y=%d\n", lx, ly);
      pressed = false;
    }
  }
  data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  data->point.x = lx;
  data->point.y = ly;
}

// ---------------- RTC / PMU ----------------
static SensorPCF85063 rtc;
static XPowersPMU power;
static bool rtcOk = false, pmuOk = false;

static bool rtc_to_system() {
  if (!rtcOk) return false;
  RTC_DateTime dt = rtc.getDateTime();
  if (dt.getYear() < 2024 || dt.getYear() > 2099) return false;
  struct tm t = {};
  t.tm_year = dt.getYear() - 1900;
  t.tm_mon  = dt.getMonth() - 1;
  t.tm_mday = dt.getDay();
  t.tm_hour = dt.getHour();
  t.tm_min  = dt.getMinute();
  t.tm_sec  = dt.getSecond();
  t.tm_isdst = 0;
  time_t e = mktime(&t);  // RTC holds local time; TZ is already set
  struct timeval tv = { e, 0 };
  settimeofday(&tv, nullptr);
  return true;
}

static void system_to_rtc() {
  if (!rtcOk) return;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  rtc.setDateTime(RTC_DateTime(t));
}

// ---------------- timekeeping without an RTC ----------------
// The 1.75C has no RTC chip, so a real power-off loses the clock. Mitigations:
//  * the last known time is saved to NVS every 5 min and restored at boot (shown with "≈" until synced)
//  * the Mac hook sends its clock with every status push; NTP corrects when the internet is reachable
//  * "power off" is a deep sleep (long-press): the ESP32's RTC timer keeps running, so time survives
static bool timeApprox = false;
static const char* bootTimeSrc = "none";     // what the clock looked like right after boot
static const char* bootWake = "power-on/reset";

static bool time_valid() {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  return t.tm_year + 1900 >= 2024;
}

static void time_save_nvs() {
  if (!time_valid()) return;
  Preferences p; p.begin("cw", false);
  p.putULong("epoch", (unsigned long)time(nullptr));
  p.end();
}

static void time_restore_nvs() {
  if (time_valid()) { bootTimeSrc = "kept across sleep/reset"; return; }   // RTC domain survived
  Preferences p; p.begin("cw", true);
  unsigned long saved = p.getULong("epoch", 0);
  p.end();
  if (saved > 1700000000UL) {
    struct timeval tv = { (time_t)saved, 0 };
    settimeofday(&tv, nullptr);
    timeApprox = true;                      // could be stale by however long we were powered off
    bootTimeSrc = "restored from NVS (approx)";
    Serial.printf("[time] restored last known time (approx) %lu\n", saved);
  }
}

static void time_mark_synced(const char* src) {
  timeApprox = false;
  ui_set_time_approx(false);
  time_save_nvs();
  Serial.printf("[time] synced from %s\n", src);
}

// Called by status_apply_json when the Mac sends its clock along with a status push.
void time_from_host(long long epoch) {
  if (epoch < 1700000000LL) return;
  if (!time_valid() || timeApprox) {
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);
    time_mark_synced("host");
  }
}

// ---------------- activity / screen off ----------------
static uint32_t lastActivityMs = 0;
static bool screenOff = false;
void activity_touch() { lastActivityMs = millis(); }     // called from touch/button handlers

static void screen_set(bool on);   // defined after apply_brightness

// ---------------- deep sleep ("power off") ----------------
static void enter_sleep() {
  Serial.println("[power] entering deep sleep (long press). Press BOOT or touch the screen to wake.");
  time_save_nvs();
  settings_save();
  pix_wait_idle();
  gfx->setBrightness(0);
  gfx->displayOff();
  digitalWrite(PA, LOW);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  // wake on BOOT (GPIO0) or touch interrupt (GPIO11), both active-low RTC IOs
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_BOOT_PIN) | (1ULL << TP_INT), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

// ---------------- brightness ----------------
static int brightnessOverride = -1;
static uint8_t currentBrightness = 0;

static void apply_brightness() {
  if (screenOff) return;
  uint8_t b;
  if (brightnessOverride >= 0) {
    b = brightnessOverride;
  } else {
    b = g_settings.brightness;
    if (g_settings.autoDim) {
      time_t now = time(nullptr);
      struct tm t;
      localtime_r(&now, &t);
      bool night = (t.tm_year + 1900 >= 2024) && (t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR);
      if (night && b > BRIGHTNESS_NIGHT) b = BRIGHTNESS_NIGHT;
    }
  }
  if (b != currentBrightness) {
    currentBrightness = b;
    pix_wait_idle();             // never send a command while a pixel burst is in flight
    gfx->setBrightness(b);
  }
}

// Screen off = low-power standby: panel Sleep-In, Wi-Fi modem sleep, CPU 80 MHz, LVGL paused.
// The board keeps its IP and still receives status pushes; a touch (INT flag), a button or
// Claude entering WAITING/ERROR brings everything back.
static void screen_set(bool on) {
  if (on == !screenOff) return;
  pix_wait_idle();
  if (on) {
    setCpuFrequencyMhz(240);
    WiFi.setSleep(false);
    gfx->displayOn();
    currentBrightness = 0;                 // force apply_brightness to resend the level
    screenOff = false;
    apply_brightness();
    lv_obj_invalidate(lv_scr_act());
    Serial.println("[screen] on");
  } else {
    gfx->setBrightness(0);
    gfx->displayOff();                     // DISPOFF + SLPIN
    screenOff = true;
    touchIrq = false;                      // a fresh touch will set it again -> wake
    WiFi.setSleep(true);                   // DTIM modem sleep: biggest single saving
    setCpuFrequencyMhz(80);
    Serial.println("[screen] off (inactivity) -> low-power standby");
  }
}

// ---------------- serial commands ----------------
static void print_info() {
  char js[1200];
  status_to_json(js, sizeof(js));
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char ts[40];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
  Serial.println("---- ClaudeWatch ----");
  Serial.printf("time     : %s (%s)%s\n", ts, TZ_INFO, timeApprox ? "  [approx]" : "");
  Serial.printf("boot     : %s | clock at boot: %s\n", bootWake, bootTimeSrc);
  Serial.printf("wifi     : %s  ssid=\"%s\"  ip=%s\n", net_connected() ? "connected" : "down", net_ssid(), net_ip());
  Serial.printf("mdns     : http://%s.local/\n", MDNS_HOSTNAME);
  Serial.printf("rtc      : %s   touch: %s   pmu: %s\n", rtcOk ? "ok" : "-", touchOk ? "ok" : "-", pmuOk ? "ok" : "-");
  if (pmuOk) {
    Serial.printf("battery  : %s %d%%  %dmV  charging=%d  vbus=%d\n",
                  power.isBatteryConnect() ? "present" : "none",
                  power.isBatteryConnect() ? power.getBatteryPercent() : -1,
                  power.getBattVoltage(), power.isCharging(), power.isVbusIn());
  }
  Serial.printf("heap     : %u free, psram %u free\n", ESP.getFreeHeap(), ESP.getFreePsram());
  { char dbg[96]; ui_debug_state(dbg, sizeof(dbg));
    Serial.printf("ui       : %s | touch irqs=%lu presses=%lu\n", dbg, (unsigned long)touchIrqs, (unsigned long)touchPresses); }
  Serial.printf("status   : %s\n", js);
}

static void handle_line(char* line) {
  // trim
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
  if (!n) return;

  if (line[0] == '{') {
    bool changed;
    if (status_apply_json(line, &changed)) {
      ui_status_dirty();
      Serial.println("{\"ok\":true}");
    } else {
      Serial.println("{\"ok\":false,\"error\":\"missing or invalid state\"}");
    }
    return;
  }
  if (strncmp(line, "wifi ", 5) == 0) {
    char* ssid = line + 5;
    while (*ssid == ' ') ssid++;
    char* pass = strchr(ssid, ' ');
    if (pass) { *pass = 0; pass++; while (*pass == ' ') pass++; } else pass = (char*)"";
    net_set_wifi(ssid, pass);
    return;
  }
  if (strncmp(line, "time ", 5) == 0) {
    long long epoch = atoll(line + 5);
    if (epoch < 1700000000LL) { Serial.println("bad epoch"); return; }
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);
    system_to_rtc();
    time_mark_synced("USB");
    return;
  }
  if (strncmp(line, "bright", 6) == 0) {
    int v = atoi(line + 6);
    brightnessOverride = (v >= 0 && v <= 255 && strlen(line) > 7) ? v : -1;
    apply_brightness();
    Serial.printf("[bright] %d\n", currentBrightness);
    return;
  }
  if (strcmp(line, "status") == 0) {
    char js[1200];
    status_to_json(js, sizeof(js));
    Serial.println(js);
    return;
  }
  if (strcmp(line, "info") == 0) { print_info(); return; }
  if (strcmp(line, "touchlog") == 0) {
    touchLog = !touchLog;
    ui_set_gesture_log(touchLog);
    char dbg[96]; ui_debug_state(dbg, sizeof(dbg));
    Serial.printf("[touch] log %s | irqs=%lu presses=%lu | %s\n", touchLog ? "on" : "off",
                  (unsigned long)touchIrqs, (unsigned long)touchPresses, dbg);
    return;
  }
  if (strncmp(line, "dbg ", 4) == 0) {
    Serial.printf("[dbg] toggle %s -> %s\n", line + 4, ui_debug_toggle(line + 4) ? "ok" : "unknown");
    return;
  }
  if (strncmp(line, "page ", 5) == 0) {   // page 0|1
    ui_debug_page(atoi(line + 5));
    return;
  }
  if (strcmp(line, "perf") == 0) {
    // stress test: force full-screen redraws for 3 s and report render + flush cost
    perfFlushUs = perfFlushPx = perfFlushes = 0;
    uint32_t frames = 0, t0 = millis(), renderUs = 0;
    while (millis() - t0 < 3000) {
      lv_obj_invalidate(lv_scr_act());
      uint32_t r0 = micros();
      lv_refr_now(NULL);
      renderUs += micros() - r0;
      frames++;
    }
    uint32_t total = millis() - t0;
    Serial.printf("[perf] %lu full frames in %lu ms -> %.1f fps | frame %.1f ms (flush %.1f ms = %.0f%%) | %lu flush calls, %.1f Mpx/s\n",
                  (unsigned long)frames, (unsigned long)total, frames * 1000.0f / total,
                  renderUs / 1000.0f / frames, perfFlushUs / 1000.0f / frames,
                  100.0f * perfFlushUs / renderUs, (unsigned long)perfFlushes,
                  perfFlushPx / (perfFlushUs / 1e6f) / 1e6f);
    return;
  }
  if (strcmp(line, "i2cscan") == 0) {
    Serial.print("[i2c] found:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println();
    return;
  }
  if (strcmp(line, "rtc") == 0) {
    rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
    Serial.printf("[rtc] begin -> %s\n", rtcOk ? "ok" : "fail");
    if (rtcOk) {
      RTC_DateTime dt = rtc.getDateTime();
      Serial.printf("[rtc] %04d-%02d-%02d %02d:%02d:%02d\n", dt.getYear(), dt.getMonth(), dt.getDay(), dt.getHour(), dt.getMinute(), dt.getSecond());
    }
    return;
  }
  if (strcmp(line, "reboot") == 0) { ESP.restart(); return; }
  if (strcmp(line, "sleep") == 0) { enter_sleep(); return; }
  Serial.println("commands: wifi <ssid> <pass> | time <epoch> | {json status} | status | info | bright <0-255> | reboot");
}

static void serial_poll() {
  static char buf[512];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = 0; handle_line(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;  // overflow: drop line
    }
  }
}

static void on_http_status(bool) { ui_status_dirty(); }

// ---------------- side buttons ----------------
// BOOT (GPIO0, active low) and PWR (AXP2101 power key, short-press IRQ polled over I2C).
// single click -> clock page (home), double click -> settings page.
#define DOUBLE_CLICK_MS 400
#define LONG_PRESS_MS 1500

static void button_click(uint8_t clicks) {
  activity_touch();
  Serial.printf("[btn] %s\n", clicks >= 2 ? "double -> settings" : "single -> home");
  if (screenOff) { screen_set(true); return; }          // first press only wakes the screen
  if (clicks >= 2) ui_show_settings();
  else             ui_go_home();
}

static void buttons_poll() {
  static bool bootWasDown = false;
  static uint32_t lastChangeMs = 0, firstClickMs = 0;
  static uint8_t pendingClicks = 0;
  static uint32_t lastPmuPollMs = 0;
  uint32_t now = millis();
  bool clicked = false;

  // BOOT key with debounce, count on release; holding >= LONG_PRESS_MS -> sleep
  static uint32_t bootDownMs = 0;
  static bool longFired = false;
  bool down = digitalRead(BTN_BOOT_PIN) == LOW;
  if (down != bootWasDown && now - lastChangeMs > 30) {
    lastChangeMs = now;
    bootWasDown = down;
    if (down) { bootDownMs = now; longFired = false; }
    else if (!longFired) clicked = true;
  }
  if (down && !longFired && now - bootDownMs >= LONG_PRESS_MS) { longFired = true; enter_sleep(); }
  // PWR key: short-press IRQ = click, long-press IRQ = sleep (polled over I2C every 60 ms)
  if (pmuOk && now - lastPmuPollMs >= 60) {
    lastPmuPollMs = now;
    power.getIrqStatus();
    if (power.isPekeyLongPressIrq()) { power.clearIrqStatus(); enter_sleep(); }
    if (power.isPekeyShortPressIrq()) clicked = true;
    power.clearIrqStatus();
  }

  if (clicked) {
    if (pendingClicks == 0) firstClickMs = now;
    pendingClicks++;
    if (pendingClicks >= 2) { button_click(2); pendingClicks = 0; }
  } else if (pendingClicks && now - firstClickMs > DOUBLE_CLICK_MS) {
    button_click(pendingClicks);
    pendingClicks = 0;
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] ClaudeWatch");
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) { bootWake = "deep sleep (button/touch)"; Serial.println("[boot] woke from deep sleep, clock kept"); }

  setenv("TZ", TZ_INFO, 1);
  tzset();

  Wire.begin(IIC_SDA, IIC_SCL);

  settings_load();
  time_restore_nvs();
  images_init();   // FAT partition for wallpapers / gallery (formats on first use)
  Serial.printf("[settings] brightness=%u autoDim=%d volume=%u beep=%d\n",
                g_settings.brightness, g_settings.autoDim, g_settings.volume, g_settings.beep);

  // Alert tones (ES8311 + amp; needs a speaker on the MX1.25 pads)
  audio_init();
  audio_set_volume(g_settings.volume);
  voice_init();    // ES7210 mics + voice-chat state machine (needs the I2S clocks from audio_init)

  // RTC -> system clock (so the face is right even before Wi-Fi)
  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) { delay(100); rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL); }
  // The 1.75C revision has no PCF85063: time then comes from NTP or the USB `time` command.
  Serial.printf("[rtc] %s%s\n", rtcOk ? "ok" : "absent (time via NTP / USB `time`)", rtc_to_system() ? ", time restored" : "");

  // Power management (battery telemetry)
  pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableVbusVoltageMeasure();
    power.enableSystemVoltageMeasure();
    // PWR key short press -> IRQ flag (polled in buttons_poll)
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    power.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);   // hardware cut-off only after 10 s; 1.5 s = our sleep
    power.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);
  }
  Serial.printf("[pmu] %s\n", pmuOk ? "ok" : "not found");
  pinMode(BTN_BOOT_PIN, INPUT_PULLUP);

  // Display
  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setBrightness(BRIGHTNESS_DAY);
  currentBrightness = BRIGHTNESS_DAY;

  // Touch
  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(TOUCH_SWAP_XY);
    touch.setMirrorXY(TOUCH_MIRROR_X, TOUCH_MIRROR_Y);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), touch_isr, FALLING);
  }
  Serial.printf("[touch] %s\n", touchOk ? touch.getModelName() : "not found");

  // LVGL
  pix_init();   // async pixel path (after gfx->begin() created the SPI bus)

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, lvbuf1, lvbuf2, LCD_WIDTH * LVGL_BUF_LINES);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = disp_flush;
  disp_drv.rounder_cb = disp_rounder;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t tick_args = { .callback = &lvgl_tick_cb, .name = "lvgl_tick" };
  esp_timer_handle_t tick_timer = nullptr;
  esp_timer_create(&tick_args, &tick_timer);
  esp_timer_start_periodic(tick_timer, 2000);

  ui_init();
  lastActivityMs = millis();
  ui_set_time_approx(timeApprox);
  ui_on_settings_changed([]() {
    brightnessOverride = -1;
    apply_brightness();
    audio_set_volume(g_settings.volume);
  });

  // Network
  net_on_status(on_http_status);
  net_begin();

  Serial.println("[boot] ready. type 'help' for commands");
}

void loop() {
  if (screenOff) {
    if (touchIrq) { touchIrq = false; activity_touch(); screen_set(true); }   // touch wakes the screen
  } else {
    lv_timer_handler();
  }
  net_loop();
  serial_poll();
  buttons_poll();

  if (net_take_time_synced()) {
    system_to_rtc();
    time_mark_synced("NTP");
  }

  static uint32_t lastSlow = 0;
  static bool lastWifi = false;
  uint32_t now = millis();
  if (now - lastSlow >= 1000) {
    lastSlow = now;

    if (status_tick()) ui_status_dirty();
    {
      uint32_t idle = now - lastActivityMs;
      // on USB power the watch stays on (like a phone on its dock) unless usbAlwaysOn is disabled
      bool onUsb = pmuOk && power.isVbusIn();
      bool timeoutsActive = !(g_settings.usbAlwaysOn && onUsb);
      if (timeoutsActive && g_settings.sleepSec && idle > (uint32_t)g_settings.sleepSec * 1000UL) enter_sleep();
      if (timeoutsActive && g_settings.screenOffSec && !screenOff && idle > (uint32_t)g_settings.screenOffSec * 1000UL) screen_set(false);
      if (screenOff && (idle < 1500 || !timeoutsActive)) screen_set(true);   // touch/button, or USB plugged back in
    }
    static uint16_t saveTick = 0;
    if (++saveTick >= 300) { saveTick = 0; time_save_nvs(); }

    bool w = net_connected();
    static uint32_t ipSeq = 0;
    if (w != lastWifi || (w && (ipSeq++ % 10) == 0)) {
      lastWifi = w;
      ui_set_wifi(w, net_ip());
    }

    static uint8_t battTick = 0;
    if (pmuOk && (battTick++ % 5) == 0) {
      bool present = power.isBatteryConnect();
      ui_set_battery(present ? power.getBatteryPercent() : -1, power.isCharging(), power.isVbusIn());
    }

    apply_brightness();
  }

  // alert tone when Claude starts waiting for you (or errors)
  static ClaudeState lastBeepState = CS_OFFLINE;
  if (g_claude.state != lastBeepState) {
    if ((g_claude.state == CS_WAITING || g_claude.state == CS_ERROR) && screenOff) { activity_touch(); screen_set(true); }
    if (g_settings.beep) {
      if (g_claude.state == CS_WAITING)    audio_beep(2);
      else if (g_claude.state == CS_ERROR) audio_beep(3);
    }
    lastBeepState = g_claude.state;
  }

  if (!screenOff) ui_tick();
  delay(screenOff ? 40 : 4);
}
