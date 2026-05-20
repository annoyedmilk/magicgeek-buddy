#pragma once
#include <stdint.h>
#include <stdbool.h>

// Persistent stats backed by NVS. Logic ported from the source project's
// stats.h (header-only Arduino Preferences) onto our storage.c NVS wrapper.
//
// Endurance: NVS sectors take ~100K writes. We persist on MILESTONE events
// (approval/denial/nap/level-up) only - never on a timer or per-heartbeat.
// Token deltas accumulate in RAM and only flush when a level boundary is
// crossed; worst case on hard power-off you lose up to TOKENS_PER_LEVEL of
// progress (acceptable, matches source).

#define TOKENS_PER_LEVEL 50000u

typedef struct {
    uint32_t nap_seconds;     // cumulative - placeholder (no IMU); kept for status ack shape
    uint16_t approvals;
    uint16_t denials;
    uint16_t velocity[8];     // ring of seconds-to-respond per approval
    uint8_t  vel_idx;
    uint8_t  vel_count;
    uint8_t  level;
    uint32_t tokens;          // cumulative output tokens - drives level
} stats_t;

// Settings (UI prefs persisted across reboots).
typedef struct {
    bool    sound;
    bool    bt;
    bool    wifi;     // stored only - actual radio is always on
    bool    led;      // no LED on this device; stored for future
    bool    hud;      // show transcript HUD on home
    uint8_t clock_rot; // 0=auto 1=portrait 2=landscape (no RTC; cosmetic)
} settings_t;

void stats_load(void);
void stats_save(void);   // no-op if not dirty

// Approval bookkeeping. seconds_to_respond goes into the velocity ring.
void     stats_on_approval(uint32_t seconds_to_respond);
void     stats_on_denial(void);

// Bridge tokens. Delta-tracked across heartbeats with a first-sight latch
// and bridge-restart resync (matches source semantics).
void     stats_on_bridge_tokens(uint32_t bridge_total);
bool     stats_poll_level_up(void);   // edge-triggered; true once per level-up

// Mood: 0..4 tier. Velocity sets the base; heavy denial ratio drags it down.
uint8_t  stats_mood_tier(void);
// Energy: 0..5. Without an IMU we hold at the "rested" default.
uint8_t  stats_energy_tier(void);
// Fed: 0..9 progress within the current level.
uint8_t  stats_fed_progress(void);
// Median seconds-to-respond from the velocity ring (0 if empty).
uint16_t stats_median_velocity(void);

const stats_t *stats_get(void);

// Settings
void settings_load(void);
void settings_save(void);
settings_t *settings_get(void);

// Wipe all stats + settings + persisted names. Used by factory-reset.
void stats_factory_reset(void);
