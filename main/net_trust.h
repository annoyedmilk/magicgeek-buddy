#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Network trust model. Every WiFi AP the device has joined gets a stored
// trust level pinned to its BSSID + SSID. HTTP routes (/debug*, /ota*)
// look up the current AP's trust level and respond accordingly:
//
//   UNKNOWN  - never seen, pending user decision. HTTP returns 404.
//   DENY     - user explicitly denied. HTTP returns 404.
//   READONLY - user trusted for diagnostics. /debug* allowed, /ota* 403.
//   ADMIN    - user trusted fully. /debug* and /ota* both allowed.
//
// The default for a newly-joined network is UNKNOWN, which surfaces an
// on-device prompt; the user picks the level with the touch pad. If
// they don't respond within ~60s the choice falls back to DENY.
//
// The trust anchor is BSSID (router MAC) + SSID. Spoofing requires both
// values AND the WPA2 PSK; the practical threat (a peer at a coffee shop)
// has none of them.

typedef enum {
    NET_TRUST_UNKNOWN  = 0,
    NET_TRUST_DENY     = 1,
    NET_TRUST_READONLY = 2,
    NET_TRUST_ADMIN    = 3,
} net_trust_level_t;

// Load the persisted trust list from NVS. Idempotent; safe to call once
// at boot after storage_init_nvs().
void net_trust_init(void);

// Look up the trust level for a specific BSSID+SSID pair. Returns
// NET_TRUST_UNKNOWN if no matching entry is stored.
net_trust_level_t net_trust_lookup(const uint8_t bssid[6], const char *ssid);

// Store (or overwrite) the trust level for this BSSID+SSID. LRU-evicts
// the oldest entry if the table is full. Persists to NVS immediately.
void net_trust_store(const uint8_t bssid[6], const char *ssid, net_trust_level_t level);

// Convenience: trust level for the currently-joined STA AP. Returns
// NET_TRUST_UNKNOWN if not in STA mode or AP info is unavailable.
net_trust_level_t net_trust_current(void);

// Snapshot of the BSSID + SSID the STA is currently joined to, written
// into the caller's buffers. Returns false if not in STA mode. The SSID
// buffer should be >= 33 bytes (32 + NUL).
bool net_trust_current_ap(uint8_t bssid_out[6], char *ssid_out, size_t ssid_cap);

// Signal that a new (UNKNOWN) network was just joined; the render loop
// polls this to decide whether to open UI_TRUST_PROMPT.
bool net_trust_prompt_pending(void);

// Mark a prompt as consumed (the UI just opened it). Subsequent
// net_trust_prompt_pending() calls return false until the next STA
// join arms it again.
void net_trust_prompt_consumed(void);

// Called by wifi_manager from the IP_EVENT_STA_GOT_IP path with the
// freshly-joined AP's BSSID + SSID. Looks up the trust level; if UNKNOWN,
// arms the prompt. Returns the resolved level so the caller can log it.
net_trust_level_t net_trust_on_sta_connect(const uint8_t bssid[6], const char *ssid);

// True iff the currently-joined STA AP has at least `required` trust.
// In AP/captive-portal mode (no STA AP yet) this returns true regardless
// of `required`: the captive portal needs to be reachable to set up
// WiFi, and only the user with physical proximity is on it. The STA
// path is where the hardening matters.
bool net_trust_allows(net_trust_level_t required);
