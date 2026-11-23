// SPDX-License-Identifier: MIT
#include <assert.h>

#include "camera_dvp_fake.h"
#include "xiaotai/port/os.h"

static int64_t clock_us, off_done, on_start, on_done;
static unsigned on_writes, off_writes, waits;
static bool fail_on, fail_off, early_wake;

int64_t __wrap_xiaotai_time_us(void) { return clock_us; }

void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    assert(ms && ms <= 100U);
    ++waits;
    clock_us += (int64_t)ms * 1000;
    if (early_wake) --clock_us;
    early_wake = false;
}

static xiaotai_err_t power_line(void *ctx, bool active)
{
    (void)ctx;
    if (active) {
        ++on_writes;
        on_start = clock_us;
        clock_us += 2000;
        if (fail_on) return XIAOTAI_FAIL;
        on_done = clock_us;
    } else {
        ++off_writes;
        clock_us += 3000;
        if (fail_off) return XIAOTAI_FAIL;
        off_done = clock_us;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t power(const xiaotai_camera_t *camera, bool enabled)
{
    return camera->set_power(camera->self, enabled);
}

int main(void)
{
    xiaotai_i2c_bus_t *bus;
    assert(xiaotai_i2c_create(&(xiaotai_i2c_config_t){.port=0,.sda_pin=1,.scl_pin=2,.hz=100000}, &bus) == XIAOTAI_OK);
    xiaotai_camera_dvp_config_t wiring = {.sccb = bus, .xclk_pin = 5, .xclk_hz = 20000000,
        .data_pins = {16,18,8,17,15,6,4,9}, .vsync_pin = 3, .pclk_pin = 7, .href_pin = 46,
        .sensor_id = 0x2145, .max_width = 320, .max_height = 240, .power = {.set = power_line}, .power_settle_ms = 10};
    const xiaotai_camera_t *camera;
    const xiaotai_camera_mode_t mode = {320, 240, XIAOTAI_CAMERA_YUV422, 1};

    fail_off = true;
    assert(xiaotai_camera_dvp_create(&wiring, &camera) == XIAOTAI_FAIL && !camera);
    fail_off = false;
    assert(xiaotai_camera_dvp_create(&wiring, &camera) == XIAOTAI_OK);
    clock_us += 42500;
    assert(power(camera, false) == XIAOTAI_OK && off_writes == 2);
    early_wake = true;
    assert(power(camera, true) == XIAOTAI_OK && on_writes == 1);
    assert(on_start - off_done >= 100000 && on_start - off_done < 101000);
    assert(clock_us - on_done == 50000);
    const int64_t on_again = clock_us;
    assert(power(camera, true) == XIAOTAI_OK && on_writes == 1 && clock_us == on_again);

    xt_dvp_fake_script(&(xt_dvp_fake_script_t){.partial_open = true, .open_error = XIAOTAI_FAIL});
    assert(camera->open(camera->self, &mode) == XIAOTAI_FAIL);
    assert(power(camera, false) == XIAOTAI_ERR_INVALID_STATE && off_writes == 2);
    xt_dvp_fake_script(&(xt_dvp_fake_script_t){.close_error = XIAOTAI_ERR_TIMEOUT});
    assert(camera->close(camera->self) == XIAOTAI_ERR_TIMEOUT);
    assert(power(camera, false) == XIAOTAI_ERR_INVALID_STATE && off_writes == 2);
    xt_dvp_fake_script(NULL);
    assert(camera->close(camera->self) == XIAOTAI_OK);

    fail_off = true;
    assert(power(camera, false) == XIAOTAI_FAIL);
    const unsigned before_retry_waits = waits;
    assert(power(camera, true) == XIAOTAI_ERR_INVALID_STATE && waits == before_retry_waits && on_writes == 1);
    fail_off = false;
    assert(power(camera, false) == XIAOTAI_OK);
    fail_on = true;
    assert(power(camera, true) == XIAOTAI_FAIL && on_writes == 2);
    assert(clock_us == off_done); /* immediate rollback, no on-settle on a failed release */
    fail_on = false;
    assert(power(camera, true) == XIAOTAI_OK && on_writes == 3);
    assert(on_start - off_done == 100000 && clock_us - on_done == 50000);
    assert(camera->open(camera->self, &mode) == XIAOTAI_OK);
    assert(camera->close(camera->self) == XIAOTAI_OK && power(camera, false) == XIAOTAI_OK);

    wiring.power = (xiaotai_line_t){0};
    assert(xiaotai_camera_dvp_create(&wiring, &camera) == XIAOTAI_OK);
    const int64_t unwired = clock_us;
    assert(power(camera, true) == XIAOTAI_OK && clock_us - unwired == 10000);
    assert(power(camera, false) == XIAOTAI_OK);
    return 0;
}
