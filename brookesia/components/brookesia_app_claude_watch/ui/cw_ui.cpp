// ClaudeWatch UI — LVGL 9 port for ESP-Brookesia (466x466 round AMOLED)
//
//  Page 0: watch face  — big time, 60 seconds ticks, date, Wi-Fi, Claude mini-indicator
//  Page 1: Claude page — state ring (image, tinted) + spinner segment, state word, tool, elapsed, msg
//
// Swipe left/right flips pages cyclically (swipe right -> next page enters from the right).
// Big lv_arc / translucent discs are avoided on purpose: they cost 15-20 ms per frame here.
#include "cw_ui.h"
#include "cw_status.h"
#include "claude_watch_service.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"

LV_FONT_DECLARE(font_mont_120);
LV_FONT_DECLARE(font_mont_64);
LV_IMAGE_DECLARE(img_ring);

// ---------- palette ----------
#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xF3EFE8)
#define C_MUTED   lv_color_hex(0x8A857D)
#define C_FAINT   lv_color_hex(0x4A4740)
#define C_TRACK   lv_color_hex(0x17171A)
#define C_ORANGE  lv_color_hex(0xD97757)
#define C_GREEN   lv_color_hex(0x3FB56F)
#define C_BLUE    lv_color_hex(0x4C8DFF)
#define C_RED     lv_color_hex(0xE5484D)
#define C_OFFLINE lv_color_hex(0x3A3A3F)

#define SCREEN_W     466
#define SCREEN_C     233
#define TICKS        60
#define TICK_RADIUS  226
#define TICK_SIZE    4
#define TICK_SIZE_5  6

#define PAGES 2
#define PAGE_CLOCK  0
#define PAGE_STATUS 1
#define AUTO_RETURN_TO_CLOCK_US (90LL * 1000000)

// ---------- objects ----------
static lv_obj_t *root = nullptr;
static lv_obj_t *pages[PAGES];
static int curPage = PAGE_CLOCK;
static bool pageAnimating = false;
// clock
static lv_obj_t *ticks[TICKS], *lblTime, *lblDate, *lblTop, *miniRow, *miniDot, *lblMini;
static int tickSec = -1;
// status
static lv_obj_t *ringImg, *ring, *lblTitle, *lblState, *lblTool, *lblElapsed, *lblMsg, *lblFooter;

// ---------- state ----------
static cw_state_t shownState = (cw_state_t)0xFF;
static uint32_t lastSeq = 0xFFFFFFFF;
static time_t lastSec = -1;
static int64_t lastSlowUs = 0;
static cw_state_t jumpPrev = CS_OFFLINE;
static bool wifiShown = false;

// ---------- helpers ----------
static lv_color_t state_color(cw_state_t s) {
  switch (s) {
    case CS_IDLE:    return C_GREEN;
    case CS_WORKING: return C_ORANGE;
    case CS_WAITING: return C_BLUE;
    case CS_ERROR:   return C_RED;
    default:         return C_OFFLINE;
  }
}

static const char* state_word(cw_state_t s) {
  switch (s) {
    case CS_IDLE:    return "IDLE";
    case CS_WORKING: return "WORKING";
    case CS_WAITING: return "WAITING";
    case CS_ERROR:   return "ERROR";
    default:         return "OFFLINE";
  }
}

static void fmt_dur(uint32_t secs, char* out, size_t len) {
  if (secs < 60)        snprintf(out, len, "%us", (unsigned)secs);
  else if (secs < 3600) snprintf(out, len, "%um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
  else                  snprintf(out, len, "%uh %02um", (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60));
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
static void anim_img_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_image_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
static void anim_bg_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
static void anim_x_cb(void* var, int32_t v) { lv_obj_set_x((lv_obj_t*)var, v); }

static void start_spin(lv_obj_t* arc, uint32_t period) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, arc);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_duration(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, anim_spin_cb);
  lv_anim_start(&a);
}

static void start_pulse(lv_obj_t* obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t half) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, half);
  lv_anim_set_reverse_duration(&a, half);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_start(&a);
}

// ---------- page switching ----------
static void page_anim_ready_cb(lv_anim_t* a) {
  lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
  pageAnimating = false;
}

static void switch_page(int next, bool fromRight, bool animate) {
  if (!root || next == curPage || next < 0 || next >= PAGES) return;
  lv_obj_t* in = pages[next];
  lv_obj_t* out = pages[curPage];
  curPage = next;

  lv_anim_delete(in, NULL);
  lv_anim_delete(out, NULL);
  lv_obj_remove_flag(in, LV_OBJ_FLAG_HIDDEN);
  if (!animate) {
    lv_obj_set_x(in, 0);
    lv_obj_add_flag(out, LV_OBJ_FLAG_HIDDEN);
    pageAnimating = false;
    return;
  }
  int w = SCREEN_W;
  lv_obj_set_x(in, fromRight ? w : -w);
  pageAnimating = true;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_duration(&a, 280);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, anim_x_cb);

  lv_anim_set_var(&a, in);
  lv_anim_set_values(&a, fromRight ? w : -w, 0);
  lv_anim_start(&a);

  lv_anim_set_var(&a, out);
  lv_anim_set_values(&a, 0, fromRight ? -w : w);
  lv_anim_set_completed_cb(&a, page_anim_ready_cb);
  lv_anim_start(&a);
}

