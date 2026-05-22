#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

// In-RAM tee of ESP_LOG* output. Installed via esp_log_set_vprintf() so
// every log line continues to reach UART AND lands in a circular byte
// buffer. The /debug/log HTTP handler dumps a snapshot of the buffer.
//
// Sized at ~32 KB. The boot trace fits comfortably; once full, the oldest
// bytes are overwritten so the buffer always holds the most-recent log.
// Survives BLE traffic, OTA, and WiFi reconnects - cleared only on reboot.

// Must be called as early as possible in app_main(). Safe to call before
// NVS/WiFi/BLE; any logs that arrive after this returns are captured.
void debug_log_init(void);

// Copy at most `cap` bytes of the buffer into `out`. Returns bytes written.
// Reads are non-destructive (the ring keeps streaming for the next reader).
// Output is human-readable plain text in chronological order.
size_t debug_log_snapshot(char *out, size_t cap);

// Copy a chronological window: skip the first `skip` oldest bytes, then
// copy up to `cap` more bytes into `out`. Lets a stream-style HTTP dump
// page through the ring without ever allocating a buffer bigger than
// `cap` (a stack chunk in the handler).
// Returns the number of bytes actually written.
size_t debug_log_snapshot_at(size_t skip, char *out, size_t cap);

// Free bytes available before wrap (sanity number for the /debug page).
size_t debug_log_used(void);

// Register HTTP routes on the shared httpd:
//   GET /debug             - HTML status page with live log
//   GET /debug/log         - text/plain dump of the in-RAM log ring
//   GET /debug/status      - text/plain one-shot device snapshot (heap,
//                            uptime, version, BLE state, last heartbeat)
//
// Returns ESP_FAIL if the shared httpd isn't up.
esp_err_t debug_log_register_routes(httpd_handle_t server);
