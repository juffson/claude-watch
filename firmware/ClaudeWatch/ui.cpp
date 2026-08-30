// ClaudeWatch UI — LVGL 8.4, 466x466 round AMOLED
//
//  Tile 0: watch face  — big time, seconds ring, date, wifi/battery, Claude mini-indicator
//  Tile 1: Claude page — state ring (spinner / pulse), state word, tool, elapsed, message
//
#include "ui.h"
#include "status.h"
#include "config.h"
#include "settings.h"
#include "audio.h"
#include "net.h"
#include "images.h"
#include "voice.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

LV_FONT_DECLARE(font_mont_120);
LV_FONT_DECLARE(font_mont_64);
LV_FONT_DECLARE(font_cjk_16);   // Noto Sans SC 16px: ASCII + 3755 common hanzi (Claude's replies)
LV_IMG_DECLARE(img_ring);   // 392x392 alpha ring, tinted at runtime

// Rendering note: lv_arc / big rounded objects cost 15-20 ms per full frame on this MCU
// (per-pixel radius masks over the whole bounding box). The watch face therefore uses
// 60 tiny tick dots instead of a seconds arc, and the status ring is a pre-rendered
// alpha image; lv_arc is only used for the small spinning segment while WORKING.
#define TICKS        60
#define TICK_RADIUS  226
#define TICK_SIZE    4
#define TICK_SIZE_5  6
#define SCREEN_C     233

// ---------- palette ----------
#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xF3EFE8)
#define C_MUTED   lv_color_hex(0x8A857D)
#define C_FAINT   lv_color_hex(0x4A4740)
#define C_TRACK   lv_color_hex(0x17171A)
#define C_ORANGE  lv_color_hex(0xD97757)   // Claude
#define C_GREEN   lv_color_hex(0x3FB56F)   // idle / ok
#define C_BLUE    lv_color_hex(0x4C8DFF)   // waiting for you
#define C_RED     lv_color_hex(0xE5484D)   // error
#define C_OFFLINE lv_color_hex(0x3A3A3F)

// ---------- objects ----------
// Pages are full-screen containers; a swipe slides the next page in from the side the
// finger moved towards, cyclically (swipe right forever -> pages keep coming from the right).
#define PAGES 5
#define PAGE_CLOCK    0
#define PAGE_STATUS   1
#define PAGE_GALLERY  2
#define PAGE_VOICE    3
#define PAGE_SETTINGS 4     // not in the swipe cycle (double-click the side button)
#define SWIPE_PAGES   4     // clock -> claude -> gallery -> voice -> clock
static lv_obj_t* pages[PAGES];
static int curPage = PAGE_CLOCK;
static bool pageAnimating = false;
#define tileClock  (pages[PAGE_CLOCK])
#define tileStatus (pages[PAGE_STATUS])
// clock
static lv_obj_t *ticks[TICKS], *lblTime, *lblDate, *lblTop, *miniRow, *miniDot, *lblMini, *lblApprox;
static int tickSec = -1;
// status
static lv_obj_t *ringImg, *ring, *lblTitle, *lblState, *lblTool, *lblElapsed, *lblMsg, *lblFooter, *lblOutput;
// wallpaper + gallery (image data lives in PSRAM, see images.cpp)
static lv_obj_t *bgImg = nullptr, *galImg = nullptr, *lblGal = nullptr, *lblGalHint = nullptr;
static lv_img_dsc_t wallDsc = {}, galDsc = {};
static int wallIdx = -1, galIdx = 0;
static uint32_t wallChangedMs = 0, galChangedMs = 0;
// voice page
static lv_obj_t *micBtn, *micIcon, *lblVoiceState, *lblVoiceText, *lblVoiceReply, *lblVoiceHint;
static uint32_t voiceSeqShown = 0xFFFFFFFF;
static VoiceState voiceStateShown = (VoiceState)0xFF;

// ---------- state ----------
static bool  wifiOk = false;
static char  ipStr[20] = "";
static int   battPct = -1;
static bool  battChg = false, usbPwr = false;
static ClaudeState shownState = (ClaudeState)0xFF;
static uint32_t lastSeq = 0xFFFFFFFF;
static time_t lastSec = -1;
static uint32_t lastSlowMs = 0;
static bool onStatusTile = false;
static ClaudeState jumpPrev = CS_OFFLINE;
static bool dirty = true;

// ---------- helpers ----------
static lv_color_t state_color(ClaudeState s) {
  switch (s) {
    case CS_IDLE:    return C_GREEN;
    case CS_WORKING: return C_ORANGE;
    case CS_WAITING: return C_BLUE;
    case CS_ERROR:   return C_RED;
    default:         return C_OFFLINE;
  }
}

static const char* state_word(ClaudeState s) {
  switch (s) {
    case CS_IDLE:    return "IDLE";
    case CS_WORKING: return "WORKING";
    case CS_WAITING: return "WAITING";
    case CS_ERROR:   return "ERROR";
    default:         return "OFFLINE";
  }
}

static void fmt_dur(uint32_t secs, char* out, size_t len) {
  if (secs < 60)        snprintf(out, len, "%lus", (unsigned long)secs);
  else if (secs < 3600) snprintf(out, len, "%lum %02lus", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  else                  snprintf(out, len, "%luh %02lum", (unsigned long)(secs / 3600), (unsigned long)((secs / 60) % 60));
}

static lv_obj_t* make_arc(lv_obj_t* parent, int size, int width, lv_color_t track, lv_color_t ind) {
  lv_obj_t* a = lv_arc_create(parent);
  lv_obj_set_size(a, size, size);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 270);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, ind, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, int y, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
  return l;
}

// ---------- animations ----------
static void anim_spin_cb(void* var, int32_t v) {
  lv_arc_set_angles((lv_obj_t*)var, (uint16_t)v, (uint16_t)(v + 110));
}
static void anim_arc_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_INDICATOR);
}
static void anim_bg_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
static void anim_img_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_img_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

