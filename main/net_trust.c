// Persistent trust list for joined WiFi networks. Anchors are BSSID +
// SSID; trust level is one of UNKNOWN / DENY / READONLY / ADMIN. The
// list is stored as a single NVS blob - small enough that a full read
// + rewrite on every change is cheaper than a per-entry key scheme.
#include "net_trust.h"
#include "storage.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "trust";

// 8 entries is enough for a home + a couple of mesh BSSIDs + a few
// remembered networks (coffee shop, office) without bloating the NVS
// blob. Older entries are LRU-evicted on store.
#define TRUST_MAX_ENTRIES 8
#define TRUST_NVS_KEY     "net_trust_v1"

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    uint8_t  level;          // net_trust_level_t (1 byte for compactness)
    uint8_t  pad;            // align ssid to even offset; future-proofing
    char     ssid[33];       // 32 + NUL
    uint32_t last_seen_ms;   // for LRU eviction; monotonic device time
} trust_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t        version;          // bump if the layout changes
    uint8_t        n_entries;        // how many of the slots are populated
    uint8_t        pad[2];
    trust_entry_t  entries[TRUST_MAX_ENTRIES];
} trust_table_t;

static trust_table_t g_table;
static volatile bool g_prompt_pending = false;

// ─────────────────────────────────────────────────────────────────────
// NVS persistence
// ─────────────────────────────────────────────────────────────────────

static void load_from_nvs(void)
{
    size_t len = sizeof(g_table);
    esp_err_t e = storage_get_blob(TRUST_NVS_KEY, &g_table, &len);
    if (e != ESP_OK || len != sizeof(g_table) || g_table.version != 1) {
        ESP_LOGI(TAG, "no persisted trust list (e=%d, len=%u) - starting empty",
                 (int)e, (unsigned)len);
        memset(&g_table, 0, sizeof(g_table));
        g_table.version = 1;
        g_table.n_entries = 0;
    } else {
        ESP_LOGI(TAG, "loaded %u trust entries from NVS", g_table.n_entries);
    }
    if (g_table.n_entries > TRUST_MAX_ENTRIES) g_table.n_entries = TRUST_MAX_ENTRIES;
}

static void save_to_nvs(void)
{
    esp_err_t e = storage_set_blob(TRUST_NVS_KEY, &g_table, sizeof(g_table));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist trust list: %d", (int)e);
    }
}

void net_trust_init(void)
{
    load_from_nvs();
}

// ─────────────────────────────────────────────────────────────────────
// Lookup / store
// ─────────────────────────────────────────────────────────────────────

static int find_entry(const uint8_t bssid[6], const char *ssid)
{
    for (uint8_t i = 0; i < g_table.n_entries; i++) {
        const trust_entry_t *e = &g_table.entries[i];
        if (memcmp(e->bssid, bssid, 6) == 0 &&
            strncmp(e->ssid, ssid, sizeof(e->ssid)) == 0) {
            return i;
        }
    }
    return -1;
}

net_trust_level_t net_trust_lookup(const uint8_t bssid[6], const char *ssid)
{
    if (!bssid || !ssid) return NET_TRUST_UNKNOWN;
    int idx = find_entry(bssid, ssid);
    if (idx < 0) return NET_TRUST_UNKNOWN;
    return (net_trust_level_t)g_table.entries[idx].level;
}

// LRU eviction by last_seen_ms; returns the slot index to overwrite.
static uint8_t pick_eviction_slot(void)
{
    uint8_t  victim = 0;
    uint32_t oldest = g_table.entries[0].last_seen_ms;
    for (uint8_t i = 1; i < TRUST_MAX_ENTRIES; i++) {
        if (g_table.entries[i].last_seen_ms < oldest) {
            oldest = g_table.entries[i].last_seen_ms;
            victim = i;
        }
    }
    return victim;
}

void net_trust_store(const uint8_t bssid[6], const char *ssid, net_trust_level_t level)
{
    if (!bssid || !ssid) return;
    extern uint32_t esp_log_timestamp(void);    // ms since boot, monotonic
    uint32_t now = esp_log_timestamp();

    int idx = find_entry(bssid, ssid);
    if (idx < 0) {
        if (g_table.n_entries < TRUST_MAX_ENTRIES) {
            idx = g_table.n_entries++;
        } else {
            idx = pick_eviction_slot();
            ESP_LOGI(TAG, "trust table full - evicting slot %d (ssid='%s')",
                     idx, g_table.entries[idx].ssid);
        }
        trust_entry_t *e = &g_table.entries[idx];
        memcpy(e->bssid, bssid, 6);
        memcpy(e->ssid, ssid, sizeof(e->ssid));
        e->ssid[sizeof(e->ssid) - 1] = 0;
    }

    g_table.entries[idx].level        = (uint8_t)level;
    g_table.entries[idx].last_seen_ms = now;

    ESP_LOGI(TAG, "trust set: ssid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x lvl=%d",
             g_table.entries[idx].ssid,
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
             (int)level);
    save_to_nvs();
}

// ─────────────────────────────────────────────────────────────────────
// Current AP introspection
// ─────────────────────────────────────────────────────────────────────

bool net_trust_current_ap(uint8_t bssid_out[6], char *ssid_out, size_t ssid_cap)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (bssid_out) memcpy(bssid_out, ap.bssid, 6);
    if (ssid_out && ssid_cap > 0) {
        size_t n = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
        if (n >= ssid_cap) n = ssid_cap - 1;
        memcpy(ssid_out, ap.ssid, n);
        ssid_out[n] = 0;
    }
    return true;
}

net_trust_level_t net_trust_current(void)
{
    uint8_t bssid[6];
    char    ssid[33];
    if (!net_trust_current_ap(bssid, ssid, sizeof(ssid))) return NET_TRUST_UNKNOWN;
    return net_trust_lookup(bssid, ssid);
}

// ─────────────────────────────────────────────────────────────────────
// Prompt-pending flag (consumed by the render loop / UI)
// ─────────────────────────────────────────────────────────────────────

bool net_trust_prompt_pending(void) { return g_prompt_pending; }

void net_trust_prompt_consumed(void) { g_prompt_pending = false; }

net_trust_level_t net_trust_on_sta_connect(const uint8_t bssid[6], const char *ssid)
{
    net_trust_level_t lvl = net_trust_lookup(bssid, ssid);
    if (lvl == NET_TRUST_UNKNOWN) {
        ESP_LOGI(TAG, "joined unknown network '%s' - arming trust prompt", ssid);
        g_prompt_pending = true;
    } else {
        // Refresh last_seen_ms so the LRU table favors recent connections.
        int idx = find_entry(bssid, ssid);
        if (idx >= 0) {
            extern uint32_t esp_log_timestamp(void);
            g_table.entries[idx].last_seen_ms = esp_log_timestamp();
            save_to_nvs();
        }
        ESP_LOGI(TAG, "joined known network '%s' - trust lvl %d", ssid, (int)lvl);
    }
    return lvl;
}

bool net_trust_allows(net_trust_level_t required)
{
    // In SoftAP / captive-portal mode the only reachable peer is on our
    // own AP (physical proximity required). Skip the gate so the user
    // can complete setup; the STA path is where untrusted peers live.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        return true;
    }
    net_trust_level_t cur = net_trust_current();
    return (int)cur >= (int)required;
}
