// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_camera_port.py
// Replaces legacy: tests/test_board_rollback.py
#include "camera_dvp_fake.h"
#include "xiaotai_mem_host.h"
#include "xiaotai/port/mem.h"
#include <assert.h>
#include <string.h>

enum { RESET_ON = 1, RESET_OFF, POWER_ON, POWER_OFF, WAIT = 100 };
static xiaotai_i2c_bus_t *bus;
static const xiaotai_camera_t *camera, *external_clock;
static unsigned trace[24];
static size_t trace_count;
static bool fail_reset_on, fail_reset_release, fail_power_on, fail_power_off;
static size_t allocations[8], alloc_count;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    assert(kind == XIAOTAI_MEM_BULK);
    assert(alloc_count < sizeof(allocations) / sizeof(*allocations));
    allocations[alloc_count++] = size;
    return __real_xiaotai_mem_alloc(kind, size);
}

static void add(unsigned value)
{
    assert(trace_count < sizeof(trace) / sizeof(*trace));
    trace[trace_count++] = value;
}

void __wrap_xiaotai_delay_ms(uint32_t ms) { add(WAIT + ms); }

static xiaotai_err_t reset_line(void *self, bool active)
{
    (void)self;
    add(active ? RESET_ON : RESET_OFF);
    return (active ? fail_reset_on : fail_reset_release) ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

static xiaotai_err_t power_line(void *self, bool active)
{
    (void)self;
    add(active ? POWER_ON : POWER_OFF);
    return (active ? fail_power_on : fail_power_off) ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static void check_trace(const unsigned *steps, size_t count)
{
    assert(trace_count == count && !memcmp(trace, steps, sizeof(*steps) * count));
    trace_count = 0;
}

#define STEPS(...) do { const unsigned steps[] = {__VA_ARGS__}; check_trace(steps, sizeof(steps) / sizeof(*steps)); } while (0)

static xiaotai_camera_dvp_config_t wiring(void)
{
    return (xiaotai_camera_dvp_config_t){.sccb = bus, .xclk_pin = 5, .xclk_hz = 20000000,
        .xclk_timer = 2, .xclk_channel = 5, .data_pins = {16, 18, 8, 17, 15, 6, 4, 9},
        .vsync_pin = 3, .href_pin = 46, .pclk_pin = 7, .sensor_id = 0x2145, .max_width = 320,
        .max_height = 240, .max_fps = 20, .rotation = 1,
        .power = {.set = power_line}, .reset = {.set = reset_line}, .power_settle_ms = 20, .reset_settle_ms = 10};
}

static xiaotai_err_t power(const xiaotai_camera_t *part, bool on) { return part->set_power(part->self, on); }

static xiaotai_err_t open(const xiaotai_camera_t *part, uint16_t w, uint16_t h, uint32_t format)
{
    return part->open(part->self, &(xiaotai_camera_mode_t){w, h, format, 255}); /* Rate is a hint. */
}

static xiaotai_err_t close(const xiaotai_camera_t *part) { return part->close(part->self); }

static void test_power(void)
{
    xiaotai_camera_dvp_config_t c = wiring();
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_camera_dvp_create(&c, &camera) == XIAOTAI_ERR_NO_MEM && !camera && !trace_count);
    fail_reset_on = true;
    assert(xiaotai_camera_dvp_create(&c, &camera) == XIAOTAI_ERR_TIMEOUT && !camera);
    STEPS(RESET_ON, POWER_OFF);
    assert(xiaotai_mem_host_outstanding() == 0);
    fail_reset_on = false;
    assert(xiaotai_camera_dvp_create(&c, &camera) == XIAOTAI_OK && camera);
    STEPS(RESET_ON, POWER_OFF);
    assert(camera->desc.formats == XIAOTAI_CAMERA_YUV422 && !camera->prepare && camera->desc.row_align == 0);
    assert(camera->desc.min_width == 160 && camera->desc.min_height == 120 && camera->desc.rotation == 1 &&
           camera->desc.max_width == 320 && camera->desc.max_height == 240 && camera->desc.max_fps == 20);
    assert(xt_dvp_fake_status().opens == 0 && alloc_count == 0);
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_INVALID_STATE);
    fail_power_on = true;
    assert(power(camera, true) == XIAOTAI_FAIL);
    STEPS(RESET_ON, POWER_ON, POWER_OFF);
    fail_power_on = false;
    fail_reset_release = fail_power_off = true;
    assert(power(camera, true) == XIAOTAI_ERR_TIMEOUT);
    STEPS(RESET_ON, POWER_ON, WAIT + 20, RESET_OFF, POWER_OFF);
    assert(power(camera, true) == XIAOTAI_ERR_INVALID_STATE && !trace_count);
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_INVALID_STATE);
    fail_reset_release = fail_power_off = false;
    assert(power(camera, false) == XIAOTAI_OK);
    STEPS(POWER_OFF);
    assert(power(camera, true) == XIAOTAI_OK && power(camera, true) == XIAOTAI_OK);
    STEPS(RESET_ON, POWER_ON, WAIT + 20, RESET_OFF, WAIT + 10);
    assert(xt_dvp_fake_status().opens == 0 && alloc_count == 0);
    assert(power(camera, false) == XIAOTAI_OK && power(camera, false) == XIAOTAI_OK);
    STEPS(POWER_OFF);
}