static void start_spin(lv_obj_t* arc, uint32_t period) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, arc);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_time(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, anim_spin_cb);
  lv_anim_start(&a);
}

static void start_pulse(lv_obj_t* obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t half) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, half);
  lv_anim_set_playback_time(&a, half);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_start(&a);
}

static void gallery_show(int idx);
static void no_gesture(lv_obj_t* o);

// ---------- page switching ----------
static void anim_x_cb(void* var, int32_t v) { lv_obj_set_x((lv_obj_t*)var, v); }

static void page_anim_ready_cb(lv_anim_t* a) {
  lv_obj_t* outgoing = (lv_obj_t*)a->var;
  lv_obj_add_flag(outgoing, LV_OBJ_FLAG_HIDDEN);
  pageAnimating = false;
}

// Slide to `next`. fromRight: the new page enters from the right edge (old one exits left).
static void switch_page(int next, bool fromRight, bool animate) {
  if (next == curPage || next < 0 || next >= PAGES) return;
  if (curPage == PAGE_VOICE) voice_cancel();
  lv_obj_t* in = pages[next];
  lv_obj_t* out = pages[curPage];
  curPage = next;
  onStatusTile = (curPage == PAGE_STATUS);

  lv_anim_del(in, NULL);
  lv_anim_del(out, NULL);
  lv_obj_clear_flag(in, LV_OBJ_FLAG_HIDDEN);
  if (!animate) {
    lv_obj_set_x(in, 0);
    lv_obj_add_flag(out, LV_OBJ_FLAG_HIDDEN);
    pageAnimating = false;
    return;
  }
  int w = LV_HOR_RES;
  lv_obj_set_x(in, fromRight ? w : -w);
  pageAnimating = true;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_time(&a, 280);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, anim_x_cb);

  lv_anim_set_var(&a, in);
  lv_anim_set_values(&a, fromRight ? w : -w, 0);
  lv_anim_start(&a);

  lv_anim_set_var(&a, out);
  lv_anim_set_values(&a, 0, fromRight ? -w : w);
  lv_anim_set_ready_cb(&a, page_anim_ready_cb);
  lv_anim_start(&a);
}

// Swipe right -> next page comes in from the right; swipe left -> from the left. Cyclic.
static bool gestureLog = false;
static uint32_t gestureCount = 0;
static lv_dir_t lastGesture = LV_DIR_NONE;

static void gesture_cb(lv_event_t* e) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  gestureCount++;
  lastGesture = d;
  if (gestureLog) Serial.printf("[gesture] dir=%d (1=L 2=R 4=T 8=B) animating=%d page=%d\n", (int)d, (int)pageAnimating, curPage);
  if (pageAnimating) return;
  // Swipes only cycle the clock <-> Claude pages. The settings page is opened with a
  // double-click on the side button and ignores swipes (so sliders never fight the gesture).
  if (curPage == PAGE_SETTINGS) return;
  if (curPage == PAGE_GALLERY && (d == LV_DIR_TOP || d == LV_DIR_BOTTOM)) {
    gallery_show(galIdx + (d == LV_DIR_TOP ? 1 : -1));   // swipe up = next, swipe down = previous
    return;
  }
  if (d == LV_DIR_RIGHT)      switch_page((curPage + 1) % SWIPE_PAGES, true, true);
  else if (d == LV_DIR_LEFT)  switch_page((curPage + SWIPE_PAGES - 1) % SWIPE_PAGES, false, true);
}

