// SPDX-License-Identifier: MIT
/* Executes the exact, hash-checked vendor init/deinit functions included by
 * check_native_context.py. Only the hardware and sensor boundaries are fakes. */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL = -1, ESP_ERR_NOT_SUPPORTED = 0x106,
       ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE = 0x20001, GAINCEILING_2X, OV2640_PID = 0x2640 };
typedef enum { FRAMESIZE_QVGA } framesize_t;
typedef enum { PIXFORMAT_YUV422, PIXFORMAT_JPEG } pixformat_t;
typedef enum { CAMERA_NONE, CAMERA_GC } camera_model_t;
typedef struct { framesize_t frame_size; pixformat_t pixel_format; int jpeg_quality; } camera_config_t;
typedef struct sensor sensor_t;
struct sensor {
    struct { framesize_t framesize; } status;
    struct { unsigned PID; } id;
    pixformat_t pixformat;
    int (*set_framesize)(sensor_t *, framesize_t);
    int (*set_pixformat)(sensor_t *, pixformat_t);
    int (*init_status)(sensor_t *);
    int (*set_gainceiling)(sensor_t *, int);
    int (*set_bpc)(sensor_t *, int);
    int (*set_wpc)(sensor_t *, int);
    int (*set_lenc)(sensor_t *, int);
    int (*set_quality)(sensor_t *, int);
};
typedef struct { sensor_t sensor; } camera_state_t;
static camera_state_t *s_state;
static camera_config_t s_saved_config;
static const struct { bool support_jpeg; framesize_t max_size; } camera_sensor[] = {
    {false, FRAMESIZE_QVGA}, {false, FRAMESIZE_QVGA},
};
static const struct { int width, height; } resolution[] = {{320, 240}};
static bool format_fails, status_fails, framesize_fails;
static bool sccb_release_fails;
static unsigned formats, statuses, starts, cleanups, releases;
static unsigned sensor_pid;
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define CAMERA_DISABLE_OUT_CLOCK() ((void)0)

static int frame_size(sensor_t *sensor, framesize_t size)
{
    assert(sensor == &s_state->sensor && size == FRAMESIZE_QVGA);
    return framesize_fails ? -1 : 0;
}
static int format(sensor_t *sensor, pixformat_t pixel_format)
{
    assert(sensor == &s_state->sensor && pixel_format == PIXFORMAT_YUV422);
    ++formats;
    return format_fails ? -1 : 0;
}
static int read_status(sensor_t *sensor)
{
    assert(sensor == &s_state->sensor);
    ++statuses;
    return status_fails ? -1 : 0;
}
static esp_err_t cam_init(const camera_config_t *config) { (void)config; return ESP_OK; }
static esp_err_t camera_probe(const camera_config_t *config, camera_model_t *model)
{
    (void)config;
    assert(!s_state);
    s_state = calloc(1, sizeof(*s_state));
    assert(s_state);
    s_state->sensor = (sensor_t){.id.PID = sensor_pid, .set_framesize = frame_size,
        .set_pixformat = format, .init_status = read_status};
    *model = CAMERA_GC;
    return ESP_OK;
}
static esp_err_t cam_config(const camera_config_t *config, framesize_t size, unsigned pid)
{
    assert(config && size == FRAMESIZE_QVGA && pid == sensor_pid);
    return ESP_OK;
}
static void cam_start(void) { ++starts; }
static esp_err_t cam_deinit(void) { ++cleanups; return ESP_OK; }
static int SCCB_Deinit(void) { ++releases; return sccb_release_fails ? ESP_FAIL : ESP_OK; }
esp_err_t esp_camera_deinit(void);

#include "wrapper_lifecycle.inc"

int main(int argc, char **argv)
{
    const bool legacy = argc > 1;
    (void)argv;
    const camera_config_t config = {.frame_size = FRAMESIZE_QVGA, .pixel_format = PIXFORMAT_YUV422};
    (void)resolution; /* logging boundary does not format arguments in the fixture */
    for (unsigned p = 0; p < 2; ++p) {
        const unsigned old_formats = formats, old_statuses = statuses;
        sensor_pid = p ? 0x2145 : 0x308;
        format_fails = true;
        const esp_err_t format_error = esp_camera_init(&config);
        assert(formats == old_formats + 1U);
        if (legacy) {
            assert(format_error == ESP_OK && s_state && starts == 1U && statuses == 1U);
            assert(esp_camera_deinit() == ESP_OK && !s_state);
            return 0;
        }
        assert(format_error == ESP_FAIL && !s_state && starts == p && statuses == old_statuses);
        format_fails = false;
        status_fails = true;
        assert(esp_camera_init(&config) == ESP_FAIL && !s_state && starts == p);
        status_fails = false;
        framesize_fails = true;
        assert(esp_camera_init(&config) == ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE && !s_state && starts == p);
        framesize_fails = false;
        assert(esp_camera_init(&config) == ESP_OK && s_state && starts == p + 1U);
        assert(esp_camera_deinit() == ESP_OK && !s_state);
        /* First-call failures are not retried and all four attempts release. */
        assert(formats == old_formats + 3U && statuses == old_statuses + 2U);
        assert(cleanups == (p + 1U) * 4U && releases == cleanups);
    }
    format_fails = sccb_release_fails = true;
    const unsigned old_starts = starts, old_formats = formats, old_releases = releases;
    assert(esp_camera_init(&config) == ESP_FAIL && !s_state && starts == old_starts);
    assert(formats == old_formats + 1U && releases == old_releases + 1U);
    /* The pinned public wrapper frees s_state despite the SCCB cleanup error.
     * Native adapter tests separately retain and retry these residual devices. */
    return 0;
}
