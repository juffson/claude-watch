// Claude Code status model (thread-safe). Fed by HTTP / USB console, read by the UI.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CS_OFFLINE = 0, CS_IDLE, CS_WORKING, CS_WAITING, CS_ERROR } cw_state_t;

typedef struct {
    cw_state_t state;
    char tool[32];
    char project[40];
    char msg[96];
    uint8_t sessions;
    int64_t updated_us;      // esp_timer time of last accepted message (0 = never)
    int64_t state_since_us;  // when `state` last changed
    uint32_t seq;            // incremented per accepted message
} cw_status_t;

#define CW_STATUS_OFFLINE_AFTER_US (10LL * 60 * 1000000)  // no message for 10 min -> OFFLINE

void cw_status_init(void);
// Parse a flat JSON object {"state":"working","tool":"Bash","project":"x","msg":"...","sessions":2}
bool cw_status_apply_json(const char *json, bool *state_changed);
// Offline timeout; call periodically. Returns true if the state changed.
bool cw_status_tick(void);
// Snapshot copy.
void cw_status_get(cw_status_t *out);
void cw_status_to_json(char *buf, size_t len);
const char *cw_state_name(cw_state_t s);

#ifdef __cplusplus
}
#endif
