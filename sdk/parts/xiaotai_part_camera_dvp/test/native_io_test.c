// SPDX-License-Identifier: MIT
#include <assert.h>
#include "camera_dvp_io.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"

/* The port is not linked into this test: every line passes the gate. */
bool xiaotai_log_enabled(uint8_t level, const char *tag)
{
    (void)level;
    (void)tag;
    return true;
}

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    uint16_t address;
} device_t;

static device_t devices[4];
static uint8_t device_count;
static int sccb_i2c_port;
static bool sccb_owns_i2c_port;

struct xiaotai_i2c_bus { int identity; };
static xiaotai_i2c_bus_t buses[3]; /* two installed, one foreign */
static unsigned lookups, removals[4], deinitializations, initializations, mirroring, flipping, takes, returns;
static int handles[4];
static void *busy_device;
static bool installed[4], native_open, external_dma, failed_init, deinit_fails, task_failed;
static bool early_init_error;
static sensor_t sensor;
static camera_config_t configured;
static camera_fb_t *queued;

const resolution_info_t resolution[] = {
    [FRAMESIZE_QQVGA] = {160, 120}, [FRAMESIZE_HQVGA] = {240, 176}, [FRAMESIZE_QVGA] = {320, 240},
};

void *xiaotai_i2c_native(xiaotai_i2c_bus_t *bus) { return bus; }

esp_err_t i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out)
{
    assert(port == 0 || port == 1);
    ++lookups;
    *out = &buses[port];
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device);
    unsigned i = 0;
    while (i < 4 && device != &handles[i]) ++i;
    assert(i < 3 && installed[i]); /* not an unrelated board device */
    ++removals[i];
    if (busy_device == device) return ESP_ERR_INVALID_STATE;
    installed[i] = false;
    return ESP_OK;
}

#include "sccb_release.inc"

static int mirror(sensor_t *self, int setting)
{
    assert(self == &sensor && setting == 0);
    ++mirroring;
    return 0;
}

static int flip(sensor_t *self, int setting)
{
    assert(self == &sensor && setting == 0);
    ++flipping;
    return 0;
}

