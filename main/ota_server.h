#pragma once
#include "esp_err.h"

// Claude-themed browser OTA. Registers /ota (GET upload page, POST .bin
// receiver) onto the shared HTTP server that wifi_manager owns.
//
// Endpoint shape mirrors the proven F1 Dashboard firmware:
//   GET  /ota   → drag-drop HTML page (Claude-night theme)
//   POST /ota   → raw .bin body; flashes to the next OTA partition then
//                 esp_restart()s. The bootloader's rollback support
//                 (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y) will recover
//                 if the new image doesn't reach ota_mark_valid().
//
// Returns ESP_FAIL if the shared httpd isn't up yet (call after WiFi
// reports STATE_ONLINE).
esp_err_t ota_start(void);

// Cancel pending rollback. Must be called once the freshly-flashed
// firmware has reached a "good enough" state (display + WiFi + BLE +
// bridge all up). Calling earlier would defeat the safety net.
void ota_mark_valid(void);
