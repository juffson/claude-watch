#pragma once
#include <Arduino.h>
#include <lvgl.h>

// User images on the on-flash FAT partition ("ffat", ~9.9 MB), directory /img.
// Files are raw 466x466 RGB565 big-endian (LV_COLOR_16_SWAP=1 byte order), 434,312 bytes each,
// produced by the web console (browser crops/scales/encodes). ~20 images fit.
#define IMG_W 466
#define IMG_H 466
#define IMG_BYTES (IMG_W * IMG_H * 2)
#define IMG_DIR "/img"
#define IMG_MAX 32

bool images_init();                              // mount FFat (formats on first use)
int  images_count();
const char* images_name(int idx);                 // "" if out of range
int  images_find(const char* name);               // -1 if absent
void images_rescan();
bool images_remove(const char* name);
size_t images_free_bytes();
size_t images_total_bytes();
bool images_valid_name(const char* name);         // [A-Za-z0-9_-]{1,24}.bin

// Load an image into an LVGL descriptor whose data lives in PSRAM (allocated once per slot).
// dimPct darkens the pixels (0 = as is, 60 = 40% brightness) — used for the clock wallpaper.
// Returns false if the file is missing / short.
bool images_load(const char* name, lv_img_dsc_t* dsc, uint8_t dimPct);

// Streaming write used by the HTTP upload handler.
bool images_write_begin(const char* name);
bool images_write_chunk(const uint8_t* data, size_t len);
bool images_write_end();                          // validates size, renames temp -> final
void images_write_abort();
