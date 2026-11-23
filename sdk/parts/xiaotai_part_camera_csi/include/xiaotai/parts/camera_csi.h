// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: a camera sensor on MIPI CSI behind the chip's image signal
 * processor, as a camera part.
 *
 * The part delivers XIAOTAI_CAMERA_YUV420_PAIRED at the sensor's native size.
 * Frame memory is a pool of `buffers` frames of width x padded height x 1.5
 * bytes in XIAOTAI_MEM_BULK, aligned to 64 bytes, and kept for the life of the
 * system: sessions reuse it, and a board without external RAM cannot carry
 * this part.
 *
 * prepare() is implemented: it allocates the pool, powers the sensor, starts
 * the platform's video subsystem once (with its DMA channel guard) and checks
 * the sensor id, without streaming; then it powers the sensor down again
 * unless keep_powered. The video engine calls it while it starts, so that the
 * pool (several MiB of contiguous memory) is taken before the network is up.
 * When prepare() was not called or failed, the first set_power(true) does the
 * same steps. XIAOTAI_ERR_NO_MEM: the pool could not be allocated;
 * XIAOTAI_ERR_NOT_SUPPORTED: the sensor id did not match.
 *
 * The ESP-IDF options the part needs (CSI driver buffering, ISP pipeline
 * controller) are in the component's sdkconfig.defaults; the sensor driver,
 * its mode and tuning file are board facts in the board's sdkconfig.board.
 *
 * borrow() never blocks; it drains the capture queue, returns the newest
 * frame and counts the older ones it gave back in frame.skipped. A frame
 * shorter than the visible picture or longer than a buffer is refused with
 * XIAOTAI_ERR_INVALID_SIZE.
 *
 * Calling tasks: create is [board]; the part follows the camera contract of
 * xiaotai/hal.h (one worker task owns all calls). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A change to a sensor register with a 16-bit address: the bits in mask are
 * replaced with those of value (mask 0xff writes the whole register). */
typedef struct {
    uint16_t reg;
    uint8_t mask;
    uint8_t value;
} xiaotai_sensor_reg_t;

typedef struct {
    xiaotai_i2c_bus_t *sccb;    /* the sensor's control bus */
    uint8_t sensor_address;     /* 7-bit */
    /* Product id read from the sensor after start and compared; 0: not
     * checked. A mismatch fails open() with XIAOTAI_ERR_NOT_SUPPORTED. */
    uint16_t sensor_id;
    uint16_t sensor_id_reg;     /* address of the high byte of the id */
    uint16_t width, height;     /* native visible size: the only size open() accepts */
    uint8_t fps;                /* the sensor's frame rate; 0: unknown */
    /* true: the sensor runs at its own rate and open() never asks for one;
     * false: open() asks for mode.fps and only counts a refusal. */
    bool fixed_fps;
    uint8_t row_align;          /* frames are padded to a multiple of this many rows; 0: none */
    bool full_range;            /* ask the processor for full-range BT.601 */
    uint8_t rotation;           /* quarter turns clockwise that make the image upright */
    uint8_t buffers;            /* capture buffers; 0: 3 (capture, encode and requeue overlap) */
    xiaotai_line_t power;       /* active: powered; zeroed: always powered */
    uint16_t power_settle_ms;
    /* Once the capture pipeline has started, set_power(false) leaves the
     * sensor powered: the platform's video subsystem cannot be started twice
     * and loses the sensor when it is powered down. */
    bool keep_powered;
    /* Register changes applied after the stream format is set, each read back
     * and compared under its mask; a mismatch fails open(). NULL: none. Static
     * storage. */
    const xiaotai_sensor_reg_t *setup;
    size_t setup_count;
    /* Sensor device clock on the shared SCCB controller (not the bus's
     * default for other I2C devices). 0: 100 kHz; maximum 400 kHz. */
    uint32_t sccb_hz;
} xiaotai_camera_csi_config_t;

/* [board] Creates the camera part; touches no hardware except driving power
 * off. The object is allocated once and never freed. Descriptor: formats
 * YUV420_PAIRED, min and max the native size, row_align, rotation and
 * FULL_RANGE as configured. */
xiaotai_err_t xiaotai_camera_csi_create(const xiaotai_camera_csi_config_t *config, const xiaotai_camera_t **camera);

typedef struct {
    uint32_t opens, open_failures;
    uint32_t frames;            /* frames lent */
    uint32_t stale_frames;      /* older frames returned unread */
    uint32_t bad_frames;        /* refused: error flag or impossible length */
    uint32_t rate_refused;      /* the sensor refused the requested frame rate */
    size_t pool_bytes;          /* frame pool held */
} xiaotai_camera_csi_stats_t;

/* [any] Never blocks. */
void xiaotai_camera_csi_stats(const xiaotai_camera_t *camera, xiaotai_camera_csi_stats_t *stats);

/* [any] Diagnostics for the board's Report(): the counters above and, while
 * the sensor is powered, the exposure and gain it reports. Reads only; blocks
 * for bus transfers, so never from a real-time task. */
void xiaotai_camera_csi_report(const xiaotai_camera_t *camera, xiaotai_hal_write_fn_t write, void *ctx);

#ifdef __cplusplus
}
#endif
