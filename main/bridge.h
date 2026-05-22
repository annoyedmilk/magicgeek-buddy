#pragma once
#include <stdint.h>
#include <stdbool.h>

// Claude Buddy device-to-desktop bridge.
//
// Wire format follows the Hardware Buddy spec shipped with the
// claude-desktop-buddy reference project (REFERENCE.md).
//   Desktop sends newline-delimited UTF-8 JSON over the NUS RX char:
//     heartbeats (state snapshots, every ~10s plus on change)
//     one-shot commands (status, name, owner, unpair, time, prompt...)
//     turn events {"evt":"turn",...}
//   Device replies on the NUS TX char (notify), same line-JSON shape.
//   When a heartbeat contains "prompt", the user can approve/deny by
//   posting {"cmd":"permission","id":...,"decision":"once"|"deny"}.

// One-line summary, transcript entries, prompt info - drives the screen.
typedef struct {
    uint8_t  sessions_total;
    uint8_t  sessions_running;
    uint8_t  sessions_waiting;
    bool     recently_completed;
    uint32_t tokens_today;
    uint32_t last_updated_ms;     // millis() when the last snapshot landed
    bool     connected;           // last heartbeat within DATA_TIMEOUT_MS
    char     msg[40];             // one-line summary suitable for a small display

    char     prompt_id[40];       // empty if no prompt waiting
    char     prompt_tool[24];
    char     prompt_hint[48];

    // Recent transcript lines (newest last). nLines counts how many of
    // `lines` are valid. line_gen bumps on any change so the UI can
    // detect "new content" without diffing strings.
    char     lines[6][80];
    uint8_t  n_lines;
    uint16_t line_gen;
} tama_state_t;

// Persona derived from the snapshot. Each value drives a specific
// ASCII pet animation in buddy.c and a screen accent color in main.c.
// P_DIZZY is a one-shot reaction state fired by the bridge whenever a
// `{"evt":"turn"}` event arrives; main.c overrides the derived persona
// with P_DIZZY for ~1.5s so the user gets a visible "turn completed"
// micro-interaction without persistent state.
typedef enum {
    P_SLEEP,        // no desktop connected
    P_IDLE,         // connected, nothing urgent
    P_BUSY,         // ≥3 sessions actively generating
    P_ATTENTION,    // a permission prompt is waiting
    P_CELEBRATE,    // recently completed
    P_DIZZY,        // turn just landed (one-shot, ~1.5s)
} persona_state_t;

// Initialize the bridge. Must be called after ble_init() since acks are
// sent through the NUS link. Spawns a poll task on its own stack - the
// render loop must never block on RX.
void bridge_init(void);

// Monotonically incrementing counter; bumped whenever the bridge state
// changes (heartbeat received, stale timeout). Use this to skip the full
// struct copy when nothing has changed since the last call.
uint32_t bridge_get_generation(void);

// Snapshot of the latest state. Copied (not aliased) so callers can read
// safely from any task. The render loop polls this at frame rate.
void bridge_get_state(tama_state_t *out);

// Persona from a state. Pure function; no side effects.
persona_state_t bridge_derive(const tama_state_t *s);

// Reply to a pending prompt. allow=true → "once", false → "deny".
// No-op if there is no prompt; clears the local prompt fields on send so
// the screen returns to the persona view immediately.
void bridge_send_permission(bool allow);

// Has the device received any heartbeat in the last DATA_TIMEOUT_MS?
// Cheaper helper for the render loop than reading the full snapshot.
bool bridge_data_alive(void);

// One-shot edge: returns true exactly once after a `{"evt":"turn"}`
// landed. Lets the render loop trigger a brief P_DIZZY animation
// without keeping a "turn pending" field in tama_state_t.
bool bridge_poll_turn(void);
