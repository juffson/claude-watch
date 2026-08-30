#pragma once
#include <Arduino.h>

enum ClaudeState : uint8_t { CS_OFFLINE = 0, CS_IDLE, CS_WORKING, CS_WAITING, CS_ERROR };

struct ClaudeStatus {
  ClaudeState state = CS_OFFLINE;
  char tool[32] = "";
  char project[40] = "";
  char msg[96] = "";
  char output[400] = "";      // Claude's last reply (UTF-8, trimmed by the host)
  uint8_t sessions = 0;       // active Claude Code sessions on the host
  uint32_t updatedMs = 0;     // millis() of last message received
  uint32_t stateSinceMs = 0;  // millis() when `state` last changed
  uint32_t seq = 0;           // incremented on every accepted message
};

extern ClaudeStatus g_claude;

const char* claude_state_name(ClaudeState s);
ClaudeState claude_state_from_name(const char* s, bool* ok);

// Parse a flat JSON object, e.g.
//   {"state":"working","tool":"Bash","project":"esp32","msg":"...","sessions":2}
// Returns true if a valid "state" was found and applied.
// *stateChanged is set when the state value differs from the previous one.
bool status_apply_json(const char* json, bool* stateChanged);

// Call periodically; flips to OFFLINE when no message arrived for a while.
// Returns true if the state changed.
bool status_tick();

// Serialise current status as JSON into buf.
void status_to_json(char* buf, size_t len);

// Minimal flat-JSON helpers (shared with settings / web console).
bool cw_json_get_str(const char* json, const char* key, char* out, size_t outLen);
bool cw_json_get_int(const char* json, const char* key, long* out);
bool cw_json_get_bool(const char* json, const char* key, bool* out);
