/* Temporary, opt-in pre-SDK H264 evidence capture. Never persists user media. */
#include "p4_video_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "starter_tirtc.h"
#include "camera_pipeline.h"

#define CAPTURE_CAP (512U * 1024U)
static SemaphoreHandle_t s_mutex;
static atomic_bool s_active, s_missed;
static uint8_t *s_data;
static size_t s_size;
static uint32_t s_generation, s_frames;
static uint16_t s_width, s_height;
static int s_mode;
static int64_t s_deadline;
static bool s_started;
static const char *s_reason = "empty";

/* Annex B validation: do not export a clip starting without SPS/PPS/IDR. */
static bool has_headers_and_idr(const uint8_t *data, size_t len)
{
    unsigned mask = 0;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] || data[i + 1]) continue;
        size_t n = 0;
        if (data[i + 2] == 1) n = i + 3;
        else if (i + 4 < len && data[i + 2] == 0 && data[i + 3] == 1) n = i + 4;
        if (!n) continue;
        unsigned type = data[n] & 31U;
        if (type == 7) mask |= 1;
        if (type == 8) mask |= 2;
        if (type == 5) return mask == 3;
    }
    return false;
}

void p4_video_capture_offer(const uint8_t *data, size_t len, uint16_t w,
                            uint16_t h, bool key, uint32_t generation)
{
    if (!atomic_load(&s_active)) return;
    /* No waiting or I/O on the encoder task. A missed frame terminates the clip. */
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_store(&s_missed, true);
        return;
    }
    if (!atomic_load(&s_active)) goto done;
    if (atomic_load(&s_missed)) { s_reason = "contention"; goto stop; }
    if (generation != s_generation) { s_reason = "session-changed"; goto stop; }
    if (esp_timer_get_time() >= s_deadline) { s_reason = "time-limit"; goto stop; }
    if (!s_started) {
        if (!key || !has_headers_and_idr(data, len)) goto done;
        s_started = true;
        s_width = w; s_height = h;
        s_deadline = esp_timer_get_time() + 2000000;
    }
    if (w != s_width || h != s_height) { s_reason = "size-changed"; goto stop; }
    if (len > CAPTURE_CAP - s_size) { s_reason = "capacity"; goto stop; }
    memcpy(s_data + s_size, data, len);
    s_size += len;
    ++s_frames;
    goto done;
stop:
    atomic_store(&s_active, false);
done:
    xSemaphoreGive(s_mutex);
}

static uint32_t checksum(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < len; ++i) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}

static int capture_command(int argc, char **argv)
{
    if (argc != 2) {
        puts("usage: video-capture start|stop|status|dump|clear");
        return 1;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = 0;
    if (!strcmp(argv[1], "start")) {
        if (!starter_tirtc_video_ready() || atomic_load(&s_active) || s_data) {
            puts("VCAP ERROR require active video and clear previous capture"); rc = 1;
        } else {
            s_data = heap_caps_malloc(CAPTURE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_data) { puts("VCAP ERROR PSRAM allocation failed"); rc = 1; }
            else {
                s_size = 0; s_frames = 0; s_width = s_height = 0;
                s_started = false; s_reason = "armed";
                s_generation = starter_tirtc_generation(); s_mode = starter_tirtc_mode();
                s_deadline = esp_timer_get_time() + 5000000;
                atomic_store(&s_missed, false);
                atomic_store(&s_active, true);
                camera_pipeline_request_key_frame();
                puts("VCAP ARMED");
            }
        }
    } else if (!strcmp(argv[1], "stop")) {
        if (atomic_exchange(&s_active, false)) s_reason = "manual-stop";
    } else if (!strcmp(argv[1], "clear")) {
        atomic_store(&s_active, false);
        free(s_data); s_data = NULL; s_size = 0; s_frames = 0;
        s_width = s_height = 0; s_reason = "empty";
    } else if (!strcmp(argv[1], "dump")) {
        if (atomic_load(&s_active) || starter_tirtc_connected() || !s_size) {
            puts("VCAP ERROR stop capture and end session before dump; nonempty clip required"); rc = 1;
        } else {
            /* Console task only. Export after hangup, never from the video callback. */
            printf("\nVCAP BEGIN bytes=%u fnv=%08lx mode=%d generation=%lu width=%u height=%u frames=%lu\n",
                   (unsigned)s_size, (unsigned long)checksum(s_data, s_size), s_mode,
                   (unsigned long)s_generation, s_width, s_height, (unsigned long)s_frames);
            for (size_t i = 0; i < s_size; i += 64) {
                char line[160];
                int n = snprintf(line, sizeof(line), "VCAP DATA %08x ", (unsigned)i);
                size_t count = s_size - i < 64 ? s_size - i : 64;
                static const char hex[] = "0123456789abcdef";
                for (size_t j = 0; j < count; ++j) {
                    line[n++] = hex[s_data[i + j] >> 4];
                    line[n++] = hex[s_data[i + j] & 15];
                }
                line[n++] = '\n'; line[n] = 0;
                fputs(line, stdout);
            }
            puts("VCAP END");
        }
    } else if (strcmp(argv[1], "status")) {
        puts("VCAP ERROR unknown command"); rc = 1;
    }
    printf("VCAP STATUS active=%d bytes=%u frames=%lu width=%u height=%u reason=%s\n",
           atomic_load(&s_active), (unsigned)s_size, (unsigned long)s_frames,
           s_width, s_height, s_reason);
    xSemaphoreGive(s_mutex);
    return rc;
}

esp_err_t p4_video_capture_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    const esp_console_cmd_t cmd = {.command = "video-capture",
        .help = "Temporary pre-SDK H264: start|stop|status|dump|clear", .func = capture_command};
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret != ESP_OK) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
    return ret;
}
