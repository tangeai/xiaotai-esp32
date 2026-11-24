// SPDX-License-Identifier: MIT
#include "camera_dvp_io.h"

#include <limits.h>
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "xiaotai/port/log.h"
#ifndef XIAOTAI_DVP_USE_OVERLAY
#include "xiaotai/port/os.h"
#endif
#if defined(CONFIG_XIAOTAI_PART_CAMERA_DVP_BENCH_DIAG) && defined(CONFIG_IDF_TARGET_ESP32S3) && \
    !defined(XIAOTAI_DVP_USE_OVERLAY)
#define XIAOTAI_DVP_BENCH_DIAG 1
#include "dvp_bench_window.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/lcd_cam_struct.h"
#include "soc/soc.h"
#endif

#if CONFIG_CAMERA_PSRAM_DMA || !CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW
#error "DVP requires internal DMA staging and a borrowed new-I2C SCCB controller"
#endif

#ifdef XIAOTAI_DVP_USE_OVERLAY
/* Private entry points of the fingerprinted build-local driver copies. */
camera_fb_t *xiaotai_dvp_try_take_raw(uint32_t *skipped);
bool xiaotai_dvp_raw_ready(void);
bool xiaotai_dvp_sccb_idle(void);
esp_err_t xiaotai_dvp_sccb_release_borrowed(int port);
#else
typedef enum { FETCH_IDLE, FETCH_PENDING, FETCH_READY } fetch_state_t;

typedef struct {
    xiaotai_signal_t request;
    camera_fb_t *frame;
    xiaotai_err_t result;
    fetch_state_t state;
    bool started, stop, finished;
#ifdef XIAOTAI_DVP_BENCH_DIAG
    dvp_bench_window_t bench;
    uint32_t bench_pins[DVP_BENCH_SIGNALS];
    uint32_t bench_xclk_hz;
    int bench_xclk_pin;
#endif
} stock_capture_t;

/* esp_camera_fb_get may wait 4 s even when availability was checked. */
#define STOCK_STOP_MS 4500U
#define STOCK_STACK_BYTES 4096U
#endif

struct xt_dvp_io {
    bool claimed, camera_open, closing, upright;
#ifdef XIAOTAI_DVP_USE_OVERLAY
    int sccb_port;
#else
    stock_capture_t stock;
#endif
    framesize_t size;
    size_t width, height;
    camera_fb_t *loan;
};

/* esp32-camera owns one global controller; HAL operations have one owner. */
static xt_dvp_io_t controller;

#ifndef XIAOTAI_DVP_USE_OVERLAY
static xiaotai_critical_t stock_lock;
static bool stock_lock_initialized;

