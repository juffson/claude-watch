#pragma once
#include <Arduino.h>

// Build the LVGL UI (call after lv_init + display/indev registration).
void ui_init();

// Call from loop() as often as possible (after lv_timer_handler()).
void ui_tick();

// Feed system info to the UI.
void ui_set_wifi(bool connected, const char* ip);
void ui_set_battery(int percent /* -1 = none */, bool charging, bool usbPower);
// Clock restored from NVS but not yet synced (NTP / host): show a small ≈ marker.
void ui_set_time_approx(bool approx);

// Force a full refresh of the status page (e.g. after a status message arrived).
void ui_status_dirty();

// Settings page changed g_settings (brightness / volume / toggles): re-apply hardware state.
void ui_on_settings_changed(void (*cb)());
// g_settings changed externally (web console): re-theme the UI and re-apply hardware state.
void ui_apply_settings();

// Image set changed (upload / delete): reload wallpaper + gallery.
void ui_images_changed();

// Side button actions: single click -> clock (home), double click -> settings page.
void ui_go_home();
void ui_show_settings();

// Debug: toggle visibility of a named element ("arc","time","date","top","mini","ring","disc","state").
// Returns false for unknown names.
bool ui_debug_toggle(const char* name);
// Debug: jump to page 0 (clock) or 1 (Claude status) without animation.
void ui_debug_page(int page);
// Debug: print gesture events to Serial when enabled.
void ui_set_gesture_log(bool on);
// Debug: "page=N animating=0/1 gestures=N last=dir" into buf.
void ui_debug_state(char* buf, size_t len);
