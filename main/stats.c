// Persistent stats. Semantics ported from the source's stats.h; backed by
// our storage.c NVS wrapper (namespace "buddy"). See stats.h for the
// design rules - milestone-only saves, RAM token accumulation, etc.
#include "stats.h"
#include "storage.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "stats";

// ─────────────────────────────────────────────────────────────────────
// In-memory state
// ─────────────────────────────────────────────────────────────────────

static stats_t    g_stats;
static bool       g_dirty;
static settings_t g_settings = {
    .sound = true, .bt = true, .wifi = false,
    .led = true,  .hud = true, .clock_rot = 0,
};

// Bridge-token tracking. Matches source's _lastBridgeTokens/_tokensSynced
// latch: on device reboot _last_bridge starts at 0 while the bridge's
// running total is whatever it is - first packet would otherwise credit
// the entire session. Latch on first sight, then track deltas.
static uint32_t g_last_bridge_tokens = 0;
static bool     g_tokens_synced      = false;
static bool     g_level_up_pending   = false;

// Energy is gated on the last nap-end timestamp. We have no IMU so the
// nap-trigger never fires; this stays at "rested" until something else
// stamps it. The plumbing is preserved so a future input path can hook in.
static uint64_t g_last_nap_end_us = 0;
static uint8_t  g_energy_at_nap   = 5;

// ─────────────────────────────────────────────────────────────────────
// Load / save
// ─────────────────────────────────────────────────────────────────────

// Per-key gets, defaults to zero. We keep a small set of u32 keys + one
// blob for the velocity ring - cheaper than serializing the whole struct
// and survives field additions without migration logic.
void stats_load(void)
{
    memset(&g_stats, 0, sizeof(g_stats));

    uint32_t v;
    storage_get_u32("nap",  &v, 0); g_stats.nap_seconds = v;
    storage_get_u32("appr", &v, 0); g_stats.approvals   = (uint16_t)v;
    storage_get_u32("deny", &v, 0); g_stats.denials     = (uint16_t)v;
    storage_get_u32("vidx", &v, 0); g_stats.vel_idx     = (uint8_t)v;
    storage_get_u32("vcnt", &v, 0); g_stats.vel_count   = (uint8_t)v;
    storage_get_u32("lvl",  &v, 0); g_stats.level       = (uint8_t)v;
    storage_get_u32("tok",  &v, 0); g_stats.tokens      = v;

    size_t vlen = sizeof(g_stats.velocity);
    if (storage_get_blob("vel", g_stats.velocity, &vlen) != 0
        || vlen != sizeof(g_stats.velocity)) {
        memset(g_stats.velocity, 0, sizeof(g_stats.velocity));
    }

    // Level derives from tokens; backfill if an older save had level set
    // but tokens at 0 (matches source migration behavior).
    if (g_stats.tokens == 0 && g_stats.level > 0) {
        g_stats.tokens = (uint32_t)g_stats.level * TOKENS_PER_LEVEL;
    }

    g_dirty = false;
    ESP_LOGI(TAG, "loaded: lvl=%u tokens=%lu appr=%u deny=%u",
             g_stats.level, (unsigned long)g_stats.tokens,
             g_stats.approvals, g_stats.denials);
}

void stats_save(void)
{
    if (!g_dirty) return;
    storage_set_u32("nap",  g_stats.nap_seconds);
    storage_set_u32("appr", g_stats.approvals);
    storage_set_u32("deny", g_stats.denials);
    storage_set_u32("vidx", g_stats.vel_idx);
    storage_set_u32("vcnt", g_stats.vel_count);
    storage_set_u32("lvl",  g_stats.level);
    storage_set_u32("tok",  g_stats.tokens);
    storage_set_blob("vel", g_stats.velocity, sizeof(g_stats.velocity));
    g_dirty = false;
}

// ─────────────────────────────────────────────────────────────────────
// Event hooks
// ─────────────────────────────────────────────────────────────────────

void stats_on_approval(uint32_t seconds_to_respond)
{
    g_stats.approvals++;
    g_stats.velocity[g_stats.vel_idx] =
        (seconds_to_respond > 65535u) ? 65535u : (uint16_t)seconds_to_respond;
    g_stats.vel_idx = (g_stats.vel_idx + 1) % 8;
    if (g_stats.vel_count < 8) g_stats.vel_count++;
    g_dirty = true;
    stats_save();
}

void stats_on_denial(void)
{
    g_stats.denials++;
    g_dirty = true;
    stats_save();
}

