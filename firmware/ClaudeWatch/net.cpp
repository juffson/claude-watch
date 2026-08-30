#include "net.h"
#include "config.h"
#include "status.h"
#include "settings.h"
#include "audio.h"
#include "ui.h"
#include "images.h"
#include "voice.h"
#include "config_page.h"
#include <FFat.h>
#include <Update.h>

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <sys/time.h>
#include "esp_sntp.h"

static WebServer server(HTTP_PORT);
static Preferences prefs;
static String g_ssid, g_pass;
static char g_ipStr[20] = "";
static bool wasConnected = false;
static bool mdnsUp = false;
static volatile bool timeSynced = false;
static uint32_t lastRetryMs = 0;
static void (*statusCb)(bool) = nullptr;

static void sntp_cb(struct timeval*) { timeSynced = true; }

// ---------- HTTP handlers ----------
static void handleRoot() {
  String s;
  s.reserve(700);
  s += "ClaudeWatch (ESP32-S3-Touch-AMOLED-1.75)\n\n";
  s += "GET  /status              current status JSON\n";
  s += "POST /status  {json}      {\"state\":\"working|idle|waiting|error|offline\",\n";
  s += "                           \"tool\":\"Bash\",\"project\":\"esp32\",\"msg\":\"...\",\"sessions\":1}\n";
  s += "POST /time?epoch=N        set clock (unix seconds)\n\n";
  s += "curl -X POST http://" MDNS_HOSTNAME ".local/status -d '{\"state\":\"working\",\"tool\":\"Bash\"}'\n";
  server.send(200, "text/plain", s);
}

static void handleGetStatus() {
  char buf[1200];
  status_to_json(buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

static void handlePostStatus() {
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }
  bool changed = false;
  if (!status_apply_json(body.c_str(), &changed)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing or invalid state\"}");
    return;
  }
  Serial.printf("[http] status <- %s\n", body.c_str());
  if (statusCb) statusCb(changed);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handlePostTime() {
  String v = server.hasArg("epoch") ? server.arg("epoch") : server.arg("plain");
  long long epoch = atoll(v.c_str());
  if (epoch < 1700000000LL) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad epoch\"}");
    return;
  }
  struct timeval tv = { (time_t)epoch, 0 };
  settimeofday(&tv, nullptr);
  timeSynced = true;  // mirror to RTC in main loop
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found\n");
}

// ---------- web console ----------
static void handleConfigPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", CONFIG_PAGE_HTML);
}

static void handleGetConfig() {
  char settings[640], status[1200], out[2400];
  settings_to_json(settings, sizeof(settings));
  status_to_json(status, sizeof(status));
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
  snprintf(out, sizeof(out),
           "{\"settings\":%s,\"status\":%s,\"info\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
           "\"mdns\":\"%s.local\",\"time\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,\"audio\":%s,"
           "\"mic\":%s,\"voiceServer\":\"%s\"}}",
           settings, status, wasConnected ? "true" : "false", g_ssid.c_str(), g_ipStr, MDNS_HOSTNAME, ts,
           (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), audio_ok() ? "true" : "false",
           voice_mic_ok() ? "true" : "false", voice_server());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handlePostConfig() {
  String body = server.arg("plain");
  if (!settings_apply_json(body.c_str())) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no known keys\"}");
    return;
  }
  Serial.printf("[web] config <- %s\n", body.c_str());
  ui_apply_settings();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handlePostWifi() {
  String body = server.arg("plain");
  char ssid[33] = "", pass[65] = "";
  if (!cw_json_get_str(body.c_str(), "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }
  cw_json_get_str(body.c_str(), "pass", pass, sizeof(pass));
  server.send(200, "application/json", "{\"ok\":true}");
  delay(50);                       // let the response leave before the radio drops
  net_set_wifi(ssid, pass);
}

static void handlePostBeep() {
  audio_beep(2);
  server.send(200, "application/json", audio_ok() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no audio\"}");
}

static void handlePostReboot() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  ESP.restart();
}

// ---------- images (wallpaper / gallery) ----------
static bool uploadOk = false;

static void handleImgList() {
  String s;
  s.reserve(1024);
  s += "{\"images\":[";
  for (int i = 0; i < images_count(); i++) {
    if (i) s += ',';
    s += "{\"name\":\""; s += images_name(i); s += "\",\"size\":"; s += (int)IMG_BYTES; s += '}';
  }
  s += "],\"free\":"; s += (unsigned long)images_free_bytes();
  s += ",\"total\":"; s += (unsigned long)images_total_bytes();
  s += ",\"slotBytes\":"; s += (int)IMG_BYTES; s += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

// multipart/form-data upload: field "file", filename "<name>.bin", exactly IMG_BYTES of RGB565-BE
static void handleImgUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadOk = images_write_begin(up.filename.c_str());
    Serial.printf("[img] upload start %s -> %s\n", up.filename.c_str(), uploadOk ? "ok" : "rejected");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOk) uploadOk = images_write_chunk(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadOk) uploadOk = images_write_end();
    if (uploadOk) ui_images_changed();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    images_write_abort();
    uploadOk = false;
  }
}

static void handleImgUploadDone() {
  server.send(uploadOk ? 200 : 400, "application/json",
              uploadOk ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"upload rejected (name/size/space)\"}");
}