// ---------- build ----------
static void build_clock(lv_obj_t* parent) {
  // optional wallpaper (user image from flash, darkened) — created first so it stays behind
  bgImg = lv_img_create(parent);
  lv_obj_center(bgImg);
  lv_obj_clear_flag(bgImg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);

  // 60 tick dots around the rim; elapsed seconds light up in Claude orange
  for (int i = 0; i < TICKS; i++) {
    int sz = (i % 5 == 0) ? TICK_SIZE_5 : TICK_SIZE;
    float a = i * (2.0f * 3.14159265f / TICKS);
    lv_obj_t* d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_pos(d, (int)lroundf(SCREEN_C + TICK_RADIUS * sinf(a) - sz / 2.0f),
                      (int)lroundf(SCREEN_C - TICK_RADIUS * cosf(a) - sz / 2.0f));
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, C_TRACK, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ticks[i] = d;
  }
  tickSec = -1;

  lblTop = make_label(parent, &lv_font_montserrat_18, C_MUTED, -168, "");
  lv_label_set_recolor(lblTop, true);

  lblTime = make_label(parent, &font_mont_120, C_TEXT, -18, "--:--");
  lblDate = make_label(parent, &lv_font_montserrat_22, C_MUTED, 72, "");
  lblApprox = make_label(parent, &font_cjk_16, C_MUTED, 105, "≈ 未校准");
  lv_obj_add_flag(lblApprox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_letter_space(lblDate, 1, 0);

  // Claude mini indicator: dot + text
  miniRow = lv_obj_create(parent);
  lv_obj_remove_style_all(miniRow);
  lv_obj_set_size(miniRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(miniRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(miniRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(miniRow, 10, 0);
  lv_obj_clear_flag(miniRow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(miniRow, LV_ALIGN_CENTER, 0, 150);

  miniDot = lv_obj_create(miniRow);
  lv_obj_remove_style_all(miniDot);
  lv_obj_set_size(miniDot, 12, 12);
  lv_obj_set_style_radius(miniDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(miniDot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(miniDot, C_OFFLINE, 0);

  lblMini = lv_label_create(miniRow);
  lv_obj_set_style_text_font(lblMini, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(lblMini, C_MUTED, 0);
  lv_label_set_text(lblMini, "Claude offline");
}

static void build_status(lv_obj_t* parent) {
  // full ring: pre-rendered alpha image, tinted with the state colour (cheap to draw)
  ringImg = lv_img_create(parent);
  lv_img_set_src(ringImg, &img_ring);
  lv_obj_center(ringImg);
  lv_obj_set_style_img_recolor_opa(ringImg, LV_OPA_COVER, 0);
  lv_obj_set_style_img_recolor(ringImg, C_OFFLINE, 0);
  lv_obj_clear_flag(ringImg, LV_OBJ_FLAG_CLICKABLE);

  // spinning segment (WORKING only): arc indicator on top of the dimmed image ring
  ring = make_arc(parent, 392, 16, C_TRACK, C_ORANGE);
  lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);   // track comes from ringImg
  lv_arc_set_angles(ring, 0, 110);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);

  lblTitle = make_label(parent, &lv_font_montserrat_14, C_FAINT, -124, "CLAUDE CODE");
  lv_obj_set_style_text_letter_space(lblTitle, 4, 0);

  lblState   = make_label(parent, &font_mont_64, C_TEXT, -58, "OFFLINE");
  lblTool    = make_label(parent, &lv_font_montserrat_24, C_MUTED, 2, "");
  lblElapsed = make_label(parent, &lv_font_montserrat_18, C_MUTED, 34, "");

  // one-line detail (tool hint), Latin + CJK
  lblMsg = make_label(parent, &font_cjk_16, C_FAINT, 60, "");
  lv_obj_set_width(lblMsg, 270);
  lv_label_set_long_mode(lblMsg, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lblMsg, LV_TEXT_ALIGN_CENTER, 0);

  // Claude's latest reply: up to 3 wrapped lines, cut with "..."
  lblOutput = lv_label_create(parent);
  lv_obj_set_style_text_font(lblOutput, &font_cjk_16, 0);
  lv_obj_set_style_text_color(lblOutput, C_TEXT, 0);
  lv_obj_set_style_text_line_space(lblOutput, 3, 0);
  lv_obj_set_size(lblOutput, 268, 62);
  lv_label_set_long_mode(lblOutput, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lblOutput, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblOutput, "");
  lv_obj_align(lblOutput, LV_ALIGN_CENTER, 0, 110);

  lblFooter = make_label(parent, &lv_font_montserrat_14, C_FAINT, 158, "");
}

// ---------- wallpaper (clock background) ----------
static void wallpaper_apply() {
  if (!bgImg) return;
  int n = images_count();
  int idx = -1;
  if (n > 0 && g_settings.wallMode == 1) {
    idx = images_find(g_settings.wallName);
    if (idx < 0) idx = 0;
  } else if (n > 0 && g_settings.wallMode == 2) {
    idx = (wallIdx >= 0 && wallIdx < n) ? wallIdx : 0;
  }
  if (idx < 0) {
    lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    wallIdx = -1;
    return;
  }
  if (images_load(images_name(idx), &wallDsc, g_settings.wallDim)) {
    lv_img_cache_invalidate_src(NULL);
    lv_img_set_src(bgImg, &wallDsc);
    lv_obj_clear_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(bgImg);
    wallIdx = idx;
    wallChangedMs = millis();
  } else {
    lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    wallIdx = -1;
  }
}

static void wallpaper_tick() {
  if (g_settings.wallMode != 2 || images_count() < 2) return;
  if (millis() - wallChangedMs >= (uint32_t)g_settings.wallRotateMin * 60000UL) {
    wallIdx = (wallIdx + 1) % images_count();
    if (curPage == PAGE_CLOCK && !pageAnimating) wallpaper_apply();
    else { wallChangedMs = millis(); wallIdx--; if (wallIdx < 0) wallIdx = images_count() - 1; }  // retry later
  }
}

// ---------- gallery page ----------
static void gallery_show(int idx) {
  int n = images_count();
  if (n == 0) {
    lv_obj_add_flag(galImg, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lblGal, "");
    lv_label_set_text(lblGalHint, "No photos yet\nupload at claude-watch.local");
    lv_obj_clear_flag(lblGalHint, LV_OBJ_FLAG_HIDDEN);
    galIdx = 0;
    return;
  }
  if (idx < 0) idx = n - 1;
  if (idx >= n) idx = 0;
  galIdx = idx;
  lv_obj_add_flag(lblGalHint, LV_OBJ_FLAG_HIDDEN);
  if (images_load(images_name(idx), &galDsc, 0)) {
    lv_img_cache_invalidate_src(NULL);
    lv_img_set_src(galImg, &galDsc);
    lv_obj_clear_flag(galImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(galImg);
  }
  char b[16];
  snprintf(b, sizeof(b), "%d / %d", idx + 1, n);
  lv_label_set_text(lblGal, b);
  galChangedMs = millis();
}

static void build_gallery(lv_obj_t* parent) {
  galImg = lv_img_create(parent);
  lv_obj_center(galImg);
  lv_obj_clear_flag(galImg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(galImg, LV_OBJ_FLAG_HIDDEN);

  lblGalHint = make_label(parent, &lv_font_montserrat_18, C_MUTED, 0, "");
  lv_obj_set_style_text_align(lblGalHint, LV_TEXT_ALIGN_CENTER, 0);

  lblGal = make_label(parent, &lv_font_montserrat_14, C_MUTED, 200, "");
}

static void gallery_tick() {
  if (curPage != PAGE_GALLERY || pageAnimating || g_settings.galleryIntervalSec == 0 || images_count() < 2) return;
  if (millis() - galChangedMs >= (uint32_t)g_settings.galleryIntervalSec * 1000UL) gallery_show(galIdx + 1);
}

void ui_images_changed() {
  images_rescan();
  wallpaper_apply();
  gallery_show(galIdx);
}

// ---------- voice page ----------
static void mic_btn_cb(lv_event_t* e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_PRESSED) voice_press();
  else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) voice_release();
}

static void build_voice(lv_obj_t* parent) {
  lv_obj_t* title = make_label(parent, &lv_font_montserrat_14, C_FAINT, -186, "VOICE");
  lv_obj_set_style_text_letter_space(title, 4, 0);

  // what you said (top), Claude's answer (below the button)
  lblVoiceText = lv_label_create(parent);
  lv_obj_set_style_text_font(lblVoiceText, &font_cjk_16, 0);
  lv_obj_set_style_text_color(lblVoiceText, C_MUTED, 0);
  lv_obj_set_size(lblVoiceText, 300, 44);
  lv_label_set_long_mode(lblVoiceText, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lblVoiceText, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblVoiceText, "");
  lv_obj_align(lblVoiceText, LV_ALIGN_CENTER, 0, -140);

  micBtn = lv_btn_create(parent);
  lv_obj_set_size(micBtn, 132, 132);
  lv_obj_align(micBtn, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_radius(micBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(micBtn, C_TRACK, 0);
  lv_obj_set_style_bg_color(micBtn, C_ORANGE, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(micBtn, 3, 0);
  lv_obj_set_style_border_color(micBtn, C_ORANGE, 0);
  lv_obj_set_style_shadow_width(micBtn, 0, 0);
  no_gesture(micBtn);
  lv_obj_add_event_cb(micBtn, mic_btn_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(micBtn, mic_btn_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(micBtn, mic_btn_cb, LV_EVENT_PRESS_LOST, NULL);
  micIcon = lv_label_create(micBtn);
  lv_obj_set_style_text_font(micIcon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(micIcon, C_TEXT, 0);
  lv_label_set_text(micIcon, LV_SYMBOL_AUDIO);
  lv_obj_center(micIcon);

  lblVoiceState = make_label(parent, &font_cjk_16, C_MUTED, 56, "");

  lblVoiceReply = lv_label_create(parent);
  lv_obj_set_style_text_font(lblVoiceReply, &font_cjk_16, 0);
  lv_obj_set_style_text_color(lblVoiceReply, C_TEXT, 0);
  lv_obj_set_style_text_line_space(lblVoiceReply, 3, 0);
  lv_obj_set_size(lblVoiceReply, 290, 82);
  lv_label_set_long_mode(lblVoiceReply, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lblVoiceReply, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblVoiceReply, "");
  lv_obj_align(lblVoiceReply, LV_ALIGN_CENTER, 0, 118);

  lblVoiceHint = make_label(parent, &lv_font_montserrat_14, C_FAINT, 178, "");
}

static void voice_tick() {
  if (curPage != PAGE_VOICE || !micBtn) return;
  VoiceInfo v;
  voice_get(&v);
  char buf[64];
  if (v.state == VS_RECORDING) {                       // live timer
    snprintf(buf, sizeof(buf), "录音中 %.1fs  松开发送", v.seconds);
    lv_label_set_text(lblVoiceState, buf);
  }
  if (v.seq == voiceSeqShown && v.state == voiceStateShown) return;
  voiceSeqShown = v.seq;
  voiceStateShown = v.state;
  lv_color_t border = C_ORANGE;
  switch (v.state) {
    case VS_NOSERVER:
      lv_label_set_text(lblVoiceState, voice_mic_ok() ? "未连接语音服务器" : "麦克风不可用");
      lv_label_set_text(lblVoiceHint, "run host/voice_server.py on the Mac");
      border = C_OFFLINE; break;
    case VS_IDLE:
      lv_label_set_text(lblVoiceState, "按住说话");
      lv_label_set_text(lblVoiceHint, voice_server());
      break;
    case VS_RECORDING:
      lv_label_set_text(lblVoiceHint, ""); border = C_RED; break;
    case VS_SENDING:
      lv_label_set_text(lblVoiceState, "识别中…  思考中…");
      lv_label_set_text(lblVoiceHint, ""); border = C_BLUE; break;
    case VS_PLAYING:
      lv_label_set_text(lblVoiceState, "播放回答");
      border = C_GREEN; break;
    case VS_ERROR:
      snprintf(buf, sizeof(buf), "失败: %s", v.error);
      lv_label_set_text(lblVoiceState, buf);
      lv_label_set_text(lblVoiceHint, "tap and hold to retry"); border = C_RED; break;
  }
  lv_obj_set_style_border_color(micBtn, border, 0);
  lv_label_set_text(lblVoiceText, v.text[0] ? v.text : "");
  lv_label_set_text(lblVoiceReply, v.reply);
}

// ---------- settings page ----------
static lv_obj_t *sldBright, *lblBrightVal, *swDim, *sldVol, *lblVolVal, *swBeep, *lblInfo1, *lblInfo2;
static void (*settingsChangedCb)() = nullptr;

static void settings_apply() { if (settingsChangedCb) settingsChangedCb(); }

static void no_gesture(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE); }

static lv_obj_t* make_slider(lv_obj_t* parent, int y, int min, int max, int val) {
  lv_obj_t* s = lv_slider_create(parent);
  lv_obj_set_size(s, 250, 10);
  lv_obj_align(s, LV_ALIGN_CENTER, 0, y);
  lv_slider_set_range(s, min, max);
  lv_slider_set_value(s, val, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, C_ORANGE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
  lv_obj_set_ext_click_area(s, 18);
  no_gesture(s);
  return s;
}

static lv_obj_t* make_switch(lv_obj_t* parent, int y, bool on) {
  lv_obj_t* sw = lv_switch_create(parent);
  lv_obj_set_size(sw, 56, 30);
  lv_obj_align(sw, LV_ALIGN_CENTER, 100, y);
  lv_obj_set_style_bg_color(sw, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, C_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, C_TEXT, LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  no_gesture(sw);
  return sw;
}

static lv_obj_t* make_row_label(lv_obj_t* parent, int y, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(l, C_MUTED, 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_CENTER, -50, y);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_width(l, 150);
  return l;
}

static void bright_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_slider_get_value(sldBright);
  if (code == LV_EVENT_VALUE_CHANGED) {
    g_settings.brightness = v;
    char b[8]; snprintf(b, sizeof(b), "%d%%", v * 100 / 255); lv_label_set_text(lblBrightVal, b);
    settings_apply();
  } else if (code == LV_EVENT_RELEASED) {
    settings_save();
  }
}

static void vol_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_slider_get_value(sldVol);
  if (code == LV_EVENT_VALUE_CHANGED) {
    g_settings.volume = v;
    char b[8]; snprintf(b, sizeof(b), "%d%%", v); lv_label_set_text(lblVolVal, b);
    settings_apply();
  } else if (code == LV_EVENT_RELEASED) {
    settings_save();
    audio_beep(1);   // preview at the new volume
  }
}

static void dim_cb(lv_event_t* e) {
  g_settings.autoDim = lv_obj_has_state(swDim, LV_STATE_CHECKED);
  settings_save();
  settings_apply();
}

static void beep_cb(lv_event_t* e) {
  g_settings.beep = lv_obj_has_state(swBeep, LV_STATE_CHECKED);
  settings_save();
  if (g_settings.beep) audio_beep(2);
}

static void reboot_cb(lv_event_t* e) { ESP.restart(); }

static void update_settings_info() {
  if (curPage != PAGE_SETTINGS) return;
  char b[96];
  snprintf(b, sizeof(b), "%s  %s", net_connected() ? net_ssid() : "Wi-Fi off", net_connected() ? net_ip() : "");
  lv_label_set_text(lblInfo1, b);
  snprintf(b, sizeof(b), "1.75C  •  %s  •  heap %uK", audio_ok() ? "audio ok" : "no audio", (unsigned)(ESP.getFreeHeap() / 1024));
  lv_label_set_text(lblInfo2, b);
}

// discrete choices for the two timeout sliders (index -> seconds)
static const uint16_t kScreenOffSteps[] = { 0, 15, 30, 60, 120, 300, 900, 1800 };
static const uint16_t kSleepSteps[]     = { 0, 300, 600, 1800, 3600, 7200, 14400 };
static lv_obj_t *sldScreenOff, *lblScreenOffVal, *sldSleep, *lblSleepVal;

static int step_index(const uint16_t* steps, int n, uint16_t v) {
  int best = 0;
  for (int i = 0; i < n; i++) if (abs((int)steps[i] - (int)v) < abs((int)steps[best] - (int)v)) best = i;
  return best;
}
static void fmt_timeout(uint16_t sec, char* out, size_t len) {
  if (sec == 0)        snprintf(out, len, "never");
  else if (sec < 60)   snprintf(out, len, "%us", sec);
  else if (sec < 3600) snprintf(out, len, "%um", sec / 60);
  else                 snprintf(out, len, "%uh", sec / 3600);
}
static void screenoff_cb(lv_event_t* e) {
  int i = lv_slider_get_value(sldScreenOff);
  g_settings.screenOffSec = kScreenOffSteps[i];
  char b[12]; fmt_timeout(g_settings.screenOffSec, b, sizeof(b)); lv_label_set_text(lblScreenOffVal, b);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void sleep_cb(lv_event_t* e) {
  int i = lv_slider_get_value(sldSleep);
  g_settings.sleepSec = kSleepSteps[i];
  char b[12]; fmt_timeout(g_settings.sleepSec, b, sizeof(b)); lv_label_set_text(lblSleepVal, b);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static lv_obj_t* row_value(lv_obj_t* parent, int y) {
  lv_obj_t* l = make_label(parent, &lv_font_montserrat_16, C_TEXT, y, "");
  lv_obj_align(l, LV_ALIGN_CENTER, 104, y);
  return l;
}

static void build_settings(lv_obj_t* parent) {
  lv_obj_t* title = make_label(parent, &lv_font_montserrat_14, C_FAINT, -192, "SETTINGS");
  lv_obj_set_style_text_letter_space(title, 4, 0);
  char b[12];

  // brightness  (label -158, slider -134)
  make_row_label(parent, -158, LV_SYMBOL_EYE_OPEN "  Brightness");
  lblBrightVal = row_value(parent, -158);
  sldBright = make_slider(parent, -134, 10, 255, g_settings.brightness);
  lv_obj_set_width(sldBright, 230);
  lv_obj_add_event_cb(sldBright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sldBright, bright_cb, LV_EVENT_RELEASED, NULL);
  snprintf(b, sizeof(b), "%d%%", g_settings.brightness * 100 / 255); lv_label_set_text(lblBrightVal, b);

  // night dim (-102)   alert tone (-72)
  make_row_label(parent, -102, LV_SYMBOL_IMAGE "  Night dim 23-07");
  swDim = make_switch(parent, -102, g_settings.autoDim);
  lv_obj_add_event_cb(swDim, dim_cb, LV_EVENT_VALUE_CHANGED, NULL);
  make_row_label(parent, -70, LV_SYMBOL_BELL "  Alert tone");
  swBeep = make_switch(parent, -70, g_settings.beep);
  lv_obj_add_event_cb(swBeep, beep_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // volume  (label -38, slider -14)
  make_row_label(parent, -38, LV_SYMBOL_VOLUME_MAX "  Volume");
  lblVolVal = row_value(parent, -38);
  sldVol = make_slider(parent, -14, 0, 100, g_settings.volume);
  lv_obj_set_width(sldVol, 230);
  lv_obj_add_event_cb(sldVol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sldVol, vol_cb, LV_EVENT_RELEASED, NULL);
  snprintf(b, sizeof(b), "%d%%", g_settings.volume); lv_label_set_text(lblVolVal, b);

  // screen off  (label 18, slider 42)
  make_row_label(parent, 18, LV_SYMBOL_POWER "  Screen off");
  lblScreenOffVal = row_value(parent, 18);
  sldScreenOff = make_slider(parent, 42, 0, (int)(sizeof(kScreenOffSteps) / sizeof(kScreenOffSteps[0])) - 1,
                             step_index(kScreenOffSteps, sizeof(kScreenOffSteps) / sizeof(kScreenOffSteps[0]), g_settings.screenOffSec));
  lv_obj_set_width(sldScreenOff, 230);
  lv_obj_add_event_cb(sldScreenOff, screenoff_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sldScreenOff, screenoff_cb, LV_EVENT_RELEASED, NULL);
  fmt_timeout(g_settings.screenOffSec, b, sizeof(b)); lv_label_set_text(lblScreenOffVal, b);

  // deep sleep  (label 74, slider 98)
  make_row_label(parent, 74, LV_SYMBOL_BATTERY_2 "  Deep sleep");
  lblSleepVal = row_value(parent, 74);
  sldSleep = make_slider(parent, 98, 0, (int)(sizeof(kSleepSteps) / sizeof(kSleepSteps[0])) - 1,
                         step_index(kSleepSteps, sizeof(kSleepSteps) / sizeof(kSleepSteps[0]), g_settings.sleepSec));
  lv_obj_set_width(sldSleep, 230);
  lv_obj_add_event_cb(sldSleep, sleep_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sldSleep, sleep_cb, LV_EVENT_RELEASED, NULL);
  fmt_timeout(g_settings.sleepSec, b, sizeof(b)); lv_label_set_text(lblSleepVal, b);

  // info (one line) + reboot
  lblInfo1 = make_label(parent, &lv_font_montserrat_14, C_FAINT, 134, "");
  lblInfo2 = make_label(parent, &lv_font_montserrat_14, C_FAINT, 152, "");

  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 110, 32);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 184);
  lv_obj_set_style_bg_color(btn, C_TRACK, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  no_gesture(btn);
  lv_obj_add_event_cb(btn, reboot_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(btn);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bl, C_MUTED, 0);
  lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Reboot");
  lv_obj_center(bl);
}

void ui_on_settings_changed(void (*cb)()) { settingsChangedCb = cb; }

// Side button: single click -> home (clock), double click -> settings page.
void ui_go_home() {
  if (curPage == PAGE_CLOCK) return;
  switch_page(PAGE_CLOCK, false, true);
}

void ui_show_settings() {
  if (curPage == PAGE_SETTINGS) { ui_go_home(); return; }   // double-click again closes it
  update_settings_info();
  switch_page(PAGE_SETTINGS, true, true);
}

// Re-theme from g_settings: page background, accent ticks, tick visibility; sync the on-device
// settings widgets; then let the sketch re-apply brightness / volume.
void ui_apply_settings() {
  lv_color_t bg = lv_color_hex(g_settings.bgColor);
  lv_obj_set_style_bg_color(lv_scr_act(), bg, 0);
  for (int i = 0; i < PAGES; i++) lv_obj_set_style_bg_color(pages[i], bg, 0);
  for (int i = 0; i < TICKS; i++) {
    if (g_settings.showTicks) lv_obj_clear_flag(ticks[i], LV_OBJ_FLAG_HIDDEN);
    else                      lv_obj_add_flag(ticks[i], LV_OBJ_FLAG_HIDDEN);
  }
  tickSec = -1;      // force recolour with the (possibly new) accent
  lastSec = -1;      // force time text refresh (12/24h)
  wallpaper_apply();
  if (sldBright) {
    lv_slider_set_value(sldBright, g_settings.brightness, LV_ANIM_OFF);
    char b[8]; snprintf(b, sizeof(b), "%d%%", g_settings.brightness * 100 / 255); lv_label_set_text(lblBrightVal, b);
    lv_slider_set_value(sldVol, g_settings.volume, LV_ANIM_OFF);
    snprintf(b, sizeof(b), "%d%%", g_settings.volume); lv_label_set_text(lblVolVal, b);
    if (g_settings.autoDim) lv_obj_add_state(swDim, LV_STATE_CHECKED); else lv_obj_clear_state(swDim, LV_STATE_CHECKED);
    if (g_settings.beep)    lv_obj_add_state(swBeep, LV_STATE_CHECKED); else lv_obj_clear_state(swBeep, LV_STATE_CHECKED);
    lv_slider_set_value(sldScreenOff, step_index(kScreenOffSteps, sizeof(kScreenOffSteps) / sizeof(kScreenOffSteps[0]), g_settings.screenOffSec), LV_ANIM_OFF);
    fmt_timeout(g_settings.screenOffSec, b, sizeof(b)); lv_label_set_text(lblScreenOffVal, b);
    lv_slider_set_value(sldSleep, step_index(kSleepSteps, sizeof(kSleepSteps) / sizeof(kSleepSteps[0]), g_settings.sleepSec), LV_ANIM_OFF);
    fmt_timeout(g_settings.sleepSec, b, sizeof(b)); lv_label_set_text(lblSleepVal, b);
  }
  settings_apply();
}

void ui_init() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < PAGES; i++) {
    lv_obj_t* p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    if (i != curPage) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    pages[i] = p;
  }
  // swipe gestures bubble up from whatever was pressed to the screen
  lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

  build_clock(tileClock);
  build_status(tileStatus);
  build_gallery(pages[PAGE_GALLERY]);
  build_voice(pages[PAGE_VOICE]);
  build_settings(pages[PAGE_SETTINGS]);
  update_settings_info();
  ui_apply_settings();   // theme (bg / accent / ticks / 12-24h / wallpaper) from NVS
  gallery_show(0);

  ui_set_wifi(false, "");
  ui_set_battery(-1, false, false);
  dirty = true;
}

// ---------- updates ----------
static void update_top_bar() {
  char buf[96];
  const char* bsym = LV_SYMBOL_BATTERY_EMPTY;
  if (battPct >= 90) bsym = LV_SYMBOL_BATTERY_FULL;
  else if (battPct >= 60) bsym = LV_SYMBOL_BATTERY_3;
  else if (battPct >= 35) bsym = LV_SYMBOL_BATTERY_2;
  else if (battPct >= 10) bsym = LV_SYMBOL_BATTERY_1;

  uint32_t wifiCol = wifiOk ? 0x3FB56F : 0x4A4740;
  if (battPct < 0) {
    snprintf(buf, sizeof(buf), "#%06lX " LV_SYMBOL_WIFI "#   %s",
             (unsigned long)wifiCol, usbPwr ? LV_SYMBOL_USB : "");
  } else {
    snprintf(buf, sizeof(buf), "#%06lX " LV_SYMBOL_WIFI "#   %s%s %d%%",
             (unsigned long)wifiCol, battChg ? LV_SYMBOL_CHARGE " " : "", bsym, battPct);
  }
  lv_label_set_text(lblTop, buf);
}

void ui_set_time_approx(bool approx) {
  if (!lblApprox) return;
  if (approx) lv_obj_clear_flag(lblApprox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(lblApprox, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_wifi(bool connected, const char* ip) {
  wifiOk = connected;
  strlcpy(ipStr, ip ? ip : "", sizeof(ipStr));
  update_top_bar();
  dirty = true;
}

void ui_set_battery(int percent, bool charging, bool usb) {
  battPct = percent;
  battChg = charging;
  usbPwr = usb;
  update_top_bar();
}

void ui_status_dirty() { dirty = true; }

void ui_set_gesture_log(bool on) { gestureLog = on; }

void ui_debug_state(char* buf, size_t len) {
  snprintf(buf, len, "page=%d animating=%d gestures=%lu last=%d", curPage, (int)pageAnimating,
           (unsigned long)gestureCount, (int)lastGesture);
}

void ui_debug_page(int page) {
  if (page < 0 || page >= PAGES) page = PAGE_CLOCK;
  switch_page(page, true, false);
  update_settings_info();
}

bool ui_debug_toggle(const char* name) {
  if (strcmp(name, "ticks") == 0) {
    bool hidden = lv_obj_has_flag(ticks[0], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < TICKS; i++) {
      if (hidden) lv_obj_clear_flag(ticks[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ticks[i], LV_OBJ_FLAG_HIDDEN);
    }
    return true;
  }
  struct { const char* n; lv_obj_t* o; } map[] = {
    {"time", lblTime}, {"date", lblDate}, {"top", lblTop}, {"mini", miniRow},
    {"ring", ring}, {"ringimg", ringImg}, {"state", lblState},
  };
  for (auto& m : map) {
    if (strcmp(m.n, name) == 0) {
      if (lv_obj_has_flag(m.o, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(m.o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(m.o, LV_OBJ_FLAG_HIDDEN);
      return true;
    }
  }
  return false;
}

static void apply_state_visuals(ClaudeState s) {
  lv_color_t c = state_color(s);

  lv_anim_del(ring, NULL);
  lv_anim_del(ringImg, NULL);
  lv_anim_del(miniDot, NULL);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_arc_color(ring, c, LV_PART_INDICATOR);
  lv_obj_set_style_img_recolor(ringImg, c, 0);
  lv_obj_set_style_img_opa(ringImg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(miniDot, c, 0);
  lv_obj_set_style_bg_opa(miniDot, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(lblTool, c, 0);
  lv_obj_set_style_text_color(lblState, s == CS_OFFLINE ? C_MUTED : C_TEXT, 0);
  lv_label_set_text(lblState, state_word(s));

  switch (s) {
    case CS_WORKING:
      lv_obj_set_style_img_recolor(ringImg, C_TRACK, 0);   // image becomes the dim track
      lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
      start_spin(ring, 1400);
      start_pulse(miniDot, anim_bg_opa_cb, 255, 60, 700);
      break;
    case CS_WAITING:
      start_pulse(ringImg, anim_img_opa_cb, 255, 50, 550);
      start_pulse(miniDot, anim_bg_opa_cb, 255, 30, 350);
      break;
    default:
      break;
  }
}

// Claude Code's spinner vocabulary — shown while WORKING with no specific tool running.
// A new word every few seconds; order is scrambled so it doesn't read like a list.
static const char* const kWorkingWords[] = {
  "Thinking", "Bootstrapping", "Sketching", "Cogitating", "Percolating", "Brewing", "Pondering",
  "Synthesizing", "Marinating", "Reticulating", "Noodling", "Crafting", "Deliberating", "Conjuring",
  "Ruminating", "Scheming", "Tinkering", "Incubating", "Musing", "Wrangling", "Simmering", "Forging",
  "Deciphering", "Ideating", "Hatching", "Envisioning", "Elucidating", "Contemplating", "Crunching",
  "Spelunking", "Puzzling", "Manifesting", "Germinating", "Concocting", "Inferring", "Unravelling",
  "Cerebrating", "Coalescing", "Stewing", "Mulling", "Channelling", "Transmuting", "Whirring",
  "Computing", "Baking", "Wizarding", "Vibing", "Clauding", "Combobulating", "Flibbertigibbeting",
};
static const int kWorkingWordCount = sizeof(kWorkingWords) / sizeof(kWorkingWords[0]);

static const char* working_word() {
  // 3.5 s per word; stride 17 (coprime with the count) walks the table in a scrambled order
  uint32_t slot = millis() / 3500;
  return kWorkingWords[(slot * 17 + 5) % kWorkingWordCount];
}

static void update_status_texts() {
  const ClaudeStatus& st = g_claude;
  uint32_t now = millis();
  char dur[24], buf[160];
  fmt_dur((now - st.stateSinceMs) / 1000, dur, sizeof(dur));

  switch (st.state) {
    case CS_WORKING:
      if (st.tool[0]) lv_label_set_text(lblTool, st.tool);
      else { snprintf(buf, sizeof(buf), "%s...", working_word()); lv_label_set_text(lblTool, buf); }
      snprintf(buf, sizeof(buf), "running %s", dur);
      lv_label_set_text(lblElapsed, buf);
      break;
    case CS_WAITING:
      lv_label_set_text(lblTool, st.tool[0] ? st.tool : "needs you");
      snprintf(buf, sizeof(buf), "waiting %s", dur);
      lv_label_set_text(lblElapsed, buf);
      break;
    case CS_IDLE:
      lv_label_set_text(lblTool, "ready for input");
      snprintf(buf, sizeof(buf), "idle %s", dur);
      lv_label_set_text(lblElapsed, buf);
      break;
    case CS_ERROR:
      lv_label_set_text(lblTool, st.tool[0] ? st.tool : "error");
      snprintf(buf, sizeof(buf), "%s ago", dur);
      lv_label_set_text(lblElapsed, buf);
      break;
    default:  // offline
      lv_label_set_text(lblTool, "no session");
      if (!wifiOk)          lv_label_set_text(lblElapsed, "USB: wifi <ssid> <pass>");
      else                  lv_label_set_text(lblElapsed, "claude-watch.local");
      break;
  }

  if (st.state == CS_OFFLINE) {
    lv_label_set_text(lblMsg, wifiOk ? ipStr : "");
    lv_label_set_text(lblOutput, "");
    lv_label_set_text(lblFooter, "");
  } else {
    lv_label_set_text(lblMsg, st.msg);
    // Claude's last reply (arrives on Stop); while working with no reply yet, keep it empty
    lv_label_set_text(lblOutput, st.output);
    lv_obj_set_style_text_color(lblOutput, st.state == CS_IDLE ? C_TEXT : C_MUTED, 0);
    if (st.sessions > 1) snprintf(buf, sizeof(buf), "%s  •  %u sessions", st.project, st.sessions);
    else                 snprintf(buf, sizeof(buf), "%s", st.project);
    lv_label_set_text(lblFooter, buf);
  }

  // mini indicator on the watch face
  switch (st.state) {
    case CS_WORKING:
      snprintf(buf, sizeof(buf), "Claude working  •  %s", st.tool[0] ? st.tool : working_word());
      break;
    case CS_WAITING:
      snprintf(buf, sizeof(buf), "Claude needs you%s%s", st.tool[0] ? "  •  " : "", st.tool);
      break;
    case CS_IDLE:    snprintf(buf, sizeof(buf), "Claude idle"); break;
    case CS_ERROR:   snprintf(buf, sizeof(buf), "Claude error"); break;
    default:         snprintf(buf, sizeof(buf), "Claude offline"); break;
  }
  lv_label_set_text(lblMini, buf);
  lv_obj_set_style_text_color(lblMini, st.state == CS_WAITING ? C_TEXT : C_MUTED, 0);
}

static void update_clock() {
  struct timeval tvNow;
  gettimeofday(&tvNow, NULL);
  struct tm t;
  localtime_r(&tvNow.tv_sec, &t);
  bool valid = (t.tm_year + 1900) >= 2024;

  if (tvNow.tv_sec != lastSec) {
    lastSec = tvNow.tv_sec;

    // seconds ticks: only restyle the dots that changed (cheap invalidation)
    int sec = valid ? t.tm_sec : -1;
    if (sec != tickSec) {
      if (sec < 0 || tickSec < 0 || sec < tickSec) {          // reset / minute rollover
        for (int i = 0; i < TICKS; i++) lv_obj_set_style_bg_color(ticks[i], C_TRACK, 0);
        for (int i = 0; i < sec; i++) lv_obj_set_style_bg_color(ticks[i], lv_color_hex(g_settings.accentColor), 0);
      } else {
        for (int i = tickSec; i < sec; i++) lv_obj_set_style_bg_color(ticks[i], lv_color_hex(g_settings.accentColor), 0);
      }
      if (sec >= 0) lv_obj_set_style_bg_color(ticks[sec], C_TEXT, 0);   // current second: bright
      tickSec = sec;
    }
    char buf[48];
    if (valid) {
      strftime(buf, sizeof(buf), g_settings.hour24 ? "%H:%M" : "%I:%M", &t);
      if (!g_settings.hour24 && buf[0] == '0') memmove(buf, buf + 1, strlen(buf));   // "9:05" not "09:05"
      lv_label_set_text(lblTime, buf);
      strftime(buf, sizeof(buf), "%A  %b %d", &t);
      lv_label_set_text(lblDate, buf);
    } else {
      lv_label_set_text(lblTime, "--:--");
      lv_label_set_text(lblDate, wifiOk ? "syncing time..." : "no time  •  connect Wi-Fi");
    }
  }
}

void ui_tick() {
  update_clock();
  wallpaper_tick();
  gallery_tick();
  voice_tick();

  const ClaudeStatus& st = g_claude;

  if (st.state != shownState) {
    shownState = st.state;
    apply_state_visuals(st.state);
    dirty = true;
  }

  // auto-jump to the Claude page when it needs attention
  if (st.state != jumpPrev) {
    if (g_settings.autoJump && (st.state == CS_WAITING || st.state == CS_ERROR) && !pageAnimating) {
      switch_page(PAGE_STATUS, true, true);
    }
    jumpPrev = st.state;
  }
  // ...and drift back to the watch face when nothing is happening
  if (onStatusTile && !pageAnimating && (st.state == CS_IDLE || st.state == CS_OFFLINE) &&
      millis() - st.stateSinceMs > (uint32_t)g_settings.autoReturnSec * 1000UL) {
    switch_page(PAGE_CLOCK, false, true);
  }

  uint32_t now = millis();
  if (dirty || st.seq != lastSeq || now - lastSlowMs >= 1000) {
    lastSlowMs = now;
    lastSeq = st.seq;
    dirty = false;
    update_status_texts();
    update_settings_info();
  }
}