static void test_modes(void)
{
    const unsigned shape[][2] = {{160,120},{240,176},{320,240}};
    assert(power(camera, true) == XIAOTAI_OK);
    assert(open(camera, 160, 121, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(open(camera, 640, 480, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_JPEG) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422 | XIAOTAI_CAMERA_JPEG) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xt_dvp_fake_status().opens == 0);
    for (unsigned i = 0; i < 3; ++i) {
        alloc_count = 0;
        const int64_t free_before = xiaotai_mem_host_outstanding();
        assert(open(camera, (uint16_t)shape[i][0], (uint16_t)shape[i][1], XIAOTAI_CAMERA_YUV422) == XIAOTAI_OK);
        assert(alloc_count == 2 && allocations[0] == shape[i][0] * shape[i][1] * 2U && allocations[0] == allocations[1]);
        assert(xiaotai_mem_host_outstanding() == free_before + 2);
        xt_dvp_fake_status_t s = xt_dvp_fake_status();
        assert(s.size == (xt_dvp_size_t)i && s.config.sccb == bus && s.config.xclk_pin == 5 &&
               s.config.xclk_hz == 20000000 && s.config.xclk_timer == 2 && s.config.xclk_channel == 5);
        assert(s.config.data_pins[0] == 16 && s.config.data_pins[7] == 9 && s.mirror == i + 1 && s.flips == i + 1);
        assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_INVALID_STATE);
        assert(power(camera, false) == XIAOTAI_ERR_INVALID_STATE);
        assert(close(camera) == XIAOTAI_OK && close(camera) == XIAOTAI_OK);
        assert(xiaotai_mem_host_outstanding() == free_before);
    }
    assert(power(camera, false) == XIAOTAI_OK);
    STEPS(RESET_ON, POWER_ON, WAIT + 20, RESET_OFF, WAIT + 10, POWER_OFF);

    xiaotai_camera_dvp_config_t c = wiring();
    c.xclk_pin = -1;
    c.xclk_timer = 1;
    c.xclk_channel = 4;
    c.max_width = 240;
    c.max_height = 176;
    c.power = c.reset = (xiaotai_line_t){0};
    assert(xiaotai_camera_dvp_create(&c, &external_clock) == XIAOTAI_OK && !trace_count);
    assert(power(external_clock, true) == XIAOTAI_OK);
    STEPS(WAIT + 20, WAIT + 10); /* Optional wires do not remove waits. */
    assert(open(external_clock, 320, 240, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_NOT_SUPPORTED);
    alloc_count = 0;
    assert(open(external_clock, 240, 176, XIAOTAI_CAMERA_YUV422) == XIAOTAI_OK);
    const xt_dvp_fake_status_t ext = xt_dvp_fake_status();
    assert(ext.config.xclk_pin == -1 && ext.config.xclk_timer == 1 && ext.config.xclk_channel == 4);
    assert(close(external_clock) == XIAOTAI_OK && power(external_clock, false) == XIAOTAI_OK && !trace_count);
}

static void test_frames(void)
{
    const size_t bytes = 160U * 120U * 2U;
    assert(power(camera, true) == XIAOTAI_OK);
    alloc_count = 0;
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_OK);
    xiaotai_camera_frame_t frame = {0}, second;
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_ERR_NOT_FOUND);
    assert(camera->borrow(camera->self, NULL) == XIAOTAI_ERR_INVALID_ARG);
    xt_dvp_fake_frame(1, bytes, 160, 120, XIAOTAI_CAMERA_YUV422, 4000123);
    xt_dvp_fake_frame(2, bytes, 160, 120, XIAOTAI_CAMERA_YUV422, 4004567);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_OK);
    assert(frame.data[0] == 2 && frame.skipped == 1 && frame.sequence == 0 && frame.captured_us == 4004567 &&
           frame.width == 160 && frame.rows == 120 && frame.height == 120 && frame.size == bytes);
    second = frame;
    assert(camera->release(camera->self, &second) == XIAOTAI_ERR_INVALID_STATE);
    assert(camera->borrow(camera->self, &second) == XIAOTAI_ERR_INVALID_STATE);
    assert(close(camera) == XIAOTAI_ERR_INVALID_STATE && power(camera, false) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_mem_host_clear();
    xt_dvp_fake_frame(3, bytes, 160, 120, XIAOTAI_CAMERA_YUV422, 4004568);
    xt_dvp_fake_frame(4, bytes, 160, 120, XIAOTAI_CAMERA_YUV422, 4004569);
    assert(frame.data[0] == 2 && xiaotai_mem_host_allocations() == 0);
    assert(camera->release(camera->self, &frame) == XIAOTAI_OK && !frame.data && !frame.size);
    assert(camera->release(camera->self, &frame) == XIAOTAI_ERR_INVALID_STATE);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_OK && frame.data[0] == 4 && frame.skipped == 1);
    assert(camera->release(camera->self, &frame) == XIAOTAI_OK);
    const unsigned before = xt_dvp_fake_status().returns;
    xt_dvp_fake_frame(0, bytes - 1U, 160, 120, XIAOTAI_CAMERA_YUV422, 3);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_ERR_INVALID_SIZE);
    xt_dvp_fake_frame(0, 64, 160, 120, XIAOTAI_CAMERA_YUV422, 3);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_ERR_INVALID_SIZE);
    xt_dvp_fake_frame(0, bytes, 161, 120, XIAOTAI_CAMERA_YUV422, 4);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_ERR_INVALID_SIZE);
    xt_dvp_fake_frame(0, bytes, 160, 120, XIAOTAI_CAMERA_RGB565, 5);
    assert(camera->borrow(camera->self, &frame) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(!frame.data && xt_dvp_fake_status().returns == before + 4U);
    assert(close(camera) == XIAOTAI_OK && power(camera, false) == XIAOTAI_OK);
    STEPS(RESET_ON, POWER_ON, WAIT + 20, RESET_OFF, WAIT + 10, POWER_OFF);
}

