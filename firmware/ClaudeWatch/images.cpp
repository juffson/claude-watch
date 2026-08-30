#include "images.h"
#include <FFat.h>
#include <string.h>
#include "esp_heap_caps.h"

static bool mounted = false;
static char names[IMG_MAX][32];
static int  count = 0;

// upload staging (PSRAM): the HTTP parser hands us ~1.4 KB pieces; writing those straight to
// FAT is slow, so we collect the whole image and write it in large chunks at the end.
static uint8_t* stage = nullptr;
static size_t   stageLen = 0;
static char     stageName[40] = "";

static void path_of(const char* name, char* out, size_t len) { snprintf(out, len, IMG_DIR "/%s", name); }

bool images_valid_name(const char* name) {
  size_t n = strlen(name);
  if (n < 5 || n > 28) return false;
  if (strcmp(name + n - 4, ".bin") != 0) return false;
  for (size_t i = 0; i < n - 4; i++) {
    char c = name[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

bool images_init() {
  mounted = FFat.begin(true, "/ffat", 10, "ffat");
  if (!mounted) { Serial.println("[img] FFat mount failed"); return false; }
  if (!FFat.exists(IMG_DIR)) FFat.mkdir(IMG_DIR);
  images_rescan();
  Serial.printf("[img] FFat ok: %u KB used / %u KB, %d images\n",
                (unsigned)((FFat.totalBytes() - FFat.freeBytes()) / 1024), (unsigned)(FFat.totalBytes() / 1024), count);
  return true;
}

void images_rescan() {
  count = 0;
  if (!mounted) return;
  File dir = FFat.open(IMG_DIR);
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile()) && count < IMG_MAX) {
    const char* base = strrchr(f.path(), '/');
    base = base ? base + 1 : f.name();
    if (!f.isDirectory() && images_valid_name(base) && f.size() == IMG_BYTES) {
      // insertion sort by name so the order is stable
      int i = count;
      while (i > 0 && strcmp(names[i - 1], base) > 0) { strcpy(names[i], names[i - 1]); i--; }
      strlcpy(names[i], base, sizeof(names[i]));
      count++;
    }
    f.close();
  }
  dir.close();
}

int images_count() { return count; }
const char* images_name(int idx) { return (idx >= 0 && idx < count) ? names[idx] : ""; }
int images_find(const char* name) {
  for (int i = 0; i < count; i++) if (strcmp(names[i], name) == 0) return i;
  return -1;
}
size_t images_free_bytes() { return mounted ? FFat.freeBytes() : 0; }
size_t images_total_bytes() { return mounted ? FFat.totalBytes() : 0; }

bool images_remove(const char* name) {
  if (!mounted || !images_valid_name(name)) return false;
  char p[48]; path_of(name, p, sizeof(p));
  bool ok = FFat.remove(p);
  images_rescan();
  return ok;
}

bool images_load(const char* name, lv_img_dsc_t* dsc, uint8_t dimPct) {
  if (!mounted || !images_valid_name(name)) return false;
  if (!dsc->data) {
    dsc->data = (const uint8_t*)heap_caps_malloc(IMG_BYTES, MALLOC_CAP_SPIRAM);
    if (!dsc->data) { Serial.println("[img] PSRAM alloc failed"); return false; }
  }
  char p[48]; path_of(name, p, sizeof(p));
  File f = FFat.open(p, FILE_READ);
  if (!f || f.size() != IMG_BYTES) { if (f) f.close(); return false; }
  uint8_t* dst = (uint8_t*)dsc->data;
  size_t got = 0;
  while (got < IMG_BYTES) {
    int r = f.read(dst + got, min((size_t)16384, IMG_BYTES - got));
    if (r <= 0) break;
    got += r;
  }
  f.close();
  if (got != IMG_BYTES) return false;

  if (dimPct > 0) {
    // scale each RGB565 channel in place (big-endian pixel order)
    uint8_t k = 100 - min<uint8_t>(dimPct, 95);
    uint8_t lut5[32], lut6[64];
    for (int i = 0; i < 32; i++) lut5[i] = i * k / 100;
    for (int i = 0; i < 64; i++) lut6[i] = i * k / 100;
    for (size_t i = 0; i < IMG_BYTES; i += 2) {
      uint16_t v = (dst[i] << 8) | dst[i + 1];
      uint16_t o = (lut5[v >> 11] << 11) | (lut6[(v >> 5) & 0x3F] << 5) | lut5[v & 0x1F];
      dst[i] = o >> 8; dst[i + 1] = o & 0xFF;
    }
  }
  dsc->header.always_zero = 0;
  dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc->header.w = IMG_W;
  dsc->header.h = IMG_H;
  dsc->data_size = IMG_BYTES;
  return true;
}

// ---- upload ----
bool images_write_begin(const char* name) {
  if (!mounted || !images_valid_name(name)) return false;
  if (!stage) stage = (uint8_t*)heap_caps_malloc(IMG_BYTES, MALLOC_CAP_SPIRAM);
  if (!stage) return false;
  if (FFat.freeBytes() < IMG_BYTES + 64 * 1024 && images_find(name) < 0) {
    Serial.println("[img] not enough space");
    return false;
  }
  strlcpy(stageName, name, sizeof(stageName));
  stageLen = 0;
  return true;
}

bool images_write_chunk(const uint8_t* data, size_t len) {
  if (!stage || !stageName[0]) return false;
  if (stageLen + len > IMG_BYTES) { images_write_abort(); return false; }
  memcpy(stage + stageLen, data, len);
  stageLen += len;
  return true;
}

bool images_write_end() {
  if (!stage || !stageName[0]) return false;
  bool ok = false;
  if (stageLen == IMG_BYTES) {
    char p[48]; path_of(stageName, p, sizeof(p));
    FFat.remove(p);
    File f = FFat.open(p, FILE_WRITE);
    if (f) {
      size_t w = 0;
      while (w < IMG_BYTES) {
        size_t n = min((size_t)32768, IMG_BYTES - w);
        if (f.write(stage + w, n) != n) break;
        w += n;
      }
      f.close();
      ok = (w == IMG_BYTES);
      if (!ok) FFat.remove(p);
    }
  } else {
    Serial.printf("[img] bad upload size %u (expected %u)\n", (unsigned)stageLen, (unsigned)IMG_BYTES);
  }
  Serial.printf("[img] upload %s -> %s\n", stageName, ok ? "saved" : "failed");
  stageName[0] = 0;
  stageLen = 0;
  images_rescan();
  return ok;
}

void images_write_abort() {
  stageName[0] = 0;
  stageLen = 0;
}
