// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_camera_port.py
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#include "camera_dvp_io.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "xiaotai/port/os.h"

struct xiaotai_i2c_bus { int identity; };
static xiaotai_i2c_bus_t bus[2];
static sensor_t sensor;
static bool open, start_fails, task_alive, init_fails, deinit_fails;
static pthread_t owner, worker;
static xiaotai_task_config_t requested_task;
static atomic_bool ready_frame, stall_get, fail_get, jump_clock;
static atomic_uint worker_gets, returns;
static unsigned deinitializations;
static uint8_t pixels[320U * 240U * 2U];
static camera_fb_t frame = {.buf = pixels, .len = sizeof(pixels), .width = 320, .height = 240,
    .format = PIXFORMAT_YUV422, .timestamp = {.tv_sec = 19, .tv_usec = 37}};

const resolution_info_t resolution[] = {
    [FRAMESIZE_QQVGA] = {160, 120}, [FRAMESIZE_HQVGA] = {240, 176}, [FRAMESIZE_QVGA] = {320, 240},
};

static void *run_task(void *arg)
{
    const xiaotai_task_config_t *task = arg;
    task->entry(task->arg);
    assert(false); /* tasks must exit through the port, as required by FreeRTOS */
    return NULL;
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    assert(config && task && config->stack_bytes && !task_alive);
    if (start_fails) return XIAOTAI_ERR_NO_MEM;
    requested_task = *config;
    assert(pthread_create(&worker, NULL, run_task, &requested_task) == 0);
    task_alive = true;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_task_exit(void) { pthread_exit(NULL); }

static void join_worker(void)
{
    assert(task_alive && pthread_join(worker, NULL) == 0);
    task_alive = false;
}

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    static int64_t offset;
    if (atomic_load(&jump_clock)) offset += INT64_C(5000000);
    return __real_xiaotai_time_us() + offset;
}

void *xiaotai_i2c_native(xiaotai_i2c_bus_t *candidate) { return candidate; }

esp_err_t i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out)
{
    assert(port == 0 || port == 1);
    *out = &bus[port];
    return ESP_OK;
}

static int orientation(sensor_t *instance, int setting)
{
    assert(instance == &sensor && !setting);
    return 0;
}

esp_err_t esp_camera_init(const camera_config_t *cfg)
{
    assert(cfg->pin_sccb_sda == -1 && cfg->pin_sccb_scl == -1 && cfg->sccb_i2c_port == 1 && !open);
    assert(cfg->pin_pwdn == -1 && cfg->pin_reset == -1);
    assert(cfg->fb_count == 2 && cfg->fb_location == CAMERA_FB_IN_PSRAM && cfg->grab_mode == CAMERA_GRAB_LATEST);
    assert(cfg->pixel_format == PIXFORMAT_YUV422 && cfg->frame_size == FRAMESIZE_QVGA);
    if (init_fails) return XIAOTAI_ERR_NOT_SUPPORTED;
    sensor = (sensor_t){.id.PID = 0x2145, .status.framesize = cfg->frame_size, .pixformat = cfg->pixel_format,
        .set_hmirror = orientation, .set_vflip = orientation};
    open = true;
    return ESP_OK;
}

esp_err_t esp_camera_deinit(void)
{
    assert(open && pthread_equal(owner, pthread_self()) && !atomic_load(&stall_get));
    ++deinitializations;
    if (deinit_fails) return ESP_FAIL;
    open = false;
    return ESP_OK;
}

sensor_t *esp_camera_sensor_get(void) { return open ? &sensor : NULL; }
bool esp_camera_get_psram_mode(void) { return false; }

bool esp_camera_available_frames(void)
{
    assert(open && !pthread_equal(owner, pthread_self()));
    return atomic_load(&ready_frame);
}

camera_fb_t *esp_camera_fb_get(void)
{
    assert(open && !pthread_equal(owner, pthread_self()) && atomic_exchange(&ready_frame, false));
    atomic_fetch_add(&worker_gets, 1);
    while (atomic_load(&stall_get)) xiaotai_delay_ms(1);
    return atomic_load(&fail_get) ? NULL : &frame;
}

void esp_camera_fb_return(camera_fb_t *returned)
{
    assert(open && returned == &frame);
    atomic_fetch_add(&returns, 1);
}