esp_err_t esp_camera_init(const camera_config_t *cfg)
{
    assert(xiaotai_dvp_sccb_idle() && !native_open);
    ++initializations;
    configured = *cfg;
    assert(cfg->pin_sccb_sda == -1 && cfg->pin_sccb_scl == -1 && cfg->sccb_i2c_port == 1);
    assert(cfg->pin_pwdn == -1 && cfg->pin_reset == -1);
    assert(cfg->fb_count == 2 && cfg->fb_location == CAMERA_FB_IN_PSRAM && cfg->grab_mode == CAMERA_GRAB_LATEST);
    assert(cfg->pixel_format == PIXFORMAT_YUV422);
    if (early_init_error) return XIAOTAI_ERR_NO_MEM;
    sccb_i2c_port = cfg->sccb_i2c_port;
    for (unsigned i = 0; i < 3; ++i) {
        assert(!installed[i]);
        installed[i] = true;
        devices[i] = (device_t){.dev_handle = &handles[i], .address = (uint16_t)(0x10U + i)};
    }
    device_count = 3;
    if (failed_init) {
        /* Pinned public init calls its own deinit. SCCB drops its first
         * handle, then returns BUSY; public init returns the original error. */
        for (unsigned i = 0; i < device_count; ++i) {
            if (i2c_master_bus_rm_device(devices[i].dev_handle) != ESP_OK) return XIAOTAI_ERR_NOT_SUPPORTED;
            devices[i] = (device_t){0};
        }
        device_count = 0;
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    sensor = (sensor_t){.id.PID = 0x2145, .status.framesize = cfg->frame_size, .pixformat = cfg->pixel_format,
        .set_hmirror = mirror, .set_vflip = flip};
    native_open = true;
    return ESP_OK;
}

esp_err_t esp_camera_deinit(void)
{
    assert(native_open && xiaotai_dvp_sccb_idle());
    ++deinitializations;
    if (deinit_fails) return ESP_FAIL;
    native_open = false;
    return ESP_OK;
}

sensor_t *esp_camera_sensor_get(void) { return native_open ? &sensor : NULL; }
bool esp_camera_get_psram_mode(void) { return external_dma; }

camera_fb_t *xiaotai_dvp_try_take_raw(uint32_t *skipped)
{
    assert(native_open && skipped);
    ++takes;
    camera_fb_t *out = queued;
    queued = NULL;
    if (out) *skipped = 7;
    return out;
}

bool xiaotai_dvp_raw_ready(void) { return native_open && !task_failed; }

void esp_camera_fb_return(camera_fb_t *frame)
{
    assert(native_open && frame);
    ++returns;
    queued = frame;
}

int main(void)
{
    xiaotai_camera_dvp_config_t config = {.sccb = &buses[2], .xclk_hz = 20000000,
        .xclk_pin = 5, .xclk_timer = 1, .xclk_channel = 1, .data_pins = {16, 18, 8, 17, 15, 6, 4, 9},
        .vsync_pin = 3, .href_pin = 46, .pclk_pin = 7};
    xt_dvp_io_t *io, *other;
    installed[3] = true; /* camera never owns this board device */
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_NOT_FOUND && !io);
    assert(lookups == 2 && !initializations);
    config.sccb = &buses[1];
    sccb_owns_i2c_port = true;
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_INVALID_STATE && !io);
    assert(xiaotai_dvp_sccb_release_borrowed(1) == XIAOTAI_ERR_INVALID_STATE);
    sccb_owns_i2c_port = false;
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_OK && io);
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &other) == XIAOTAI_ERR_INVALID_STATE && !other);
    assert(lookups == 4 && configured.pin_xclk == 5 && configured.xclk_freq_hz == 20000000 &&
           configured.ledc_timer == 1 && configured.ledc_channel == 1);
    assert(configured.pin_d0 == 16 && configured.pin_d7 == 9 && configured.pin_vsync == 3 &&
           configured.pin_href == 46 && configured.pin_pclk == 7 && configured.frame_size == FRAMESIZE_QVGA);
    assert(xiaotai_dvp_sccb_release_borrowed(0) == XIAOTAI_ERR_INVALID_STATE && !removals[0]);

    uint16_t pid;
    bool direct;
    sensor.status.framesize = FRAMESIZE_HQVGA;
    assert(xt_dvp_io_sensor(io, &pid, &direct) == XIAOTAI_ERR_NOT_SUPPORTED);
    sensor.status.framesize = FRAMESIZE_QVGA;
    sensor.pixformat = PIXFORMAT_JPEG;
    assert(xt_dvp_io_sensor(io, &pid, &direct) == XIAOTAI_ERR_NOT_SUPPORTED);
    sensor.pixformat = PIXFORMAT_YUV422;
    external_dma = true;
    assert(xt_dvp_io_sensor(io, &pid, &direct) == XIAOTAI_OK && pid == 0x2145 && direct);
    external_dma = false;
    assert(xt_dvp_io_sensor(io, &pid, &direct) == XIAOTAI_OK && !direct);
    assert(xt_dvp_io_upright(io) == XIAOTAI_OK && mirroring == 1 && flipping == 1);
    xt_dvp_frame_t frame;
    assert(xt_dvp_io_take(io, &frame) == XIAOTAI_ERR_NOT_FOUND && takes == 1);
    static uint8_t pixels[320U * 240U * 2U];
    camera_fb_t fb = {.buf = pixels, .len = sizeof(pixels), .timestamp = {.tv_sec = 19, .tv_usec = 37}};
    queued = &fb;
    assert(xt_dvp_io_take(io, &frame) == XIAOTAI_OK && takes == 2 && frame.data == pixels && frame.token == &fb);
    assert(frame.size == sizeof(pixels) && frame.format == XIAOTAI_CAMERA_YUV422 && frame.width == 320 &&
           frame.height == 240 && frame.skipped == 7 && frame.timestamp_us == 19000037);
    assert(xt_dvp_io_take(io, &frame) == XIAOTAI_ERR_INVALID_STATE && takes == 2);
    assert(xt_dvp_io_close(io) == XIAOTAI_ERR_INVALID_STATE && !removals[0] && !deinitializations);
    xt_dvp_io_return(io, &frame);
    assert(!returns);
    xt_dvp_io_return(io, frame.token);
    assert(returns == 1);

    busy_device = &handles[1];
    assert(xt_dvp_io_close(io) == XIAOTAI_ERR_INVALID_STATE && native_open);
    assert(removals[0] == 1 && removals[1] == 1 && !removals[2] && !deinitializations);
    assert(xt_dvp_io_take(io, &frame) == XIAOTAI_ERR_INVALID_STATE && takes == 2);
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &other) == XIAOTAI_ERR_INVALID_STATE && !other);
    busy_device = NULL;
    deinit_fails = true;
    assert(xt_dvp_io_close(io) == XIAOTAI_FAIL && deinitializations == 1 && native_open);
    assert(removals[0] == 1 && removals[1] == 2 && removals[2] == 1 && installed[3]);
    deinit_fails = false;
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && deinitializations == 2 && !native_open);

    failed_init = true;
    config.xclk_pin = -1; /* external clock remains external */
    busy_device = &handles[1];
    assert(xt_dvp_io_open(&config, XT_DVP_240_176, &io) == XIAOTAI_ERR_NOT_SUPPORTED && io);
    assert(configured.pin_xclk == -1 && configured.frame_size == FRAMESIZE_HQVGA && !native_open);
    assert(!installed[0] && installed[1] && installed[2]);
    assert(xt_dvp_io_close(io) == XIAOTAI_ERR_INVALID_STATE && deinitializations == 2);
    busy_device = NULL;
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && deinitializations == 2 && xiaotai_dvp_sccb_idle());
    assert(removals[0] == 2 && removals[1] == 5 && removals[2] == 2 && installed[3] && !removals[3]);
    failed_init = false;
    assert(xt_dvp_io_open(&config, XT_DVP_160_120, &io) == XIAOTAI_OK && configured.frame_size == FRAMESIZE_QQVGA);
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && installed[3]);
    task_failed = true;
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_NO_MEM && io && native_open);
    assert(xt_dvp_io_take(io, &frame) == XIAOTAI_ERR_INVALID_STATE);
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &other) == XIAOTAI_ERR_INVALID_STATE && !other);
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && !native_open && xiaotai_dvp_sccb_idle());
    task_failed = false;
    failed_init = true;
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_NOT_SUPPORTED && !io);
    assert(!native_open && xiaotai_dvp_sccb_idle());
    failed_init = false;
    early_init_error = true;
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_NO_MEM && !io);
    assert(!native_open && xiaotai_dvp_sccb_idle());
    return 0;
}
