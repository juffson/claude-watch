// ClaudeWatch background service: Wi-Fi (creds in NVS), HTTP API, mDNS, SNTP, USB console.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CW_MDNS_HOSTNAME "claude-watch"
#define CW_HTTP_PORT     80
#define CW_TZ            "CST-8"
#define CW_NTP_SERVER    "ntp.aliyun.com"

// Call once from app_main (after nvs_flash_init). Non-blocking.
void cw_service_start(void);

bool cw_service_wifi_connected(void);
const char *cw_service_ip(void);            // "" when not connected
const char *cw_service_ssid(void);
void cw_service_set_wifi(const char *ssid, const char *pass);   // store in NVS + reconnect

#ifdef __cplusplus
}
#endif
