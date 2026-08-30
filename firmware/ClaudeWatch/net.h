#pragma once
#include <Arduino.h>

void net_begin();
void net_loop();

bool net_connected();
const char* net_ip();          // "" when not connected
const char* net_ssid();
bool net_has_credentials();

// Store credentials in NVS and (re)connect.
void net_set_wifi(const char* ssid, const char* pass);

// Returns true once after every successful NTP sync (caller should mirror time to the RTC).
bool net_take_time_synced();

// Register a callback fired whenever a status message is accepted over HTTP.
void net_on_status(void (*cb)(bool stateChanged));
