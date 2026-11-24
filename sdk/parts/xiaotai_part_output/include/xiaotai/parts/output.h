// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: output parts - an addressable RGB LED strip, and single
 * on/off outputs (a light, a vibrator, a relay) on a line.
 *
 * Patterns, timing and what a state looks like belong to the feedback engine
 * and the application's table; these parts only show what they are given. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_LED_STRIP_PIXELS_MAX 16U

typedef enum {
    XIAOTAI_LED_ORDER_GRB = 0,  /* WS2812 class */
    XIAOTAI_LED_ORDER_RGB,
} xiaotai_led_order_t;

typedef struct {
    int pin;                    /* data pin */
    uint16_t pixels;            /* 1..XIAOTAI_LED_STRIP_PIXELS_MAX */
    xiaotai_led_order_t order;  /* colour order on the wire */
    /* Every channel is scaled by brightness / 255 (rounded) before it goes out:
     * the supply limit of the board. 0: 255. */
    uint8_t brightness;
    const char *role;           /* static string: "status" */
} xiaotai_led_strip_config_t;

/* [board] Output part of kind RGB_LIGHT with desc.pixels elements and write().
 * One frame goes out per write()/set(); the call waits up to 10 ms for the
 * frame to leave and counts a failed send instead of logging it. */
xiaotai_err_t xiaotai_led_strip_create(const xiaotai_led_strip_config_t *config, const xiaotai_output_t **output);

/* [any] Failed writes to the strip's transmitter since creation. Invalid
 * write arguments and non-strip parts are not counted. */
uint32_t xiaotai_led_strip_failures(const xiaotai_output_t *output);

typedef struct {
    xiaotai_output_kind_t kind; /* LIGHT, VIBRATOR or RELAY */
    const char *role;           /* static string: "status"; a door lock relay "door" or "door.<name>" */
    xiaotai_line_t line;        /* active: lit, vibrating, relay closed */
    bool initially_active;      /* state set at creation; a door relay starts inactive */
} xiaotai_output_line_config_t;

/* [board] Output part with one element; set() turns it on when red != 0. */
xiaotai_err_t xiaotai_output_line_create(const xiaotai_output_line_config_t *config, const xiaotai_output_t **output);

#ifdef __cplusplus
}
#endif