#ifdef XIAOTAI_DVP_BENCH_DIAG
static void bench_report(const stock_capture_t *stock, bool first_frame)
{
    const dvp_bench_window_t *w = &stock->bench;
    const lcd_cam_cam_ctrl_reg_t clock = {.val = LCD_CAM.cam_ctrl.val};
    const lcd_cam_cam_ctrl1_reg_t ctrl1 = {.val = LCD_CAM.cam_ctrl1.val};
    const lcd_cam_lc_dma_int_ena_reg_t ena = {.val = LCD_CAM.lc_dma_int_ena.val};
    const uint32_t raw = LCD_CAM.lc_dma_int_raw.val, pending = LCD_CAM.lc_dma_int_st.val;
    const uint32_t vsync_mux = GPIO.func_in_sel_cfg[CAM_V_SYNC_IDX].val;
    TaskHandle_t cam_task = xTaskGetHandle("cam_task");
    XIAOTAI_LOGW("camera.dvp", "first_frame=%u open_us=%" PRId64 " size=%zux%zu pins=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                " samples=%" PRIu32 " bursts=%u window_us=%" PRId64 " sampled_us=%" PRIu32 " max_gap_us=%" PRIu32
                " vsync_changes=%" PRIu32 " pclk_changes=%" PRIu32 " href_changes=%" PRIu32 " high=0x%x low=0x%x",
                (unsigned)first_frame, w->opened_us, controller.width, controller.height,
                stock->bench_pins[DVP_BENCH_VSYNC], stock->bench_pins[DVP_BENCH_PCLK], stock->bench_pins[DVP_BENCH_HREF], w->samples,
                (unsigned)w->bursts, w->ended_us - w->began_us, w->sampled_us, w->largest_gap_us,
                w->changes[DVP_BENCH_VSYNC], w->changes[DVP_BENCH_PCLK], w->changes[DVP_BENCH_HREF],
                (unsigned)w->highs, (unsigned)w->lows);
    XIAOTAI_LOGW("camera.dvp", "open_us=%" PRId64 " xclk_pin=%d xclk_config_hz=%" PRIu32 " clock=%u div=%u/%u/%u cam_start=%u"
                " vsync_filter=%u/%u ctrl_inv=%u vsync_mux=0x%" PRIx32 " irq_ena=%u raw=0x%" PRIx32 " pending=0x%" PRIx32
                " cam_task_state=%d frame_queue_ready=%u",
                w->opened_us, stock->bench_xclk_pin, stock->bench_xclk_hz, (unsigned)clock.cam_clk_sel,
                (unsigned)clock.cam_clkm_div_num,
                (unsigned)clock.cam_clkm_div_a, (unsigned)clock.cam_clkm_div_b, (unsigned)ctrl1.cam_start,
                (unsigned)ctrl1.cam_vsync_filter_en, (unsigned)clock.cam_vsync_filter_thres,
                (unsigned)ctrl1.cam_vsync_inv, vsync_mux, (unsigned)ena.cam_vsync_int_ena, raw, pending,
                cam_task ? (int)eTaskGetState(cam_task) : -1, (unsigned)esp_camera_available_frames());
}

static void bench_poll(stock_capture_t *stock, bool first_frame)
{
    if (stock->bench.done) return;
    const int64_t now = xiaotai_time_us();
    switch (dvp_bench_step(&stock->bench, now, first_frame)) {
    case DVP_BENCH_WAIT: return;
    case DVP_BENCH_REPORT: bench_report(stock, first_frame); return;
    case DVP_BENCH_SAMPLE: break;
    }
    const unsigned banks[] = {stock->bench_pins[0] / 32U, stock->bench_pins[1] / 32U, stock->bench_pins[2] / 32U};
    const uint32_t masks[] = {1UL << (stock->bench_pins[0] % 32U), 1UL << (stock->bench_pins[1] % 32U),
                              1UL << (stock->bench_pins[2] % 32U)};
    dvp_bench_burst_start(&stock->bench, now);
    /* Sample in existing stock polls; never hold off interrupts or rewire the pins. */
    for (unsigned n = 0; n < DVP_BENCH_BURST_SAMPLES; ++n) {
        if ((n & 31U) == 0 && n && xiaotai_time_us() - now >= DVP_BENCH_BURST_US) break;
        const uint32_t input[] = {REG_READ(GPIO_IN_REG), REG_READ(GPIO_IN1_REG)};
        const uint8_t levels = (uint8_t)(((input[banks[0]] & masks[0]) != 0) |
            (((input[banks[1]] & masks[1]) != 0) << 1) | (((input[banks[2]] & masks[2]) != 0) << 2));
        dvp_bench_sample(&stock->bench, levels, n == 0);
    }
    dvp_bench_burst_end(&stock->bench, now, xiaotai_time_us());
}
#endif

