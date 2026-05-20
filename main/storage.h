#pragma once

#include "esp_err.h"
#include <stddef.h>

// NVS-backed key-value store. Namespace "buddy" holds WiFi creds plus
// stats / settings / owner / petname / species.

/**
 * Initialize NVS flash. Erases and re-inits on corruption.
 */
esp_err_t storage_init_nvs(void);

// --- NVS convenience wrappers ---

esp_err_t storage_get_str(const char *key, char *out, size_t max_len);
esp_err_t storage_set_str(const char *key, const char *value);
esp_err_t storage_get_blob(const char *key, void *out, size_t *len);
esp_err_t storage_set_blob(const char *key, const void *data, size_t len);
esp_err_t storage_get_u32(const char *key, uint32_t *out, uint32_t def);
esp_err_t storage_set_u32(const char *key, uint32_t value);
esp_err_t storage_erase_key(const char *key);
esp_err_t storage_erase_all(void);   // factory reset: wipe the namespace