static void handleImgDelete() {
  String name = server.arg("name");
  bool ok = images_remove(name.c_str());
  if (ok) ui_images_changed();
  server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleImgRaw() {
  String name = server.arg("name");
  if (!images_valid_name(name.c_str())) { server.send(400, "text/plain", "bad name"); return; }
  String path = String(IMG_DIR) + "/" + name;
  File f = FFat.open(path, FILE_READ);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Cache-Control", "max-age=86400");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

// ---------- OTA firmware update over Wi-Fi ----------
// curl -F "file=@firmware/build/ClaudeWatch.ino.bin" http://claude-watch.local/api/ota   (or build.sh ota)
static bool otaOk = false;
static void handleOtaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] start %s\n", up.filename.c_str());
    otaOk = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (!otaOk) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaOk && Update.write(up.buf, up.currentSize) != up.currentSize) { Update.printError(Serial); otaOk = false; }
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaOk) otaOk = Update.end(true);
    if (!otaOk) Update.printError(Serial);
    Serial.printf("[ota] %s (%u bytes)\n", otaOk ? "done, rebooting" : "FAILED", (unsigned)up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaOk = false;
  }
}
static void handleOtaDone() {
  server.sendHeader("Connection", "close");
  server.send(otaOk ? 200 : 500, "application/json", otaOk ? "{\"ok\":true,\"rebooting\":true}" : "{\"ok\":false}");
  if (otaOk) { delay(300); ESP.restart(); }
}

// ---------- voice server registration (the Mac announces itself) ----------
static void handlePostVoiceServer() {
  String body = server.arg("plain");
  char url[64] = "";
  cw_json_get_str(body.c_str(), "url", url, sizeof(url));
  if (!url[0] && !body.startsWith("{")) strlcpy(url, body.c_str(), sizeof(url));   // plain text allowed
  if (strncmp(url, "http://", 7) != 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"url must start with http://\"}");
    return;
  }
  voice_set_server(url);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- public ----------
void net_on_status(void (*cb)(bool)) { statusCb = cb; }

void net_begin() {
  prefs.begin("cw", false);
  g_ssid = prefs.getString("ssid", WIFI_SSID_DEFAULT);
  g_pass = prefs.getString("pass", WIFI_PASS_DEFAULT);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // low-latency HTTP; we are normally on USB power

  sntp_set_time_sync_notification_cb(sntp_cb);

  server.on("/", HTTP_GET, handleConfigPage);        // web console
  server.on("/help", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/wifi", HTTP_POST, handlePostWifi);
  server.on("/api/beep", HTTP_POST, handlePostBeep);
  server.on("/api/reboot", HTTP_POST, handlePostReboot);
  server.on("/api/img", HTTP_GET, handleImgList);
  server.on("/api/img", HTTP_POST, handleImgUploadDone, handleImgUpload);
  server.on("/api/img", HTTP_DELETE, handleImgDelete);
  server.on("/api/img/raw", HTTP_GET, handleImgRaw);
  server.on("/api/voice_server", HTTP_POST, handlePostVoiceServer);
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.on("/status", HTTP_GET, handleGetStatus);
  server.on("/status", HTTP_POST, handlePostStatus);
  server.on("/time", HTTP_POST, handlePostTime);
  server.onNotFound(handleNotFound);
  server.begin();

  if (g_ssid.length()) {
    Serial.printf("[wifi] connecting to \"%s\"\n", g_ssid.c_str());
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  } else {
    Serial.println("[wifi] no credentials. Send over USB:  wifi <ssid> <password>");
  }
}

void net_loop() {
  bool c = (WiFi.status() == WL_CONNECTED);
  if (c && !wasConnected) {
    strlcpy(g_ipStr, WiFi.localIP().toString().c_str(), sizeof(g_ipStr));
    Serial.printf("[wifi] connected  ip=%s  http://%s.local/\n", g_ipStr, MDNS_HOSTNAME);
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      mdnsUp = true;
    }
    configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  } else if (!c && wasConnected) {
    Serial.println("[wifi] disconnected");
    g_ipStr[0] = 0;
    if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  }
  wasConnected = c;

  if (c) {
    server.handleClient();
  } else if (g_ssid.length() && millis() - lastRetryMs > 20000) {
    lastRetryMs = millis();
    wl_status_t st = WiFi.status();
    if (st == WL_DISCONNECTED || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || st == WL_CONNECTION_LOST) {
      Serial.printf("[wifi] retry (status=%d)\n", (int)st);
      WiFi.disconnect();
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    }
  }
}

bool net_connected() { return wasConnected; }
const char* net_ip() { return g_ipStr; }
const char* net_ssid() { return g_ssid.c_str(); }
bool net_has_credentials() { return g_ssid.length() > 0; }

void net_set_wifi(const char* ssid, const char* pass) {
  g_ssid = ssid ? ssid : "";
  g_pass = pass ? pass : "";
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  Serial.printf("[wifi] saved \"%s\", connecting...\n", g_ssid.c_str());
  WiFi.disconnect();
  delay(100);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  lastRetryMs = millis();
}

bool net_take_time_synced() {
  if (!timeSynced) return false;
  timeSynced = false;
  return true;
}
