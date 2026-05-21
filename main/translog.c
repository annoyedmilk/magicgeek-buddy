// Persistent transcript log on SPIFFS (256 KB partition).
//
// Wear management: SPIFFS handles internal wear-leveling, but we still
// have to bound the total bytes written. Strategy is a 2-file ring:
//
//   /translog/cur  - current append-only segment, capped at SEG_CAP.
//   /translog/old  - last full segment. Overwritten when cur rotates.
//
// On rotation: rename cur -> old (atomic on SPIFFS), then start a fresh
// cur. Dump = cat(old) + cat(cur). This gives us ~SEG_CAP * 2 of history
// with predictable wear (each flash byte gets rewritten once per
// 2 * SEG_CAP bytes of log traffic).
//
// At 1 heartbeat/sec with ~80 B/row, SEG_CAP = 100 KB holds ~22 minutes
// of activity per segment, ~44 minutes total. Plenty for debugging the
// "Mac woke and now we're stale" failure mode.

#include "translog.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "translog";

#define MOUNT_POINT "/translog"
#define CUR_PATH    MOUNT_POINT "/cur"
#define OLD_PATH    MOUNT_POINT "/old"
#define SEG_CAP     (100 * 1024)

static bool             s_mounted = false;
static SemaphoreHandle_t s_mux = NULL;
static size_t           s_cur_size = 0;
static char             s_status[64] = "not mounted";

static void update_status(void)
{
    if (!s_mounted) {
        snprintf(s_status, sizeof(s_status), "not mounted");
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    snprintf(s_status, sizeof(s_status),
             "active %u/%u KB (cur %u KB)",
             (unsigned)(used / 1024), (unsigned)(total / 1024),
             (unsigned)(s_cur_size / 1024));
}

esp_err_t translog_init(void)
{
    if (s_mounted) return ESP_OK;
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_FAIL;

    esp_vfs_spiffs_conf_t cfg = {
        .base_path        = MOUNT_POINT,
        .partition_label  = "storage",
        .max_files        = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t r = esp_vfs_spiffs_register(&cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(r));
        return r;
    }
    s_mounted = true;

    struct stat st;
    s_cur_size = (stat(CUR_PATH, &st) == 0) ? (size_t)st.st_size : 0;
    update_status();
    ESP_LOGI(TAG, "mounted; cur=%u B", (unsigned)s_cur_size);
    return ESP_OK;
}

// Rotate cur -> old. Caller holds s_mux.
static void rotate(void)
{
    unlink(OLD_PATH);                  // ignore ENOENT
    if (rename(CUR_PATH, OLD_PATH) != 0) {
        ESP_LOGW(TAG, "rotate failed - unlinking cur instead");
        unlink(CUR_PATH);
    }
    s_cur_size = 0;
}

void translog_append(const char *event, const char *extras)
{
    if (!s_mounted || !s_mux) return;
    if (!event) event = "?";
    if (!extras) extras = "";

    char line[256];
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int n = snprintf(line, sizeof(line), "%lu,%s,%s\n",
                     (unsigned long)up_s, event, extras);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_cur_size + (size_t)n > SEG_CAP) {
        rotate();
    }
    FILE *f = fopen(CUR_PATH, "a");
    if (f) {
        size_t w = fwrite(line, 1, (size_t)n, f);
        fclose(f);
        s_cur_size += w;
    }
    // Refresh status periodically (every ~16 rows is cheap enough).
    if ((s_cur_size & 0x3FF) == 0) update_status();
    xSemaphoreGive(s_mux);
}

const char *translog_status_str(void)
{
    return s_status;
}

static esp_err_t stream_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;   // no file -> just empty section
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return ESP_OK;
}

esp_err_t translog_dump_http(httpd_req_t *req)
{
    if (!s_mounted) {
        httpd_resp_sendstr(req, "translog not mounted\n");
        return ESP_OK;
    }
    static const char HEADER[] = "uptime_s,event,extras\n";
    httpd_resp_send_chunk(req, HEADER, sizeof(HEADER) - 1);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    stream_file(req, OLD_PATH);
    stream_file(req, CUR_PATH);
    xSemaphoreGive(s_mux);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
