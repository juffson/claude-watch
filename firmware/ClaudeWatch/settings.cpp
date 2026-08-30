#include "settings.h"
#include "status.h"   // cw_json_* helpers
#include <Preferences.h>

Settings g_settings;
static Preferences prefs;

void settings_load() {
  prefs.begin("cw", true);
  g_settings.brightness    = prefs.getUChar("bright", g_settings.brightness);
  g_settings.autoDim       = prefs.getBool("autodim", g_settings.autoDim);
  g_settings.bgColor       = prefs.getUInt("bg", g_settings.bgColor);
  g_settings.accentColor   = prefs.getUInt("accent", g_settings.accentColor);
  g_settings.showTicks     = prefs.getBool("ticks", g_settings.showTicks);
  g_settings.hour24        = prefs.getBool("h24", g_settings.hour24);
  g_settings.wallMode      = prefs.getUChar("wmode", g_settings.wallMode);
  prefs.getString("wname", g_settings.wallName, sizeof(g_settings.wallName));
  g_settings.wallRotateMin = prefs.getUShort("wrot", g_settings.wallRotateMin);
  g_settings.wallDim       = prefs.getUChar("wdim", g_settings.wallDim);
  g_settings.galleryIntervalSec = prefs.getUShort("gint", g_settings.galleryIntervalSec);
  g_settings.screenOffSec  = prefs.getUShort("soff", g_settings.screenOffSec);
  g_settings.sleepSec      = prefs.getUShort("slp", g_settings.sleepSec);
  g_settings.usbAlwaysOn   = prefs.getBool("usbon", g_settings.usbAlwaysOn);
  g_settings.volume        = prefs.getUChar("vol", g_settings.volume);
  g_settings.beep          = prefs.getBool("beep", g_settings.beep);
  g_settings.autoJump      = prefs.getBool("jump", g_settings.autoJump);
  g_settings.autoReturnSec = prefs.getUShort("ret", g_settings.autoReturnSec);
  g_settings.offlineMin    = prefs.getUShort("offmin", g_settings.offlineMin);
  prefs.end();
  if (g_settings.brightness < 10) g_settings.brightness = 10;
  if (g_settings.volume > 100) g_settings.volume = 100;
  if (g_settings.offlineMin < 1) g_settings.offlineMin = 1;
  if (g_settings.wallMode > 2) g_settings.wallMode = 0;
  if (g_settings.wallDim > 90) g_settings.wallDim = 90;
  if (g_settings.wallRotateMin < 1) g_settings.wallRotateMin = 1;
}

void settings_save() {
  prefs.begin("cw", false);
  prefs.putUChar("bright", g_settings.brightness);
  prefs.putBool("autodim", g_settings.autoDim);
  prefs.putUInt("bg", g_settings.bgColor);
  prefs.putUInt("accent", g_settings.accentColor);
  prefs.putBool("ticks", g_settings.showTicks);
  prefs.putBool("h24", g_settings.hour24);
  prefs.putUChar("wmode", g_settings.wallMode);
  prefs.putString("wname", g_settings.wallName);
  prefs.putUShort("wrot", g_settings.wallRotateMin);
  prefs.putUChar("wdim", g_settings.wallDim);
  prefs.putUShort("gint", g_settings.galleryIntervalSec);
  prefs.putUShort("soff", g_settings.screenOffSec);
  prefs.putUShort("slp", g_settings.sleepSec);
  prefs.putBool("usbon", g_settings.usbAlwaysOn);
  prefs.putUChar("vol", g_settings.volume);
  prefs.putBool("beep", g_settings.beep);
  prefs.putBool("jump", g_settings.autoJump);
  prefs.putUShort("ret", g_settings.autoReturnSec);
  prefs.putUShort("offmin", g_settings.offlineMin);
  prefs.end();
}