void stats_on_bridge_tokens(uint32_t bridge_total)
{
    // First-sight latch - never credit the entire pre-existing session.
    if (!g_tokens_synced) {
        g_last_bridge_tokens = bridge_total;
        g_tokens_synced = true;
        return;
    }
    // Bridge restarted (its running total went down) → just resync, no
    // delta. We don't lose anything: our cumulative `tokens` stays put.
    if (bridge_total < g_last_bridge_tokens) {
        g_last_bridge_tokens = bridge_total;
        return;
    }
    uint32_t delta = bridge_total - g_last_bridge_tokens;
    g_last_bridge_tokens = bridge_total;
    if (delta == 0) return;

    uint8_t lvl_before = (uint8_t)(g_stats.tokens / TOKENS_PER_LEVEL);
    g_stats.tokens += delta;
    uint8_t lvl_after  = (uint8_t)(g_stats.tokens / TOKENS_PER_LEVEL);

    // Persist ONLY on a level-up boundary. Heartbeats fire every ~10s; saving
    // per-delta would burn NVS sectors in days. Worst case on hard power-off:
    // up to TOKENS_PER_LEVEL of unpersisted progress (~50K tokens, ~1-2hrs).
    if (lvl_after > lvl_before) {
        g_stats.level = lvl_after;
        g_level_up_pending = true;
        g_dirty = true;
        stats_save();
    }
}

bool stats_poll_level_up(void)
{
    bool r = g_level_up_pending;
    g_level_up_pending = false;
    return r;
}

// ─────────────────────────────────────────────────────────────────────
// Derived metrics
// ─────────────────────────────────────────────────────────────────────

uint16_t stats_median_velocity(void)
{
    if (g_stats.vel_count == 0) return 0;
    uint16_t tmp[8];
    memcpy(tmp, g_stats.velocity, sizeof(tmp));
    uint8_t n = g_stats.vel_count;
    // insertion sort, n ≤ 8
    for (uint8_t i = 1; i < n; i++) {
        uint16_t k = tmp[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    return tmp[n / 2];
}

uint8_t stats_mood_tier(void)
{
    uint16_t vel = stats_median_velocity();
    int8_t tier;
    if      (vel == 0)   tier = 2;     // no data → neutral
    else if (vel < 15)   tier = 4;
    else if (vel < 30)   tier = 3;
    else if (vel < 60)   tier = 2;
    else if (vel < 120)  tier = 1;
    else                 tier = 0;
    uint16_t a = g_stats.approvals, d = g_stats.denials;
    if (a + d >= 3) {
        if (d > a)         tier -= 2;
        else if (d * 2 > a) tier -= 1;   // deny rate > 33%
    }
    if (tier < 0) tier = 0;
    return (uint8_t)tier;
}

uint8_t stats_energy_tier(void)
{
    // Without an IMU we can't detect face-down nap → tier never decays
    // from a real reading. Source decays 1 tier per 2h; we hold at full.
    uint32_t hours_since = (uint32_t)((esp_timer_get_time() - g_last_nap_end_us)
                                      / 3600000000ULL);
    int8_t e = (int8_t)g_energy_at_nap - (int8_t)(hours_since / 2);
    if (e < 0) e = 0;
    if (e > 5) e = 5;
    return (uint8_t)e;
}

uint8_t stats_fed_progress(void)
{
    return (uint8_t)((g_stats.tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

const stats_t *stats_get(void) { return &g_stats; }

// ─────────────────────────────────────────────────────────────────────
// Settings
// ─────────────────────────────────────────────────────────────────────

// Booleans go in as u32 (NVS u8 helper isn't in our wrapper); cheap.
void settings_load(void)
{
    uint32_t v;
    storage_get_u32("s_snd",  &v, 1); g_settings.sound = v ? true : false;
    storage_get_u32("s_bt",   &v, 1); g_settings.bt    = v ? true : false;
    storage_get_u32("s_wifi", &v, 0); g_settings.wifi  = v ? true : false;
    storage_get_u32("s_led",  &v, 1); g_settings.led   = v ? true : false;
    storage_get_u32("s_hud",  &v, 1); g_settings.hud   = v ? true : false;
    storage_get_u32("s_crot", &v, 0); g_settings.clock_rot = (uint8_t)((v > 2) ? 0 : v);
}

void settings_save(void)
{
    storage_set_u32("s_snd",  g_settings.sound ? 1 : 0);
    storage_set_u32("s_bt",   g_settings.bt    ? 1 : 0);
    storage_set_u32("s_wifi", g_settings.wifi  ? 1 : 0);
    storage_set_u32("s_led",  g_settings.led   ? 1 : 0);
    storage_set_u32("s_hud",  g_settings.hud   ? 1 : 0);
    storage_set_u32("s_crot", g_settings.clock_rot);
}

settings_t *settings_get(void) { return &g_settings; }

// ─────────────────────────────────────────────────────────────────────
// Factory reset
// ─────────────────────────────────────────────────────────────────────

void stats_factory_reset(void)
{
    storage_erase_all();   // wipes the "buddy" NVS namespace entirely
    // Reset RAM state to match - caller typically reboots after this,
    // but if not we don't want zombie pre-wipe values visible.
    memset(&g_stats, 0, sizeof(g_stats));
    g_dirty = false;
    g_last_bridge_tokens = 0;
    g_tokens_synced = false;
    g_level_up_pending = false;
    g_settings = (settings_t){ .sound=true, .bt=true, .wifi=false,
                               .led=true, .hud=true, .clock_rot=0 };
    ESP_LOGW(TAG, "factory reset - NVS wiped");
}
