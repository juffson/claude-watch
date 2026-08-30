#include "cw_status.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static cw_status_t g_st;
static SemaphoreHandle_t g_mtx;

static const char *k_names[] = { "offline", "idle", "working", "waiting", "error" };

const char *cw_state_name(cw_state_t s) { return (s > CS_ERROR) ? "offline" : k_names[s]; }

static bool state_from_name(const char *s, cw_state_t *out)
{
    for (int i = 0; i <= CS_ERROR; i++) {
        if (strcasecmp(s, k_names[i]) == 0) { *out = (cw_state_t)i; return true; }
    }
    if (!strcasecmp(s, "busy") || !strcasecmp(s, "thinking")) { *out = CS_WORKING; return true; }
    if (!strcasecmp(s, "permission") || !strcasecmp(s, "input")) { *out = CS_WAITING; return true; }
    if (!strcasecmp(s, "done") || !strcasecmp(s, "ready")) { *out = CS_IDLE; return true; }
    return false;
}

void cw_status_init(void)
{
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
    memset(&g_st, 0, sizeof(g_st));
    g_st.state_since_us = esp_timer_get_time();
}

// ---- minimal flat-JSON helpers ----
static const char *json_find_value(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                return q;
            }
        }
        const char *e = p;
        while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
        if (!*e) break;
        p = e + 1;
    }
    return NULL;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') return false;
    v++;
    size_t n = 0;
    while (*v && *v != '"') {
        char c = *v;
        if (c == '\\' && v[1]) {
            v++;
            switch (*v) {
                case 'n': case 't': case 'r': c = ' '; break;
                case 'u': if (strlen(v) >= 5) v += 4; c = '?'; break;
                default: c = *v; break;
            }
        }
        if ((unsigned char)c >= 0x80) c = '?';   // fonts are Latin-only
        if (n + 1 < out_len) out[n++] = c;
        v++;
    }
    if (out_len) out[n] = 0;
    return true;
}

static bool json_get_int(const char *json, const char *key, long *out)
{
    const char *v = json_find_value(json, key);
    if (!v) return false;
    if (*v == '"') v++;
    char *end;
    long val = strtol(v, &end, 10);
    if (end == v) return false;
    *out = val;
    return true;
}

bool cw_status_apply_json(const char *json, bool *state_changed)
{
    if (state_changed) *state_changed = false;
    char s[16];
    cw_state_t ns;
    if (!json_get_str(json, "state", s, sizeof(s)) || !state_from_name(s, &ns)) return false;

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool changed = (ns != g_st.state);
    if (changed) { g_st.state = ns; g_st.state_since_us = now; }
    json_get_str(json, "tool", g_st.tool, sizeof(g_st.tool));
    json_get_str(json, "project", g_st.project, sizeof(g_st.project));
    if (!json_get_str(json, "msg", g_st.msg, sizeof(g_st.msg)) && changed) g_st.msg[0] = 0;
    long sessions;
    if (json_get_int(json, "sessions", &sessions)) {
        g_st.sessions = (uint8_t)(sessions < 0 ? 0 : sessions > 255 ? 255 : sessions);
    }
    g_st.updated_us = now;
    g_st.seq++;
    xSemaphoreGive(g_mtx);

    if (state_changed) *state_changed = changed;
    return true;
}

bool cw_status_tick(void)
{
    bool changed = false;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    if (g_st.state != CS_OFFLINE && g_st.updated_us &&
        esp_timer_get_time() - g_st.updated_us > CW_STATUS_OFFLINE_AFTER_US) {
        g_st.state = CS_OFFLINE;
        g_st.state_since_us = esp_timer_get_time();
        g_st.tool[0] = 0;
        g_st.msg[0] = 0;
        g_st.sessions = 0;
        changed = true;
    }
    xSemaphoreGive(g_mtx);
    return changed;
}

void cw_status_get(cw_status_t *out)
{
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    *out = g_st;
    xSemaphoreGive(g_mtx);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    for (; *in && n + 2 < out_len; in++) {
        if (*in == '"' || *in == '\\') { out[n++] = '\\'; out[n++] = *in; }
        else if ((unsigned char)*in < 0x20) out[n++] = ' ';
        else out[n++] = *in;
    }
    out[n] = 0;
}

void cw_status_to_json(char *buf, size_t len)
{
    cw_status_t st;
    cw_status_get(&st);
    char tool[64], project[80], msg[192];
    json_escape(st.tool, tool, sizeof(tool));
    json_escape(st.project, project, sizeof(project));
    json_escape(st.msg, msg, sizeof(msg));
    int64_t now = esp_timer_get_time();
    snprintf(buf, len,
             "{\"state\":\"%s\",\"tool\":\"%s\",\"project\":\"%s\",\"msg\":\"%s\","
             "\"sessions\":%u,\"age_s\":%lld,\"state_age_s\":%lld,\"seq\":%lu}",
             cw_state_name(st.state), tool, project, msg, st.sessions,
             st.updated_us ? (long long)((now - st.updated_us) / 1000000) : 0LL,
             (long long)((now - st.state_since_us) / 1000000), (unsigned long)st.seq);
}
