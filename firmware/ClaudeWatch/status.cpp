#include "status.h"
#include "config.h"
#include "settings.h"
#include <string.h>
#include <stdlib.h>

ClaudeStatus g_claude;
void time_from_host(long long epoch);   // ClaudeWatch.ino

static const char* kStateNames[] = { "offline", "idle", "working", "waiting", "error" };

const char* claude_state_name(ClaudeState s) {
  if (s > CS_ERROR) return "offline";
  return kStateNames[s];
}

ClaudeState claude_state_from_name(const char* s, bool* ok) {
  if (ok) *ok = true;
  for (int i = 0; i <= CS_ERROR; i++) {
    if (strcasecmp(s, kStateNames[i]) == 0) return (ClaudeState)i;
  }
  // a few aliases from the host side
  if (strcasecmp(s, "busy") == 0 || strcasecmp(s, "thinking") == 0) return CS_WORKING;
  if (strcasecmp(s, "permission") == 0 || strcasecmp(s, "input") == 0) return CS_WAITING;
  if (strcasecmp(s, "done") == 0 || strcasecmp(s, "ready") == 0) return CS_IDLE;
  if (ok) *ok = false;
  return CS_OFFLINE;
}

// ---- minimal flat-JSON helpers (enough for our own host script) ----

// Find the start of the value for "key". Returns nullptr if absent.
static const char* json_find_value(const char* json, const char* key) {
  size_t klen = strlen(key);
  const char* p = json;
  while ((p = strchr(p, '"')) != nullptr) {
    p++;
    if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
      const char* q = p + klen + 1;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      if (*q == ':') {
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
      }
    }
    // skip to closing quote of this string token
    const char* e = p;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (!*e) break;
    p = e + 1;
  }
  return nullptr;
}

static size_t utf8_put(char* out, size_t n, size_t outLen, uint32_t cp) {
  char tmp[4]; int len;
  if (cp < 0x80)        { tmp[0] = cp; len = 1; }
  else if (cp < 0x800)  { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); len = 2; }
  else if (cp < 0x10000){ tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F); tmp[2] = 0x80 | (cp & 0x3F); len = 3; }
  else                  { tmp[0] = 0xF0 | (cp >> 18); tmp[1] = 0x80 | ((cp >> 12) & 0x3F); tmp[2] = 0x80 | ((cp >> 6) & 0x3F); tmp[3] = 0x80 | (cp & 0x3F); len = 4; }
  if (n + len >= outLen) return n;   // never split a multi-byte sequence at the end
  memcpy(out + n, tmp, len);
  return n + len;
}

static uint32_t hex4(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return 0xFFFFFFFF;
  }
  return v;
}

// Copies a JSON string value, decoding escapes (incl. \uXXXX and surrogate pairs) to UTF-8.
bool cw_json_get_str(const char* json, const char* key, char* out, size_t outLen) {
  const char* v = json_find_value(json, key);
  if (!v || *v != '"') return false;
  v++;
  size_t n = 0;
  while (*v && *v != '"') {
    if (*v == '\\' && v[1]) {
      v++;
      char e = *v++;
      switch (e) {
        case 'n': case 't': case 'r': n = utf8_put(out, n, outLen, ' '); break;
        case 'u': {
          uint32_t cp = hex4(v);
          if (cp == 0xFFFFFFFF) break;
          v += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && v[0] == '\\' && v[1] == 'u') {   // surrogate pair
            uint32_t lo = hex4(v + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); v += 6; }
          }
          n = utf8_put(out, n, outLen, cp);
          break;
        }
        default: n = utf8_put(out, n, outLen, (unsigned char)e); break;
      }
      continue;
    }
    // raw byte (ASCII or UTF-8 sequence byte): copy, but don't cut a sequence at the buffer end
    unsigned char c = (unsigned char)*v;
    size_t seq = (c < 0x80) ? 1 : (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (n + seq >= outLen) break;
    for (size_t i = 0; i < seq && *v; i++) out[n++] = *v++;
  }
  if (outLen) out[n] = 0;
  return true;
}

