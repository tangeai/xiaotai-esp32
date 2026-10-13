"""Compile camera ownership functions with injected driver/allocator failures."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_camera.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"static (?:bool|esp_err_t) " + name + r"\([^)]*\)\s*\{.*?\n\}", source, re.S)
    assert match, name
    return match[0]


owner = re.search(r"typedef struct \{.*?\} camera_owner_t;", source, re.S)
assert owner
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_NO_MEM 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define VIEW_JPEG_BYTES 32768U
#define VIEW_STACK_BYTES 16384U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define GC0308_PID 0x9b
#define PIXFORMAT_YUV422 5
#define FRAMESIZE_QQVGA 1
#define FRAMESIZE_HQVGA 2
#define CAMERA_FB_IN_PSRAM 1
#define CAMERA_GRAB_LATEST 1
#define LEDC_TIMER_1 1
#define LEDC_CHANNEL_1 1
#define JPEG_PIXEL_FORMAT_YCbYCr 7
#define JPEG_SUBSAMPLE_420 3
#define JPEG_ERR_OK 0
#define JPEG_ERR_NO_MEM 1
#define pdMS_TO_TICKS(x) (x)
enum { STARTER_CAMERA_OFF, STARTER_CAMERA_STARTING, STARTER_CAMERA_RUNNING,
       STARTER_CAMERA_STOPPING, STARTER_CAMERA_ERROR };
typedef int esp_err_t;
typedef int jpeg_error_t;
typedef void *jpeg_enc_handle_t;
typedef struct {
    int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl, sccb_i2c_port;
    int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
    int pin_vsync, pin_href, pin_pclk, xclk_freq_hz, ledc_timer, ledc_channel;
    int pixel_format, frame_size, jpeg_quality, fb_count, fb_location, grab_mode;
} camera_config_t;
typedef struct sensor sensor_t;
struct sensor { struct { int PID; } id; int (*set_hmirror)(sensor_t *, int);
               int (*set_vflip)(sensor_t *, int); };
typedef struct { unsigned width, height, src_type, subsampling, quality;
                 bool task_enable; } jpeg_enc_config_t;
#define DEFAULT_JPEG_ENC_CONFIG() ((jpeg_enc_config_t){0})
static atomic_uint s_requested;
static struct { atomic_int phase, error; atomic_uint width, height, quality; } s_diag;
static unsigned current, connection;
static bool subscribed, powered, driver, encoder;
static void *allocation;
static int stage, fail_at, cancel_at, close_error, power_error;
static int power_calls, init_calls, deinit_calls;
static unsigned expected_width, expected_height;
static bool step(void) {
    ++stage;
    if (stage == cancel_at) s_requested = 0;
    return stage == fail_at;
}
static bool starter_media_h5_current(uint32_t g) { return current == g; }
static bool starter_tirtc_video_ready(void) { return subscribed; }
static uint32_t starter_tirtc_generation(void) { return connection; }
static void heap_log(const char *s) { (void)s; }
static esp_err_t starter_media_camera_power(bool on) {
    ++power_calls;
    if (on && step()) return ESP_FAIL;
    powered = on;
    return !on ? power_error : ESP_OK;
}
static void vTaskDelay(unsigned ticks) { assert(ticks == 10); (void)step(); }
static int mirror(sensor_t *s, int value) { assert(s && value == 0); return step() ? -1 : 0; }
static sensor_t sensor = { .id.PID = GC0308_PID, .set_hmirror = mirror, .set_vflip = mirror };
static esp_err_t esp_camera_init(const camera_config_t *c) {
    ++init_calls;
    assert(powered && !driver && c->pin_xclk == -1 && c->xclk_freq_hz == 20000000);
    assert(c->pin_sccb_sda == -1 && c->pin_sccb_scl == -1 && c->sccb_i2c_port == 0);
    assert(c->pin_reset == -1 && c->pin_pwdn == -1);
    assert(c->pin_d0 == 39 && c->pin_d1 == 40 && c->pin_d2 == 41 && c->pin_d3 == 42);
    assert(c->pin_d4 == 15 && c->pin_d5 == 16 && c->pin_d6 == 48 && c->pin_d7 == 47);
    assert(c->pin_pclk == 45 && c->pin_vsync == 46 && c->pin_href == 38);
    assert(c->pixel_format == PIXFORMAT_YUV422 && c->fb_count == 2);
    assert(c->fb_location == CAMERA_FB_IN_PSRAM && c->grab_mode == CAMERA_GRAB_LATEST);
    assert(c->frame_size == (expected_width == 160 ? FRAMESIZE_QQVGA : FRAMESIZE_HQVGA));
    if (step()) return ESP_FAIL;
    driver = true; return ESP_OK;
}
static sensor_t *esp_camera_sensor_get(void) { return step() ? NULL : &sensor; }
static bool esp_camera_get_psram_mode(void) { return false; }
static esp_err_t esp_camera_deinit(void) {
    assert(driver); ++deinit_calls; driver = false; return close_error;
}
static void *heap_caps_aligned_alloc(size_t align, size_t bytes, int caps) {
    assert(driver && !allocation && align == 16 && bytes == VIEW_JPEG_BYTES);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (step()) return NULL;
    allocation = aligned_alloc(align, bytes); assert(allocation); return allocation;
}
static void heap_caps_free(void *p) {
    assert(!encoder && p == allocation); free(p); allocation = NULL;
}
static jpeg_error_t jpeg_enc_open(const jpeg_enc_config_t *c, jpeg_enc_handle_t *out) {
    assert(driver && allocation && !encoder);
    assert(c->width == expected_width && c->height == expected_height && c->quality == 45);
    assert(c->src_type == JPEG_PIXEL_FORMAT_YCbYCr && c->subsampling == JPEG_SUBSAMPLE_420);
    assert(!c->task_enable);
    if (step()) return JPEG_ERR_NO_MEM;
    encoder = true; *out = (void *)1; return JPEG_ERR_OK;
}
static jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t h) {
    assert(h && encoder && !driver); encoder = false; return close_error ? 2 : JPEG_ERR_OK;
}
'''
main = r'''
static void reset(bool small) {
    assert(!powered && !driver && !encoder && !allocation);
    s_requested = current = connection = 17; subscribed = true;
    stage = fail_at = cancel_at = close_error = power_error = 0;
    expected_width = small ? 160 : 240; expected_height = small ? 120 : 176;
    sensor.id.PID = GC0308_PID;
}
static void cleanup(camera_owner_t *o) {
    assert(close_camera(o) == ESP_OK);
    assert(!o->initialized && !o->encoder && !o->jpeg);
    assert(!powered && !driver && !encoder && !allocation && s_diag.phase == STARTER_CAMERA_OFF);
}
int main(void) {
    camera_owner_t o = {0}; reset(false);
    int before = power_calls;
    s_requested = 0; assert(open_camera(&o, 17, false) == ESP_ERR_INVALID_STATE);
    s_requested = 17; subscribed = false; assert(open_camera(&o, 17, false) == ESP_ERR_INVALID_STATE);
    subscribed = true; connection = 18; assert(open_camera(&o, 17, false) == ESP_ERR_INVALID_STATE);
    connection = 17; current = 18; assert(open_camera(&o, 17, false) == ESP_ERR_INVALID_STATE);
    assert(power_calls == before && init_calls == 0);
    for (int i = 0; i < 1000; ++i) {
        bool small = i % 2; reset(small);
        assert(open_camera(&o, 17, small) == ESP_OK);
        assert(stage == 8 && s_diag.phase == STARTER_CAMERA_RUNNING);
        cleanup(&o);
    }
    for (int i = 1; i <= 8; ++i) {
        if (i == 2) continue; /* Delay cannot return an I/O error. */
        reset(false); fail_at = i;
        assert(open_camera(&o, 17, false) != ESP_OK); cleanup(&o);
    }
    reset(false); sensor.id.PID = 0x2145;
    assert(open_camera(&o, 17, false) == ESP_ERR_NOT_SUPPORTED); cleanup(&o);
    for (int i = 2; i <= 8; ++i) {
        reset(false); cancel_at = i;
        (void)open_camera(&o, 17, false);
        assert(!admitted(17)); cleanup(&o);
    }
    reset(false); assert(open_camera(&o, 17, false) == ESP_OK);
    close_error = ESP_FAIL;
    assert(close_camera(&o) == ESP_FAIL && s_diag.phase == STARTER_CAMERA_ERROR);
    assert(!driver && !encoder && !allocation && !powered);
    reset(false); assert(open_camera(&o, 17, false) == ESP_OK);
    power_error = ESP_FAIL;
    assert(close_camera(&o) == ESP_FAIL && s_diag.error == ESP_FAIL);
    assert(!driver && !encoder && !allocation && !powered);
    assert(init_calls > 1000 && deinit_calls >= 1000);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-camera-owner-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + owner[0] + function("admitted") +
                                  function("close_camera") + function("open_camera") + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: GC0308 owner x1000, subscription/generation, PSRAM, init failure/cancel and close errors")
