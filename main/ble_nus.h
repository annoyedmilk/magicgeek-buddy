#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Nordic UART Service over NimBLE - the BLE link the Claude desktop apps
// use (developer mode) per ~/Downloads/claude-desktop-buddy-main/REFERENCE.md.
//
//   Service UUID  6e400001-b5a3-f393-e0a9-e50e24dcca9e
//   RX char       6e400002-b5a3-f393-e0a9-e50e24dcca9e   (client → device, WRITE)
//   TX char       6e400003-b5a3-f393-e0a9-e50e24dcca9e   (device → client, NOTIFY)
//
// Bonded LE Secure Connections with DisplayOnly IO: the device shows a
// 6-digit passkey on screen and the central enters it. NUS chars are
// encrypted-only so all transcript snippets ride the AES-CCM link.
//
// The C API is the byte-stream surface the bridge consumes: poll
// ble_available() / ble_read() for incoming newline-delimited JSON,
// ble_write() for outgoing (chunked to MTU here).

// Initialize the NimBLE stack and start advertising. Call AFTER WiFi/FB
// init so allocations happen against the realistic heap.
//   name: advertised name, e.g. "Claude-XXXX" (last MAC bytes appended)
void ble_init(const char *name);

bool ble_connected(void);

// True once LE Secure Connections bonding has completed for the current
// link. The NUS chars are encrypted-only so in practice this becomes true
// before any user data flows; exposed so the bridge's status ack can echo
// it back to the desktop ("sec":true).
bool ble_secure(void);

// Non-zero while a 6-digit pairing passkey should be on screen. main.c
// renders it; auto-cleared on auth complete or disconnect.
uint32_t ble_passkey(void);

// Erase all stored bonds (long-term keys) from NVS. Called from the
// {"cmd":"unpair"} bridge command and from factory reset.
void ble_clear_bonds(void);

// Bytes available to read from the RX ring buffer (data written by the
// central to the RX char).
size_t ble_available(void);

// Read one byte; returns -1 if the buffer is empty.
int ble_read(void);

// Notify-write to the TX char. Chunks the payload to fit (MTU - 3); waits
// briefly between chunks for the stack to drain. Returns bytes accepted.
size_t ble_write(const uint8_t *data, size_t len);

// Force-terminate the current BLE connection (if any). The disconnect
// handler will fire and re-advertise. Used by the bridge as a recovery
// hook when the link is up at the LL layer but heartbeats have stopped
// flowing - the Mac's CoreBluetooth client occasionally desynchronizes
// from the Claude desktop app's view of the link, and only a full
// teardown gets both sides agreeing again.
// Returns true if a disconnect was initiated, false if no link was up.
bool ble_disconnect(void);

// Call before ble_disconnect() when the reason is a stale heartbeat timeout.
// Arms a 30 s delay before re-advertising so CoreBluetooth cannot instantly
// auto-reconnect via stored LTK, giving the Hardware Buddy UI time to
// transition to "Disconnected" and the user a window to reconnect explicitly.
// No effect on normal (Mac-initiated) disconnects.
void ble_arm_stale_delay(void);