bool cw_json_get_int(const char* json, const char* key, long* out) {
  const char* v = json_find_value(json, key);
  if (!v) return false;
  if (*v == '"') v++;
  char* end;
  long val = strtol(v, &end, 10);
  if (end == v) return false;
  *out = val;
  return true;
}

bool cw_json_get_bool(const char* json, const char* key, bool* out) {
  const char* v = json_find_value(json, key);
  if (!v) return false;
  if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
  if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
  if (*v == '0' || *v == '1') { *out = (*v == '1'); return true; }
  return false;
}

bool status_apply_json(const char* json, bool* stateChanged) {
  if (stateChanged) *stateChanged = false;
  char stateStr[16];
  if (!cw_json_get_str(json, "state", stateStr, sizeof(stateStr))) return false;
  bool ok;
  ClaudeState ns = claude_state_from_name(stateStr, &ok);
  if (!ok) return false;

  uint32_t now = millis();
  if (ns != g_claude.state) {
    g_claude.state = ns;
    g_claude.stateSinceMs = now;
    if (stateChanged) *stateChanged = true;
  }
  // Optional fields: only overwrite when present so partial updates work.
  cw_json_get_str(json, "tool", g_claude.tool, sizeof(g_claude.tool));
  cw_json_get_str(json, "project", g_claude.project, sizeof(g_claude.project));
  if (!cw_json_get_str(json, "msg", g_claude.msg, sizeof(g_claude.msg))) {
    // a state change without a message clears any stale message
    if (stateChanged && *stateChanged) g_claude.msg[0] = 0;
  }
  cw_json_get_str(json, "output", g_claude.output, sizeof(g_claude.output));
  long long epoch = 0; long ep;
  if (cw_json_get_int(json, "epoch", &ep)) epoch = ep;
  if (epoch > 1700000000LL) time_from_host(epoch);     // the Mac's clock rides along with every push
  long sessions;
  if (cw_json_get_int(json, "sessions", &sessions)) {
    g_claude.sessions = (uint8_t)constrain(sessions, 0, 255);
  }
  g_claude.updatedMs = now;
  g_claude.seq++;
  return true;
}

bool status_tick() {
  if (g_claude.state == CS_OFFLINE) return false;
  if (g_claude.updatedMs == 0) return false;
  if (millis() - g_claude.updatedMs > (uint32_t)g_settings.offlineMin * 60000UL) {
    g_claude.state = CS_OFFLINE;
    g_claude.stateSinceMs = millis();
    g_claude.tool[0] = 0;
    g_claude.msg[0] = 0;
    g_claude.output[0] = 0;
    g_claude.sessions = 0;
    return true;
  }
  return false;
}

// Escape for JSON output (only what we may produce ourselves).
static void json_escape(const char* in, char* out, size_t outLen) {
  size_t n = 0;
  for (; *in && n + 2 < outLen; in++) {
    if (*in == '"' || *in == '\\') { out[n++] = '\\'; out[n++] = *in; }
    else if ((unsigned char)*in < 0x20) { out[n++] = ' '; }
    else out[n++] = *in;
  }
  out[n] = 0;
}

void status_to_json(char* buf, size_t len) {
  char tool[64], project[80], msg[192], output[560];
  json_escape(g_claude.tool, tool, sizeof(tool));
  json_escape(g_claude.project, project, sizeof(project));
  json_escape(g_claude.msg, msg, sizeof(msg));
  json_escape(g_claude.output, output, sizeof(output));
  uint32_t now = millis();
  snprintf(buf, len,
           "{\"state\":\"%s\",\"tool\":\"%s\",\"project\":\"%s\",\"msg\":\"%s\",\"output\":\"%s\","
           "\"sessions\":%u,\"age_s\":%lu,\"state_age_s\":%lu,\"seq\":%lu}",
           claude_state_name(g_claude.state), tool, project, msg, output, g_claude.sessions,
           g_claude.updatedMs ? (unsigned long)((now - g_claude.updatedMs) / 1000) : 0UL,
           (unsigned long)((now - g_claude.stateSinceMs) / 1000),
           (unsigned long)g_claude.seq);
}