static xt_dvp_io_t *open_io(void)
{
    const xiaotai_camera_dvp_config_t config = {.sccb = &bus[1], .xclk_hz = 20000000};
    xt_dvp_io_t *io = NULL;
    if (init_fails) {
        assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_ERR_NOT_SUPPORTED && !io);
        return NULL;
    }
    assert(xt_dvp_io_open(&config, XT_DVP_320_240, &io) == XIAOTAI_OK && io);
    uint16_t pid;
    bool external;
    assert(xt_dvp_io_sensor(io, &pid, &external) == XIAOTAI_OK && pid == 0x2145 && !external);
    return io;
}

static void wait_for_get(xt_dvp_io_t *io, unsigned count)
{
    xt_dvp_frame_t out;
    for (unsigned i = 0; i < 1000 && atomic_load(&worker_gets) < count; ++i) {
        assert(xt_dvp_io_take(io, &out) == XIAOTAI_ERR_NOT_FOUND);
        xiaotai_delay_ms(1);
    }
    assert(atomic_load(&worker_gets) == count);
}

static xiaotai_err_t wait_for_result(xt_dvp_io_t *io, xt_dvp_frame_t *out)
{
    for (unsigned i = 0; i < 1000; ++i) {
        const xiaotai_err_t err = xt_dvp_io_take(io, out);
        if (err != XIAOTAI_ERR_NOT_FOUND) return err;
        xiaotai_delay_ms(1);
    }
    return XIAOTAI_ERR_TIMEOUT;
}

int main(void)
{
    owner = pthread_self();
    init_fails = true;
    assert(open_io() == NULL && !task_alive && !deinitializations);
    init_fails = false;

    xt_dvp_io_t *io = open_io();
    start_fails = true;
    assert(xt_dvp_io_upright(io) == XIAOTAI_ERR_NO_MEM && !task_alive);
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && deinitializations == 1);
    start_fails = false;

    io = open_io();
    assert(xt_dvp_io_upright(io) == XIAOTAI_OK);
    xt_dvp_frame_t out;
    assert(xt_dvp_io_take(io, &out) == XIAOTAI_ERR_NOT_FOUND);
    assert(!atomic_load(&worker_gets));
    atomic_store(&stall_get, true);
    atomic_store(&ready_frame, true);
    wait_for_get(io, 1);
    /* An availability hit may still block in the driver. The owner can poll. */
    for (unsigned i = 0; i < 20; ++i) assert(xt_dvp_io_take(io, &out) == XIAOTAI_ERR_NOT_FOUND);
    assert(atomic_load(&worker_gets) == 1);
    atomic_store(&stall_get, false);
    assert(wait_for_result(io, &out) == XIAOTAI_OK);
    assert(out.data == pixels && out.size == sizeof(pixels) && out.width == 320 && out.height == 240);
    assert(out.token == &frame && out.timestamp_us == 19000037 && out.skipped == 0 && out.format == XIAOTAI_CAMERA_YUV422);
    assert(xt_dvp_io_take(io, &(xt_dvp_frame_t){0}) == XIAOTAI_ERR_INVALID_STATE);
    assert(xt_dvp_io_close(io) == XIAOTAI_ERR_INVALID_STATE && deinitializations == 1);
    xt_dvp_io_return(io, pixels);
    assert(!atomic_load(&returns));
    xt_dvp_io_return(io, out.token);
    assert(atomic_load(&returns) == 1);

    atomic_store(&stall_get, true);
    atomic_store(&ready_frame, true);
    wait_for_get(io, 2);
    atomic_store(&jump_clock, true);
    assert(xt_dvp_io_close(io) == XIAOTAI_ERR_TIMEOUT);
    atomic_store(&jump_clock, false);
    assert(open && deinitializations == 1 && xt_dvp_io_take(io, &out) == XIAOTAI_ERR_INVALID_STATE);
    atomic_store(&stall_get, false);
    deinit_fails = true;
    assert(xt_dvp_io_close(io) == ESP_FAIL && deinitializations == 2);
    join_worker();
    assert(atomic_load(&returns) == 2); /* an in-flight unloaned frame returned before deinit */
    deinit_fails = false;
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && deinitializations == 3);

    io = open_io();
    atomic_store(&fail_get, true);
    atomic_store(&ready_frame, true);
    assert(xt_dvp_io_upright(io) == XIAOTAI_OK);
    assert(wait_for_result(io, &out) == XIAOTAI_FAIL); /* no false success for a failed driver read */
    assert(atomic_load(&worker_gets) == 3 && atomic_load(&returns) == 2);
    assert(xt_dvp_io_close(io) == XIAOTAI_OK && deinitializations == 4);
    join_worker();
    return 0;
}
