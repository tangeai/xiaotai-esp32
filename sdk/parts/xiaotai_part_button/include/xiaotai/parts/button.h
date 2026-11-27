// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: a push button on a line, as an input part.
 *
 * The part reports debounced edges (XIAOTAI_INPUT_PRESSED, _RELEASED); clicks,
 * double clicks, long presses and their timing belong to the interaction
 * engine and the application's interaction profile, so one button driver
 * serves every product whatever its gestures are. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_BUTTON_DEBOUNCE_MS 50U

typedef struct {
    xiaotai_line_t line;        /* get() reports "pressed" */
    const char *role;           /* static string: "main", "volume_up", ... */
    uint16_t debounce_ms;       /* the level must hold this long; 0: XIAOTAI_BUTTON_DEBOUNCE_MS */
    /* The line is a switch that holds a state (a door contact): kind CONTACT,
     * and a state already active at open() is reported by the first poll
     * after the debounce instead of waiting for a release. */
    bool contact;
    /* A release counts once the line has stayed released this long; a press
     * sooner continues the one before, for a key whose contact drops out under
     * the finger. 0: debounce_ms, as for a press. */
    uint16_t release_ms;
} xiaotai_button_config_t;

/* [board] Creates the input part (kind BUTTON, or CONTACT; events PRESSED |
 * RELEASED). The object is allocated once and never freed. Its poll() reads
 * the line once. */
xiaotai_err_t xiaotai_button_create(const xiaotai_button_config_t *config, const xiaotai_input_t **input);

/* The debounce, for parts that sample a level in their own way and for host
 * tests. Zero-initialize the state. A level is accepted after it has held for
 * debounce_ms; a key held at the first step reports nothing until it has been
 * released once. */
typedef struct {
    bool initialized, stable, candidate, armed;
    uint16_t held_ms;
} xiaotai_debounce_t;

/* [engine] One sample every period_ms. Returns XIAOTAI_INPUT_PRESSED,
 * XIAOTAI_INPUT_RELEASED or 0. */
uint32_t xiaotai_debounce_step(xiaotai_debounce_t *state, bool pressed, uint16_t period_ms, uint16_t debounce_ms);

#ifdef __cplusplus
}
#endif
