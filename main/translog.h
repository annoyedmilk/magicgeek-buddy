#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Persistent transcript log on SPIFFS. Bridge calls translog_append_csv()
// on each heartbeat with the rendered CSV row; the file rotates between
// two ~200 KB segments so total flash usage is bounded and old data falls
// off the bottom (FIFO).
//
// Schema (one row per heartbeat or notable event):
//   uptime_s,event,total,running,waiting,tokens_today,prompt_tool,msg
//
// Events:
//   hb        = heartbeat (most rows)
//   prompt    = new permission prompt arrived
//   approve   = user approved (recorded after send)
//   deny      = user denied
//   conn      = ble link came up
//   disconn   = ble link dropped
//   stale     = bridge marked data stale (30s silence)

// Mount SPIFFS and prepare the rotating files. Safe if SPIFFS isn't
// formatted yet (formats on first boot). Returns ESP_OK on success;
// any other code means the translog is non-functional (subsequent
// append calls become no-ops).
esp_err_t translog_init(void);

// Append one row. The row text should NOT include a trailing newline -
// translog adds one. `event` is one of the strings in the header doc;
// `extras` is "field1,field2,..." (escaped if needed by the caller).
// Cheap and safe to call from the bridge hot path.
void translog_append(const char *event, const char *extras);

// Static-string status for the /debug page ("active 142/204 KB",
// "not mounted", etc.). Pointer is valid until the next call.
const char *translog_status_str(void);

// HTTP dump handler helper. Streams both segment files into the response
// (oldest first, newest at the bottom). Sets no headers itself - caller
// is expected to have set Content-Type/Disposition.
esp_err_t translog_dump_http(httpd_req_t *req);
