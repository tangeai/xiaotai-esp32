// SPDX-License-Identifier: MIT
#include "led_io.h"
#include "xiaotai/port/mem.h"
#include <stdatomic.h>

typedef struct {
    xiaotai_output_t part;
    xiaotai_led_io_t *io;
    _Atomic uint32_t failures;
    xiaotai_led_order_t order;
    uint8_t brightness;
} led_strip_t;

static uint8_t scale(const led_strip_t *strip, uint8_t value)
{
    return (uint8_t)(((uint32_t)value * strip->brightness + 127U) / 255U);
}

static void pixel(const led_strip_t *strip, uint8_t *wire, uint8_t r, uint8_t g, uint8_t b)
{
    const bool grb = strip->order == XIAOTAI_LED_ORDER_GRB;
    wire[0] = scale(strip, grb ? g : r);
    wire[1] = scale(strip, grb ? r : g);
    wire[2] = scale(strip, b);
}

static xiaotai_err_t send(led_strip_t *strip, const uint8_t *wire)
{
    const xiaotai_err_t err = xiaotai_led_io_send(strip->io, wire, 3U * strip->part.desc.pixels);
    if (err != XIAOTAI_OK) atomic_fetch_add(&strip->failures, 1U);
    return err;
}

static xiaotai_err_t set_all(void *self, uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_t *strip = self;
    uint8_t wire[XT_LED_BYTES_MAX];
    for (size_t i = 0; i < strip->part.desc.pixels; ++i) pixel(strip, &wire[3U * i], r, g, b);
    return send(strip, wire);
}

static xiaotai_err_t write_pixels(void *self, const uint8_t *rgb, size_t count)
{
    led_strip_t *strip = self;
    if (!rgb) return XIAOTAI_ERR_INVALID_ARG;
    if (count != strip->part.desc.pixels) return XIAOTAI_ERR_INVALID_SIZE;
    uint8_t wire[XT_LED_BYTES_MAX];
    for (size_t i = 0; i < count; ++i) pixel(strip, &wire[3U * i], rgb[3U * i], rgb[3U * i + 1U], rgb[3U * i + 2U]);
    return send(strip, wire);
}

xiaotai_err_t xiaotai_led_strip_create(const xiaotai_led_strip_config_t *config, const xiaotai_output_t **output)
{
    if (output) *output = NULL;
    if (!config || !output || !config->role || !*config->role || config->pin < 0 ||
        !config->pixels || config->pixels > XIAOTAI_LED_STRIP_PIXELS_MAX ||
        (config->order != XIAOTAI_LED_ORDER_GRB && config->order != XIAOTAI_LED_ORDER_RGB))
        return XIAOTAI_ERR_INVALID_ARG;
    led_strip_t *strip = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*strip));
    if (!strip) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xiaotai_led_io_create(config->pin, &strip->io);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(strip);
        return err;
    }
    strip->order = config->order;
    strip->brightness = config->brightness ? config->brightness : UINT8_MAX;
    strip->part = (xiaotai_output_t){
        .desc = {.kind = XIAOTAI_OUTPUT_RGB_LIGHT, .role = config->role, .pixels = config->pixels},
        .set = set_all, .write = write_pixels, .self = strip};
    *output = &strip->part;
    return XIAOTAI_OK;
}

uint32_t xiaotai_led_strip_failures(const xiaotai_output_t *output)
{
    if (!output || output->write != write_pixels) return 0;
    const led_strip_t *strip = output->self;
    return atomic_load(&strip->failures);
}
