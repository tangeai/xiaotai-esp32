// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: what the parts of one board share - an I2C bus, control
 * lines and levels.
 *
 * A line is one on/off control signal whatever drives it: a pin of the chip,
 * a bit of an IO expander, a rail of a power-management chip. Chip drivers
 * take lines, so one driver serves the board that wires its reset to a pin
 * and the board that wires it to an expander. A level is the same idea for a
 * 0..100 % control (a backlight).
 *
 * Lines and levels are small values copied into driver configurations. What a
 * line refers to (the expander object, the pin) lives until power-off.
 *
 * Calling tasks: creation is [board] (the board's Init()); transfers, set and
 * get may be called from any task and never from an interrupt handler. They
 * block for at most the bus timeout. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- I2C */

typedef struct xiaotai_i2c_bus xiaotai_i2c_bus_t;

typedef struct {
    int port;               /* controller index */
    int sda_pin, scl_pin;
    uint32_t hz;            /* speed of the transfers below */
    bool pullup;            /* enable the chip's internal pull-ups */
    uint16_t timeout_ms;    /* per transfer, and for waiting on the bus; 0: 100 */
} xiaotai_i2c_config_t;

/* [board] The bus object is allocated once (XIAOTAI_MEM_INTERNAL) and never
 * freed. XIAOTAI_ERR_INVALID_STATE when the controller is already in use. */
xiaotai_err_t xiaotai_i2c_create(const xiaotai_i2c_config_t *config, xiaotai_i2c_bus_t **bus);

/* Transfers on one bus are serialized; XIAOTAI_ERR_TIMEOUT when the bus stays
 * busy or the device stretches past the timeout, XIAOTAI_ERR_NOT_FOUND when
 * the address does not acknowledge. */
xiaotai_err_t xiaotai_i2c_probe(xiaotai_i2c_bus_t *bus, uint8_t address);
xiaotai_err_t xiaotai_i2c_read(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, uint8_t *data, size_t size);
xiaotai_err_t xiaotai_i2c_write(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, const uint8_t *data,
                                size_t size);
/* Read, replace the bits in mask with those of bits, write; one bus hold, so
 * two tasks changing different bits of one register never lose an update. A
 * failed write leaves the register as it was. */
xiaotai_err_t xiaotai_i2c_update(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, uint8_t mask, uint8_t bits);

/* The platform's bus handle, for drivers built on platform libraries (ESP-IDF:
 * i2c_master_bus_handle_t). Devices added through it share the wires but not
 * the serialization above: such a driver keeps its own transfers short. */
void *xiaotai_i2c_native(xiaotai_i2c_bus_t *bus);

/* ------------------------------------------------------------------- lines */

typedef struct xiaotai_line {
    xiaotai_err_t (*set)(void *self, bool active);
    xiaotai_err_t (*get)(void *self, bool *active);
    void *self;
} xiaotai_line_t;

/* A zeroed line is "not connected": setting it succeeds and does nothing,
 * reading it reports inactive. */
static inline bool xiaotai_line_connected(const xiaotai_line_t *line)
{
    return line && (line->set || line->get);
}

static inline xiaotai_err_t xiaotai_line_set(const xiaotai_line_t *line, bool active)
{
    return line && line->set ? line->set(line->self, active) : XIAOTAI_OK;
}

static inline xiaotai_err_t xiaotai_line_get(const xiaotai_line_t *line, bool *active)
{
    if (line && line->get) return line->get(line->self, active);
    *active = false;
    return XIAOTAI_OK;
}

typedef struct {
    int pin;
    bool active_low;        /* active drives (reads) the low level */
    bool pull_up, pull_down;
    bool open_drain;
    bool initially_active;  /* outputs: state driven before the pin becomes an output */
} xiaotai_gpio_config_t;

/* [board] A pin of the chip as a line. No allocation: the line carries the pin. */
xiaotai_err_t xiaotai_line_gpio_output(const xiaotai_gpio_config_t *config, xiaotai_line_t *line);
xiaotai_err_t xiaotai_line_gpio_input(const xiaotai_gpio_config_t *config, xiaotai_line_t *line);

/* ------------------------------------------------------------------ levels */

typedef struct xiaotai_level {
    xiaotai_err_t (*set)(void *self, uint8_t percent);  /* 0..100; above: XIAOTAI_ERR_INVALID_ARG */
    bool dimmable;          /* false: any non-zero percent is fully on */
    void *self;
} xiaotai_level_t;

typedef struct {
    int pin;
    uint8_t timer, channel; /* PWM timer and channel of the chip */
    uint32_t hz;
    uint8_t resolution_bits;
    bool active_low;
} xiaotai_pwm_config_t;

/* [board] A PWM output as a dimmable level, starting at 0 %. */
xiaotai_err_t xiaotai_level_pwm(const xiaotai_pwm_config_t *config, xiaotai_level_t *level);
/* [board] Makes a PWM level and its enable line without starting hardware.
 * The line starts the PWM at 0% and stops it at its inactive pin level.
 * Level writes while the line is off return XIAOTAI_ERR_INVALID_STATE.
 * The owner serializes operations on the pair. */
xiaotai_err_t xiaotai_level_pwm_managed(const xiaotai_pwm_config_t *config, xiaotai_level_t *level,
                                         xiaotai_line_t *enable);
/* [board] An on/off line as a level. *line must have static storage: the level
 * refers to it. */
void xiaotai_level_from_line(const xiaotai_line_t *line, xiaotai_level_t *level);

#ifdef __cplusplus
}
#endif
