// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: a camera sensor on the parallel (DVP) interface, as a camera
 * part.
 *
 * The part delivers packed YUV 4:2:2 (XIAOTAI_CAMERA_YUV422) in the modes
 * listed below, keeps two frame buffers in XIAOTAI_MEM_BULK while open and
 * always hands out the newest frame. The sensor writes into internal DMA
 * memory and frames are copied out: receiving straight into external RAM
 * corrupts frames under audio and Wi-Fi load, so the implementation refuses to
 * build with that mode enabled.
 *
 * prepare() is NULL: the part holds no memory and touches no hardware before
 * the first set_power(true), so a product that never streams pays nothing.
 *
 * Power sequence of set_power(true): reset asserted, power on, wait
 * power_settle_ms, reset released, wait reset_settle_ms. set_power(false):
 * power off; boards that hold reset while unpowered assert it before cutting
 * the rails. Lines that are not connected are skipped, their waits kept.
 *
 * Calling tasks: create is [board]; the part follows the camera contract of
 * xiaotai/hal.h (one worker task owns all calls; borrow never blocks). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sizes open() accepts: 160x120, 240x176 and 320x240, each only up to the
 * configured maximum; any other size is XIAOTAI_ERR_NOT_SUPPORTED. */

typedef struct {
    xiaotai_i2c_bus_t *sccb;    /* the sensor's control bus, shared with the board */
    int xclk_pin;               /* -1: the board clocks the sensor (crystal) */
    uint32_t xclk_hz;
    /* PWM timer and channel that generate XCLK (as in xiaotai_pwm_config_t):
     * the board picks ones no other part of it uses, e.g. a PWM backlight. */
    uint8_t xclk_timer, xclk_channel;
    int data_pins[8];           /* D0..D7 */
    int vsync_pin, href_pin, pclk_pin;
    /* Sensor identity the board declares. A different sensor answers open()
     * with XIAOTAI_ERR_NOT_SUPPORTED (the part then still needs close()). */
    uint16_t sensor_id;
    uint16_t max_width, max_height;
    uint8_t max_fps;            /* 0: unknown */
    uint8_t rotation;           /* quarter turns clockwise that make the image upright */
    xiaotai_line_t power;       /* active: powered; zeroed: always powered */
    xiaotai_line_t reset;       /* active: in reset; zeroed: not connected */
    uint16_t power_settle_ms;
    uint16_t reset_settle_ms;
    bool hold_reset_when_off;
} xiaotai_camera_dvp_config_t;

/* [board] Creates the camera part; touches no hardware except driving the
 * reset line to "in reset" and power to off. The object is allocated once and
 * never freed. Descriptor: formats YUV422, min 160x120, max as configured,
 * row_align 0, mirror and flip off. */
xiaotai_err_t xiaotai_camera_dvp_create(const xiaotai_camera_dvp_config_t *config, const xiaotai_camera_t **camera);

#ifdef __cplusplus
}
#endif
