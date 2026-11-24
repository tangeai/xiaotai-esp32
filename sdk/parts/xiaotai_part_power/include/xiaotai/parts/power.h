// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: power-management and IO-expander chips on I2C. They are not
 * parts of their own: they give other parts their lines (a chip select, an
 * amplifier enable, a sensor power-down, a panel reset, a backlight rail) and,
 * for the power key, an input part.
 *
 * One chip serves several parts from several tasks, so every register change
 * is a locked read-modify-write (xiaotai_i2c_update) or, for the expander
 * with a write-only view, a locked shadow that is updated only after the
 * write succeeded. Objects are allocated once at [board] and never freed;
 * lines made from them may be used from any task (never from an interrupt). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t reg;
    uint8_t value;
} xiaotai_reg_value_t;

/* ---------------------------------------------- PCA9557, 8-bit IO expander */

typedef struct xiaotai_pca9557 xiaotai_pca9557_t;

typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;
    uint8_t outputs;            /* mask of pins that become outputs */
    uint8_t initial;            /* output register written before the pins become outputs */
    uint16_t lock_timeout_ms;   /* wait for another task's change; 0: 500 */
} xiaotai_pca9557_config_t;

/* [board] Writes `initial`, then switches `outputs` to outputs. */
xiaotai_err_t xiaotai_pca9557_create(const xiaotai_pca9557_config_t *config, xiaotai_pca9557_t **chip);
/* [board] Output pin 0..7 as a line. XIAOTAI_ERR_TIMEOUT from set() when the
 * chip stays locked longer than lock_timeout_ms. */
xiaotai_err_t xiaotai_pca9557_line(xiaotai_pca9557_t *chip, uint8_t pin, bool active_low, xiaotai_line_t *line);

/* ------------------------------------- AW9523, 16-bit IO expander / LED driver */

typedef struct xiaotai_aw9523 xiaotai_aw9523_t;

typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;
    const xiaotai_reg_value_t *setup;   /* registers written in order at creation */
    size_t setup_count;
} xiaotai_aw9523_config_t;

xiaotai_err_t xiaotai_aw9523_create(const xiaotai_aw9523_config_t *config, xiaotai_aw9523_t **chip);
/* [board] Output pin `pin` (0..7) of port 0 or 1 as a line. */
xiaotai_err_t xiaotai_aw9523_line(xiaotai_aw9523_t *chip, uint8_t port, uint8_t pin, bool active_low,
                                  xiaotai_line_t *line);

/* ------------------------------------------------ AXP2101, power management */

typedef struct xiaotai_axp2101 xiaotai_axp2101_t;

typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;
    const xiaotai_reg_value_t *setup;   /* rail voltages and switches written in order at creation */
    size_t setup_count;
} xiaotai_axp2101_config_t;

xiaotai_err_t xiaotai_axp2101_create(const xiaotai_axp2101_config_t *config, xiaotai_axp2101_t **chip);

typedef struct {
    uint8_t enable_reg;         /* register with the rail switches */
    uint8_t enable_mask;        /* bits that switch this line's rails */
    uint8_t voltage_reg;        /* 0: the voltage is left as set up */
    uint8_t voltage_code;       /* written to voltage_reg before the rails turn on */
} xiaotai_axp2101_rail_t;

/* [board] One or more rails as a line: active = on. */
xiaotai_err_t xiaotai_axp2101_line(xiaotai_axp2101_t *chip, const xiaotai_axp2101_rail_t *rail,
                                   xiaotai_line_t *line);

/* [board] The power key as an input part (kind BUTTON, events CLICK): the chip
 * reports finished short presses, not edges. open() clears a press latched
 * before start; poll() reads the chip every fifth call (50 ms) and counts
 * read failures. */
xiaotai_err_t xiaotai_axp2101_power_key(xiaotai_axp2101_t *chip, const char *role, const xiaotai_input_t **input);

/* ------------------------------------------------------------- diagnostics */

/* [any] One line per chip for the board's Report() (xiaotai/board.hpp): the
 * chip's name, its address and the registers this library uses, read now
 * under the chip's lock. A register that cannot be read is written as "??".
 * Blocks for bus transfers; never from a real-time task. */
void xiaotai_pca9557_report(xiaotai_pca9557_t *chip, xiaotai_hal_write_fn_t write, void *ctx);
void xiaotai_aw9523_report(xiaotai_aw9523_t *chip, xiaotai_hal_write_fn_t write, void *ctx);
void xiaotai_axp2101_report(xiaotai_axp2101_t *chip, xiaotai_hal_write_fn_t write, void *ctx);

#ifdef __cplusplus
}
#endif