void settings_to_json(char* buf, size_t len) {
  snprintf(buf, len,
           "{\"brightness\":%u,\"autoDim\":%s,\"bgColor\":\"#%06lX\",\"accentColor\":\"#%06lX\","
           "\"showTicks\":%s,\"hour24\":%s,"
           "\"wallMode\":%u,\"wallName\":\"%s\",\"wallRotateMin\":%u,\"wallDim\":%u,\"galleryIntervalSec\":%u,"
           "\"screenOffSec\":%u,\"sleepSec\":%u,\"usbAlwaysOn\":%s,\"volume\":%u,\"beep\":%s,"
           "\"autoJump\":%s,\"autoReturnSec\":%u,\"offlineMin\":%u}",
           g_settings.brightness, g_settings.autoDim ? "true" : "false",
           (unsigned long)g_settings.bgColor, (unsigned long)g_settings.accentColor,
           g_settings.showTicks ? "true" : "false", g_settings.hour24 ? "true" : "false",
           g_settings.wallMode, g_settings.wallName, g_settings.wallRotateMin, g_settings.wallDim,
           g_settings.galleryIntervalSec,
           g_settings.screenOffSec, g_settings.sleepSec, g_settings.usbAlwaysOn ? "true" : "false",
           g_settings.volume, g_settings.beep ? "true" : "false",
           g_settings.autoJump ? "true" : "false", g_settings.autoReturnSec, g_settings.offlineMin);
}

static bool parse_color(const char* s, uint32_t* out) {
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  char* end;
  unsigned long v = strtoul(s, &end, 16);
  if (*end) return false;
  *out = (uint32_t)v;
  return true;
}

bool settings_apply_json(const char* json) {
  bool any = false;
  long n; bool b; char s[40]; uint32_t c;
  if (cw_json_get_int(json, "brightness", &n))    { g_settings.brightness = constrain(n, 10, 255); any = true; }
  if (cw_json_get_bool(json, "autoDim", &b))      { g_settings.autoDim = b; any = true; }
  if (cw_json_get_str(json, "bgColor", s, sizeof(s)) && parse_color(s, &c))     { g_settings.bgColor = c; any = true; }
  if (cw_json_get_str(json, "accentColor", s, sizeof(s)) && parse_color(s, &c)) { g_settings.accentColor = c; any = true; }
  if (cw_json_get_bool(json, "showTicks", &b))    { g_settings.showTicks = b; any = true; }
  if (cw_json_get_bool(json, "hour24", &b))       { g_settings.hour24 = b; any = true; }
  if (cw_json_get_int(json, "wallMode", &n))      { g_settings.wallMode = constrain(n, 0, 2); any = true; }
  if (cw_json_get_str(json, "wallName", s, sizeof(s))) { strlcpy(g_settings.wallName, s, sizeof(g_settings.wallName)); any = true; }
  if (cw_json_get_int(json, "wallRotateMin", &n)) { g_settings.wallRotateMin = constrain(n, 1, 1440); any = true; }
  if (cw_json_get_int(json, "wallDim", &n))       { g_settings.wallDim = constrain(n, 0, 90); any = true; }
  if (cw_json_get_int(json, "galleryIntervalSec", &n)) { g_settings.galleryIntervalSec = constrain(n, 0, 3600); any = true; }
  if (cw_json_get_int(json, "screenOffSec", &n))  { g_settings.screenOffSec = constrain(n, 0, 86400); any = true; }
  if (cw_json_get_int(json, "sleepSec", &n))      { g_settings.sleepSec = constrain(n, 0, 86400); any = true; }
  if (cw_json_get_bool(json, "usbAlwaysOn", &b))  { g_settings.usbAlwaysOn = b; any = true; }
  if (cw_json_get_int(json, "volume", &n))        { g_settings.volume = constrain(n, 0, 100); any = true; }
  if (cw_json_get_bool(json, "beep", &b))         { g_settings.beep = b; any = true; }
  if (cw_json_get_bool(json, "autoJump", &b))     { g_settings.autoJump = b; any = true; }
  if (cw_json_get_int(json, "autoReturnSec", &n)) { g_settings.autoReturnSec = constrain(n, 5, 3600); any = true; }
  if (cw_json_get_int(json, "offlineMin", &n))    { g_settings.offlineMin = constrain(n, 1, 1440); any = true; }
  if (any) settings_save();
  return any;
}