static void gesture_cb(lv_event_t* e) {
  if (pageAnimating) return;
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
  if (d == LV_DIR_RIGHT)     switch_page((curPage + 1) % PAGES, true, true);
  else if (d == LV_DIR_LEFT) switch_page((curPage + PAGES - 1) % PAGES, false, true);
}

// ---------- build ----------
static void build_clock(lv_obj_t* parent) {
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
    lv_obj_remove_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    ticks[i] = d;
  }
  tickSec = -1;

  lblTop = make_label(parent, &lv_font_montserrat_18, C_MUTED, -168, "");
  lv_label_set_recolor(lblTop, true);

  lblTime = make_label(parent, &font_mont_120, C_TEXT, -18, "--:--");
  lblDate = make_label(parent, &lv_font_montserrat_22, C_MUTED, 72, "");
  lv_obj_set_style_text_letter_space(lblDate, 1, 0);

  miniRow = lv_obj_create(parent);
  lv_obj_remove_style_all(miniRow);
  lv_obj_set_size(miniRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(miniRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(miniRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(miniRow, 10, 0);
  lv_obj_remove_flag(miniRow, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
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
  ringImg = lv_image_create(parent);
  lv_image_set_src(ringImg, &img_ring);
  lv_obj_center(ringImg);
  lv_obj_set_style_image_recolor_opa(ringImg, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(ringImg, C_OFFLINE, 0);
  lv_obj_remove_flag(ringImg, LV_OBJ_FLAG_CLICKABLE);

  ring = lv_arc_create(parent);
  lv_obj_set_size(ring, 392, 392);
  lv_obj_center(ring);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 16, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, C_ORANGE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
  lv_arc_set_angles(ring, 0, 110);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);

  lblTitle = make_label(parent, &lv_font_montserrat_14, C_FAINT, -124, "CLAUDE CODE");
  lv_obj_set_style_text_letter_space(lblTitle, 4, 0);

  lblState   = make_label(parent, &font_mont_64, C_TEXT, -40, "OFFLINE");
  lblTool    = make_label(parent, &lv_font_montserrat_26, C_MUTED, 26, "");
  lblElapsed = make_label(parent, &lv_font_montserrat_20, C_MUTED, 66, "");

  lblMsg = make_label(parent, &lv_font_montserrat_16, C_FAINT, 102, "");
  lv_obj_set_width(lblMsg, 240);
  lv_label_set_long_mode(lblMsg, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(lblMsg, LV_TEXT_ALIGN_CENTER, 0);

  lblFooter = make_label(parent, &lv_font_montserrat_16, C_FAINT, 136, "");
}

// ---------- updates ----------
static void update_top_bar() {
  char buf[64];
  snprintf(buf, sizeof(buf), "#%06lX " LV_SYMBOL_WIFI "#", (unsigned long)(wifiShown ? 0x3FB56F : 0x4A4740));
  lv_label_set_text(lblTop, buf);
}

static void apply_state_visuals(cw_state_t s) {
  lv_color_t c = state_color(s);

  lv_anim_delete(ring, NULL);
  lv_anim_delete(ringImg, NULL);
  lv_anim_delete(miniDot, NULL);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_arc_color(ring, c, LV_PART_INDICATOR);
  lv_obj_set_style_image_recolor(ringImg, c, 0);
  lv_obj_set_style_image_opa(ringImg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(miniDot, c, 0);
  lv_obj_set_style_bg_opa(miniDot, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(lblTool, c, 0);
  lv_obj_set_style_text_color(lblState, s == CS_OFFLINE ? C_MUTED : C_TEXT, 0);
  lv_label_set_text(lblState, state_word(s));

  switch (s) {
    case CS_WORKING:
      lv_obj_set_style_image_recolor(ringImg, C_TRACK, 0);
      lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
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

static void update_status_texts(const cw_status_t& st) {
  int64_t now = esp_timer_get_time();
  char dur[24], buf[160];
  fmt_dur((uint32_t)((now - st.state_since_us) / 1000000), dur, sizeof(dur));

  switch (st.state) {
    case CS_WORKING:
      lv_label_set_text(lblTool, st.tool[0] ? st.tool : "thinking");
      snprintf(buf, sizeof(buf), "running %s", dur); lv_label_set_text(lblElapsed, buf);
      break;
    case CS_WAITING:
      lv_label_set_text(lblTool, st.tool[0] ? st.tool : "needs you");
      snprintf(buf, sizeof(buf), "waiting %s", dur); lv_label_set_text(lblElapsed, buf);
      break;
    case CS_IDLE:
      lv_label_set_text(lblTool, "ready for input");
      snprintf(buf, sizeof(buf), "idle %s", dur); lv_label_set_text(lblElapsed, buf);
      break;
    case CS_ERROR:
      lv_label_set_text(lblTool, st.tool[0] ? st.tool : "error");
      snprintf(buf, sizeof(buf), "%s ago", dur); lv_label_set_text(lblElapsed, buf);
      break;
    default:
      lv_label_set_text(lblTool, "no session");
      lv_label_set_text(lblElapsed, wifiShown ? "claude-watch.local" : "USB: wifi <ssid> <pass>");
      break;
  }

  if (st.state == CS_OFFLINE) {
    lv_label_set_text(lblMsg, wifiShown ? cw_service_ip() : "");
    lv_label_set_text(lblFooter, "");
  } else {
    lv_label_set_text(lblMsg, st.msg);
    if (st.sessions > 1) snprintf(buf, sizeof(buf), "%s  •  %u sessions", st.project, st.sessions);
    else                 snprintf(buf, sizeof(buf), "%s", st.project);
    lv_label_set_text(lblFooter, buf);
  }

  switch (st.state) {
    case CS_WORKING: snprintf(buf, sizeof(buf), "Claude working  •  %s", st.tool[0] ? st.tool : "thinking"); break;
    case CS_WAITING: snprintf(buf, sizeof(buf), "Claude needs you%s%s", st.tool[0] ? "  •  " : "", st.tool); break;
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
  if (tvNow.tv_sec == lastSec) return;
  lastSec = tvNow.tv_sec;

  struct tm t;
  localtime_r(&tvNow.tv_sec, &t);
  bool valid = (t.tm_year + 1900) >= 2024;

  int sec = valid ? t.tm_sec : -1;
  if (sec != tickSec) {
    if (sec < 0 || tickSec < 0 || sec < tickSec) {
      for (int i = 0; i < TICKS; i++) lv_obj_set_style_bg_color(ticks[i], C_TRACK, 0);
      for (int i = 0; i < sec; i++) lv_obj_set_style_bg_color(ticks[i], C_ORANGE, 0);
    } else {
      for (int i = tickSec; i < sec; i++) lv_obj_set_style_bg_color(ticks[i], C_ORANGE, 0);
    }
    if (sec >= 0) lv_obj_set_style_bg_color(ticks[sec], C_TEXT, 0);
    tickSec = sec;
  }

  char buf[48];
  if (valid) {
    strftime(buf, sizeof(buf), "%H:%M", &t);
    lv_label_set_text(lblTime, buf);
    strftime(buf, sizeof(buf), "%A  %b %d", &t);
    lv_label_set_text(lblDate, buf);
  } else {
    lv_label_set_text(lblTime, "--:--");
    lv_label_set_text(lblDate, wifiShown ? "syncing time..." : "no time  •  connect Wi-Fi");
  }
}

static void tick_cb(lv_timer_t*) {
  if (!root) return;
  update_clock();

  bool w = cw_service_wifi_connected();
  if (w != wifiShown) { wifiShown = w; update_top_bar(); }

  cw_status_t st;
  cw_status_get(&st);

  if (st.state != shownState) {
    shownState = st.state;
    apply_state_visuals(st.state);
    lastSeq = 0xFFFFFFFF;
  }

  if (st.state != jumpPrev) {
    if ((st.state == CS_WAITING || st.state == CS_ERROR) && !pageAnimating) switch_page(PAGE_STATUS, true, true);
    jumpPrev = st.state;
  }
  if (curPage == PAGE_STATUS && !pageAnimating && (st.state == CS_IDLE || st.state == CS_OFFLINE) &&
      esp_timer_get_time() - st.state_since_us > AUTO_RETURN_TO_CLOCK_US) {
    switch_page(PAGE_CLOCK, false, true);
  }

  int64_t now = esp_timer_get_time();
  if (st.seq != lastSeq || now - lastSlowUs >= 1000000) {
    lastSlowUs = now;
    lastSeq = st.seq;
    update_status_texts(st);
  }
}

// ---------- public ----------
void cw_ui_create(lv_obj_t* parent) {
  root = parent;
  lv_obj_set_style_bg_color(root, C_BG, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  curPage = PAGE_CLOCK;
  pageAnimating = false;
  shownState = (cw_state_t)0xFF;
  lastSeq = 0xFFFFFFFF;
  lastSec = -1;
  lastSlowUs = 0;
  jumpPrev = CS_OFFLINE;
  wifiShown = cw_service_wifi_connected();

  for (int i = 0; i < PAGES; i++) {
    lv_obj_t* p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCREEN_W, SCREEN_W);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    if (i != curPage) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    pages[i] = p;
  }
  lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, NULL);

  build_clock(pages[PAGE_CLOCK]);
  build_status(pages[PAGE_STATUS]);
  update_top_bar();

  // Recorded by the app core -> deleted automatically on close.
  lv_timer_create(tick_cb, 100, NULL);
  tick_cb(NULL);
}

void cw_ui_detach(void) {
  root = nullptr;
}