static void test_cleanup(void)
{
    assert(power(camera, true) == XIAOTAI_OK);
    const int64_t before = xiaotai_mem_host_outstanding();
    alloc_count = 0;
    xiaotai_mem_host_fail_nth(2);
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_NO_MEM && alloc_count == 2);
    assert(xiaotai_mem_host_outstanding() == before);
    const unsigned closes = xt_dvp_fake_status().closes;
    assert(close(camera) == XIAOTAI_OK && xt_dvp_fake_status().closes == closes);
    const xt_dvp_fake_script_t modes[] = {{.sensor_id = 0x9b}, {.sensor_missing = true}, {.external_dma = true},
        {.fail_mirror = true}, {.fail_flip = true}, {.partial_open = true, .open_error = XIAOTAI_FAIL}};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        alloc_count = 0;
        xt_dvp_fake_script(&modes[i]);
        const xiaotai_err_t error = i < 3 ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_FAIL;
        assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == error);
        assert(xiaotai_mem_host_outstanding() == before + 2);
        assert(power(camera, false) == XIAOTAI_ERR_INVALID_STATE);
        assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_ERR_INVALID_STATE);
        assert(close(camera) == XIAOTAI_OK && xiaotai_mem_host_outstanding() == before);
    }
    xt_dvp_fake_script(&(xt_dvp_fake_script_t){.open_error = XIAOTAI_FAIL});
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_FAIL);
    assert(xiaotai_mem_host_outstanding() == before);
    xt_dvp_fake_script(NULL);
    alloc_count = 0;
    assert(open(camera, 160, 120, XIAOTAI_CAMERA_YUV422) == XIAOTAI_OK);
    xt_dvp_fake_script(&(xt_dvp_fake_script_t){.close_error = XIAOTAI_ERR_TIMEOUT});
    assert(close(camera) == XIAOTAI_ERR_TIMEOUT);
    assert(camera->borrow(camera->self, &(xiaotai_camera_frame_t){0}) == XIAOTAI_ERR_INVALID_STATE);
    assert(power(camera, false) == XIAOTAI_ERR_INVALID_STATE && xiaotai_mem_host_outstanding() == before + 2);
    xt_dvp_fake_script(NULL);
    assert(close(camera) == XIAOTAI_OK && xiaotai_mem_host_outstanding() == before);
    assert(power(camera, false) == XIAOTAI_OK);
    STEPS(RESET_ON, POWER_ON, WAIT + 20, RESET_OFF, WAIT + 10, POWER_OFF);
}

int main(void)
{
    assert(xiaotai_i2c_create(&(xiaotai_i2c_config_t){.port=0,.sda_pin=1,.scl_pin=2,.hz=400000}, &bus) == XIAOTAI_OK);
    test_power();
    test_modes();
    test_frames();
    test_cleanup();
    return 0;
}