static void stock_fetch(void *context)
{
    stock_capture_t *stock = context;
    for (;;) {
        (void)xiaotai_signal_take(&stock->request, XIAOTAI_WAIT_FOREVER);
        xiaotai_critical_enter(&stock_lock);
        const bool stopping = stock->stop;
        camera_fb_t *retired = stopping ? stock->frame : NULL;
        if (stopping) stock->frame = NULL;
        xiaotai_critical_exit(&stock_lock);
        if (stopping) {
            if (retired) esp_camera_fb_return(retired);
            xiaotai_critical_enter(&stock_lock);
            stock->finished = true;
            xiaotai_critical_exit(&stock_lock);
            xiaotai_task_exit();
        }

        camera_fb_t *frame = NULL;
        xiaotai_err_t result = XIAOTAI_ERR_NOT_FOUND;
        if (esp_camera_available_frames()) {
            frame = esp_camera_fb_get();
            result = frame ? XIAOTAI_OK : XIAOTAI_FAIL;
        }
        xiaotai_critical_enter(&stock_lock);
        stock->frame = frame;
        stock->result = result;
        stock->state = FETCH_READY;
        xiaotai_critical_exit(&stock_lock);
#ifdef XIAOTAI_DVP_BENCH_DIAG
        bench_poll(stock, frame != NULL);
#endif
    }
}

static xiaotai_err_t stock_start(stock_capture_t *stock)
{
    if (!stock_lock_initialized) {
        xiaotai_critical_init(&stock_lock);
        stock_lock_initialized = true;
    }
    xiaotai_err_t err = xiaotai_signal_init(&stock->request);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_task_config_t config = {.name = "dvp_stock", .entry = stock_fetch, .arg = stock,
        .stack_bytes = STOCK_STACK_BYTES, .stack = XIAOTAI_MEM_INTERNAL, .priority = 5, .core = XIAOTAI_CORE_ANY};
    xiaotai_task_t task;
    err = xiaotai_task_create(&config, &task);
    if (err != XIAOTAI_OK) {
        xiaotai_signal_deinit(&stock->request);
        return err;
    }
    stock->started = true;
    stock->state = FETCH_PENDING;
    xiaotai_signal_give(&stock->request);
    return XIAOTAI_OK;
}

static xiaotai_err_t stock_stop(stock_capture_t *stock)
{
    if (!stock->started) return XIAOTAI_OK;
    xiaotai_critical_enter(&stock_lock);
    stock->stop = true;
    xiaotai_critical_exit(&stock_lock);
    xiaotai_signal_give(&stock->request);

    const int64_t start = xiaotai_time_us();
    for (;;) {
        xiaotai_critical_enter(&stock_lock);
        const bool finished = stock->finished;
        xiaotai_critical_exit(&stock_lock);
        if (finished) break;
        if (xiaotai_time_us() - start >= (int64_t)STOCK_STOP_MS * 1000) return XIAOTAI_ERR_TIMEOUT;
        xiaotai_delay_ms(1);
    }
    stock->started = false;
    xiaotai_signal_deinit(&stock->request);
    return XIAOTAI_OK;
}

static xiaotai_err_t stock_take(stock_capture_t *stock, camera_fb_t **frame)
{
    xiaotai_err_t result = XIAOTAI_ERR_NOT_FOUND;
    xiaotai_critical_enter(&stock_lock);
    if (stock->state == FETCH_READY) {
        *frame = stock->frame;
        stock->frame = NULL;
        result = stock->result;
        stock->state = FETCH_IDLE;
    }
    const bool wake = stock->state == FETCH_IDLE && result == XIAOTAI_ERR_NOT_FOUND;
    if (wake) stock->state = FETCH_PENDING;
    xiaotai_critical_exit(&stock_lock);
    if (wake) xiaotai_signal_give(&stock->request);
    return result;
}
#endif

static int borrowed_port(xiaotai_i2c_bus_t *bus)
{
    i2c_master_bus_handle_t owner = xiaotai_i2c_native(bus);
    if (!owner) return -1;
    for (unsigned port = 0; port < SOC_I2C_NUM; ++port) {
        i2c_master_bus_handle_t installed = NULL;
        if (i2c_master_get_bus_handle((int)port, &installed) == ESP_OK && installed == owner) return (int)port;
    }
    return -1;
}

