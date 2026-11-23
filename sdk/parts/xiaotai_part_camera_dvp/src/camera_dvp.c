// SPDX-License-Identifier: MIT
#include "camera_dvp_io.h"
#include "xiaotai/parts/camera_dvp_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef enum { POWER_OFF, POWER_ON, POWER_UNCONFIRMED } power_state_t;

typedef struct {
    xiaotai_camera_t camera;
    xiaotai_camera_dvp_config_t config;
    power_state_t power;
    xt_dvp_io_t *io;
    bool ready;
    xiaotai_camera_mode_t mode;
    xiaotai_camera_frame_t *loan;
    void *token;
#if XIAOTAI_PART_CAMERA_DVP_BENCH_POWER_DWELL
    int64_t powered_down_us;
#endif
} dvp_t;

#if XIAOTAI_PART_CAMERA_DVP_BENCH_POWER_DWELL
enum { BENCH_PWDN_HOLD_US = 100000, BENCH_PWDN_SETTLE_US = 50000 };

static void wait_since(int64_t since_us, int64_t limit_us)
{
    for (;;) {
        const int64_t left = limit_us - (xiaotai_time_us() - since_us);
        if (left <= 0) return;
        /* A tick may end before the requested wall time. Yield and recheck. */
        xiaotai_delay_ms((uint32_t)((left + 999) / 1000));
    }
}
#endif

static xiaotai_err_t power_off(dvp_t *dvp)
{
    const xiaotai_err_t err = xiaotai_line_set(&dvp->config.power, false);
    dvp->power = err == XIAOTAI_OK ? POWER_OFF : POWER_UNCONFIRMED;
#if XIAOTAI_PART_CAMERA_DVP_BENCH_POWER_DWELL
    if (err == XIAOTAI_OK) dvp->powered_down_us = xiaotai_time_us();
#endif
    return err;
}

static xiaotai_err_t set_power(void *self, bool enabled)
{
    dvp_t *dvp = self;
    const xiaotai_camera_dvp_config_t *config = &dvp->config;
    if (!enabled) {
        if (dvp->io || dvp->loan) return XIAOTAI_ERR_INVALID_STATE;
        if (dvp->power == POWER_OFF) return XIAOTAI_OK;
        if (config->hold_reset_when_off) {
            const xiaotai_err_t err = xiaotai_line_set(&config->reset, true);
            if (err != XIAOTAI_OK) {
                dvp->power = POWER_UNCONFIRMED;
                return err;
            }
        }
        return power_off(dvp);
    }
    if (dvp->power == POWER_ON) return XIAOTAI_OK;
    if (dvp->power == POWER_UNCONFIRMED) return XIAOTAI_ERR_INVALID_STATE;
#if XIAOTAI_PART_CAMERA_DVP_BENCH_POWER_DWELL
    const bool bench_power = xiaotai_line_connected(&config->power);
    if (bench_power) wait_since(dvp->powered_down_us, BENCH_PWDN_HOLD_US);
#endif
    xiaotai_err_t err = xiaotai_line_set(&config->reset, true);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_line_set(&config->power, true);
    if (err == XIAOTAI_OK) {
#if XIAOTAI_PART_CAMERA_DVP_BENCH_POWER_DWELL
        if (bench_power) wait_since(xiaotai_time_us(), BENCH_PWDN_SETTLE_US);
        else
#endif
        if (config->power_settle_ms) xiaotai_delay_ms(config->power_settle_ms);
        err = xiaotai_line_set(&config->reset, false);
    }
    if (err != XIAOTAI_OK) {
        /* A line write can fail after the peripheral accepted it. Confirm
         * power off before allowing a retry; preserve the first error. */
        const xiaotai_err_t reset = config->hold_reset_when_off ? xiaotai_line_set(&config->reset, true) : XIAOTAI_OK;
        (void)power_off(dvp);
        if (reset != XIAOTAI_OK) dvp->power = POWER_UNCONFIRMED;
        return err;
    }
    if (config->reset_settle_ms) xiaotai_delay_ms(config->reset_settle_ms);
    dvp->power = POWER_ON;
    return XIAOTAI_OK;
}

