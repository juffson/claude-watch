#pragma once

// ---------- Wi-Fi ----------
// Leave empty and configure at runtime over USB serial instead:
//   wifi <SSID> <PASSWORD>
// Credentials are stored in NVS (Preferences) and survive reflashing.
#define WIFI_SSID_DEFAULT ""
#define WIFI_PASS_DEFAULT ""

// ---------- Time ----------
#define TZ_INFO      "CST-8"          // Asia/Shanghai (POSIX TZ string)
#define NTP_SERVER1  "ntp.aliyun.com"
#define NTP_SERVER2  "pool.ntp.org"

// ---------- Network identity ----------
#define MDNS_HOSTNAME "claude-watch"  // reachable as http://claude-watch.local/
#define HTTP_PORT     80

// ---------- Behaviour ----------
#define STATUS_OFFLINE_AFTER_MS (10UL * 60UL * 1000UL)  // no message -> OFFLINE
#define AUTO_RETURN_TO_CLOCK_MS (90UL * 1000UL)         // idle/offline this long -> clock face

// Dynamic CPU frequency: idle at 80 MHz, 160 MHz while the Claude page animates, 240 MHz for a
// few seconds after any interaction (touch, button, page switch, wake) so the UI stays snappy.
#define CPU_MHZ_BOOST   240
#define CPU_MHZ_MID     160
#define CPU_MHZ_IDLE    80
#define CPU_BOOST_MS    3000

#define BRIGHTNESS_DAY   220
#define BRIGHTNESS_NIGHT 60
#define NIGHT_START_HOUR 23
#define NIGHT_END_HOUR   7

// ---------- Touch orientation (flip if swipes feel inverted) ----------
#define TOUCH_MIRROR_X true
#define TOUCH_MIRROR_Y true
#define TOUCH_SWAP_XY  false