xiaotai_err_t xt_dvp_io_open(const xiaotai_camera_dvp_config_t *config, xt_dvp_size_t size, xt_dvp_io_t **io)
{
    if (!io) return XIAOTAI_ERR_INVALID_ARG;
    *io = NULL;
    if (!config || !config->sccb || !config->xclk_hz || config->xclk_hz > INT_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (controller.claimed || esp_camera_sensor_get()) return XIAOTAI_ERR_INVALID_STATE;
#ifdef XIAOTAI_DVP_USE_OVERLAY
    if (!xiaotai_dvp_sccb_idle()) return XIAOTAI_ERR_INVALID_STATE;
#endif
    framesize_t native_size;
    switch (size) {
    case XT_DVP_160_120: native_size = FRAMESIZE_QQVGA; break;
    case XT_DVP_240_176: native_size = FRAMESIZE_HQVGA; break;
    case XT_DVP_320_240: native_size = FRAMESIZE_QVGA; break;
    default: return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    const int port = borrowed_port(config->sccb);
    if (port < 0) return XIAOTAI_ERR_NOT_FOUND;

    const camera_config_t native = {
        .pin_pwdn = -1, .pin_reset = -1, /* power and reset belong to the part */
        .pin_xclk = config->xclk_pin, .xclk_freq_hz = (int)config->xclk_hz,
        .ledc_timer = config->xclk_timer, .ledc_channel = config->xclk_channel,
        .pin_sccb_sda = -1, .pin_sccb_scl = -1, .sccb_i2c_port = port,
        .pin_d0 = config->data_pins[0], .pin_d1 = config->data_pins[1],
        .pin_d2 = config->data_pins[2], .pin_d3 = config->data_pins[3],
        .pin_d4 = config->data_pins[4], .pin_d5 = config->data_pins[5],
        .pin_d6 = config->data_pins[6], .pin_d7 = config->data_pins[7],
        .pin_vsync = config->vsync_pin, .pin_href = config->href_pin, .pin_pclk = config->pclk_pin,
        .pixel_format = PIXFORMAT_YUV422, .frame_size = native_size, .jpeg_quality = 12,
        .fb_count = XT_DVP_BUFFERS, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    controller = (xt_dvp_io_t){.claimed = true, .size = native_size};
#ifdef XIAOTAI_DVP_USE_OVERLAY
    controller.sccb_port = port;
#endif
#ifdef XIAOTAI_DVP_BENCH_DIAG
    controller.stock.bench_pins[DVP_BENCH_VSYNC] = (uint32_t)native.pin_vsync;
    controller.stock.bench_pins[DVP_BENCH_PCLK] = (uint32_t)native.pin_pclk;
    controller.stock.bench_pins[DVP_BENCH_HREF] = (uint32_t)native.pin_href;
    controller.stock.bench_xclk_hz = config->xclk_hz;
    controller.stock.bench_xclk_pin = native.pin_xclk;
    for (unsigned i = 0; i < DVP_BENCH_SIGNALS; ++i)
        if (controller.stock.bench_pins[i] >= SOC_GPIO_PIN_COUNT) controller.stock.bench.done = true;
#endif
    const esp_err_t err = esp_camera_init(&native);
    if (err != ESP_OK) {
#ifdef XIAOTAI_DVP_USE_OVERLAY
        /* Pinned init already tears down the camera and may fail to remove
         * one or more SCCB devices without propagating that cleanup result. */
        if (!xiaotai_dvp_sccb_idle()) {
            controller.closing = true;
            *io = &controller;
            return err;
        }
#endif
        controller = (xt_dvp_io_t){0};
        return err;
    }
    controller.camera_open = true;
    *io = &controller;
#ifdef XIAOTAI_DVP_USE_OVERLAY
    if (!xiaotai_dvp_raw_ready()) {
        controller.closing = true;
        return XIAOTAI_ERR_NO_MEM;
    }
#endif
    return XIAOTAI_OK;
}

static bool ready(const xt_dvp_io_t *io)
{
    return io == &controller && io->camera_open && !io->closing;
}

xiaotai_err_t xt_dvp_io_sensor(xt_dvp_io_t *io, uint16_t *sensor_id, bool *external_dma)
{
    if (!sensor_id || !external_dma) return XIAOTAI_ERR_INVALID_ARG;
    if (!ready(io)) return XIAOTAI_ERR_INVALID_STATE;
    const sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) return XIAOTAI_ERR_INVALID_STATE;
    if (sensor->status.framesize != io->size || sensor->pixformat != PIXFORMAT_YUV422) return XIAOTAI_ERR_NOT_SUPPORTED;
    *sensor_id = sensor->id.PID;
    *external_dma = esp_camera_get_psram_mode();
    io->width = resolution[io->size].width;
    io->height = resolution[io->size].height;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_dvp_io_upright(xt_dvp_io_t *io)
{
    if (!ready(io) || !io->width || !io->height || io->upright) return XIAOTAI_ERR_INVALID_STATE;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_hmirror || !sensor->set_vflip) return XIAOTAI_ERR_NOT_SUPPORTED;
    const int hmirror = sensor->set_hmirror(sensor, 0);
    if (hmirror != 0) {
        XIAOTAI_LOGE("camera.dvp", "set_hmirror failed rc=%d", hmirror);
        return XIAOTAI_FAIL;
    }
    const int vflip = sensor->set_vflip(sensor, 0);
    if (vflip != 0) {
        XIAOTAI_LOGE("camera.dvp", "set_vflip failed rc=%d", vflip);
        return XIAOTAI_FAIL;
    }
#ifndef XIAOTAI_DVP_USE_OVERLAY
#ifdef XIAOTAI_DVP_BENCH_DIAG
    io->stock.bench.opened_us = xiaotai_time_us();
#endif
    const xiaotai_err_t err = stock_start(&io->stock);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE("camera.dvp", "frame worker start failed rc=%d", (int)err);
        return err;
    }
#endif
    io->upright = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_dvp_io_close(xt_dvp_io_t *io)
{
    if (io != &controller || !io->claimed || io->loan) return XIAOTAI_ERR_INVALID_STATE;
    io->closing = true;
#ifdef XIAOTAI_DVP_USE_OVERLAY
    /* Run the checkable, retryable device release first. Public deinit
     * suppresses SCCB failures, so it must only run after this succeeds. */
    esp_err_t err = xiaotai_dvp_sccb_release_borrowed(io->sccb_port);
#else
    esp_err_t err = stock_stop(&io->stock);
#endif
    if (err != ESP_OK) return err;
    if (io->camera_open) {
        err = esp_camera_deinit();
        if (err != ESP_OK) return err;
    }
    *io = (xt_dvp_io_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t xt_dvp_io_take(xt_dvp_io_t *io, xt_dvp_frame_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if (!ready(io) || !io->upright || io->loan) return XIAOTAI_ERR_INVALID_STATE;
    uint32_t skipped = 0;
    size_t width = io->width, height = io->height;
    uint32_t format = XIAOTAI_CAMERA_YUV422;
#ifdef XIAOTAI_DVP_USE_OVERLAY
    camera_fb_t *fb = xiaotai_dvp_try_take_raw(&skipped);
    if (!fb) return XIAOTAI_ERR_NOT_FOUND;
#else
    camera_fb_t *fb = NULL;
    const xiaotai_err_t err = stock_take(&io->stock, &fb);
    if (err != XIAOTAI_OK) return err;
    width = fb->width;
    height = fb->height;
    if (fb->format != PIXFORMAT_YUV422) format = 0;
#endif
    io->loan = fb;
    *out = (xt_dvp_frame_t){.data = fb->buf, .size = fb->len, .width = width, .height = height,
        .format = format, .timestamp_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec,
        .skipped = skipped, .token = fb};
    return XIAOTAI_OK;
}

void xt_dvp_io_return(xt_dvp_io_t *io, void *token)
{
    if (ready(io) && token && token == io->loan) {
        esp_camera_fb_return(io->loan);
        io->loan = NULL;
    }
}