static xiaotai_err_t open_camera(void *self, const xiaotai_camera_mode_t *mode)
{
    dvp_t *dvp = self;
    if (!mode) return XIAOTAI_ERR_INVALID_ARG;
    if (dvp->io || dvp->power != POWER_ON) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_camera_desc_t *desc = &dvp->camera.desc;
    if (mode->format != XIAOTAI_CAMERA_YUV422 || mode->width > desc->max_width || mode->height > desc->max_height)
        return XIAOTAI_ERR_NOT_SUPPORTED;
    xt_dvp_size_t size;
    if (mode->width == 160 && mode->height == 120) size = XT_DVP_160_120;
    else if (mode->width == 240 && mode->height == 176) size = XT_DVP_240_176;
    else if (mode->width == 320 && mode->height == 240) size = XT_DVP_320_240;
    else return XIAOTAI_ERR_NOT_SUPPORTED;

    xiaotai_err_t err = xt_dvp_io_open(&dvp->config, size, &dvp->io);
    if (err != XIAOTAI_OK) return err;
    if (!dvp->io) return XIAOTAI_FAIL;
    uint16_t sensor;
    bool external_dma;
    err = xt_dvp_io_sensor(dvp->io, &sensor, &external_dma);
    if (err != XIAOTAI_OK) return err;
    if (sensor != dvp->config.sensor_id || external_dma) return XIAOTAI_ERR_NOT_SUPPORTED;
    err = xt_dvp_io_upright(dvp->io);
    if (err != XIAOTAI_OK) return err;
    dvp->mode = *mode;
    dvp->ready = true;
    return XIAOTAI_OK;
}

static xiaotai_err_t close_camera(void *self)
{
    dvp_t *dvp = self;
    if (dvp->loan) return XIAOTAI_ERR_INVALID_STATE;
    if (!dvp->io) return XIAOTAI_OK;
    dvp->ready = false;
    const xiaotai_err_t err = xt_dvp_io_close(dvp->io);
    if (err == XIAOTAI_OK) dvp->io = NULL;
    return err;
}

static xiaotai_err_t borrow_frame(void *self, xiaotai_camera_frame_t *frame)
{
    dvp_t *dvp = self;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (!dvp->ready || dvp->loan) return XIAOTAI_ERR_INVALID_STATE;
    xt_dvp_frame_t raw;
    xiaotai_err_t err = xt_dvp_io_take(dvp->io, &raw);
    if (err != XIAOTAI_OK) return err;
    const size_t bytes = (size_t)dvp->mode.width * dvp->mode.height * 2U;
    if (raw.format != XIAOTAI_CAMERA_YUV422) err = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (!raw.data || raw.size < bytes || raw.width != dvp->mode.width || raw.height != dvp->mode.height)
        err = XIAOTAI_ERR_INVALID_SIZE;
    if (err != XIAOTAI_OK) {
        xt_dvp_io_return(dvp->io, raw.token);
        return err;
    }
    *frame = (xiaotai_camera_frame_t){.data = raw.data, .size = raw.size, .width = dvp->mode.width,
        .height = dvp->mode.height, .rows = dvp->mode.height, .format = raw.format, .captured_us = raw.timestamp_us,
        .skipped = raw.skipped};
    dvp->loan = frame;
    dvp->token = raw.token;
    return XIAOTAI_OK;
}

static xiaotai_err_t release_frame(void *self, xiaotai_camera_frame_t *frame)
{
    dvp_t *dvp = self;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (dvp->loan != frame) return XIAOTAI_ERR_INVALID_STATE;
    xt_dvp_io_return(dvp->io, dvp->token);
    dvp->loan = NULL;
    dvp->token = NULL;
    *frame = (xiaotai_camera_frame_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_camera_dvp_create(const xiaotai_camera_dvp_config_t *config, const xiaotai_camera_t **camera)
{
    if (camera) *camera = NULL;
    if (!config || !camera || !config->sccb || config->xclk_pin < -1 || !config->xclk_hz || !config->sensor_id ||
        config->vsync_pin < 0 || config->href_pin < 0 || config->pclk_pin < 0 || config->rotation > 3U ||
        config->max_width < 160 || config->max_height < 120 || (config->power.get && !config->power.set) ||
        (config->reset.get && !config->reset.set)) return XIAOTAI_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 8; ++i) if (config->data_pins[i] < 0) return XIAOTAI_ERR_INVALID_ARG;
    dvp_t *dvp = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*dvp));
    if (!dvp) return XIAOTAI_ERR_NO_MEM;
    dvp->config = *config;
    const xiaotai_err_t reset = xiaotai_line_set(&config->reset, true);
    const xiaotai_err_t off = power_off(dvp);
    const xiaotai_err_t err = reset != XIAOTAI_OK ? reset : off;
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(dvp);
        return err;
    }
    dvp->camera = (xiaotai_camera_t){.desc = {.formats = XIAOTAI_CAMERA_YUV422,
        .min_width = 160, .min_height = 120, .max_width = config->max_width, .max_height = config->max_height,
        .max_fps = config->max_fps, .rotation = config->rotation}, .set_power = set_power, .open = open_camera,
        .close = close_camera, .borrow = borrow_frame, .release = release_frame, .self = dvp};
    *camera = &dvp->camera;
    return XIAOTAI_OK;
}
