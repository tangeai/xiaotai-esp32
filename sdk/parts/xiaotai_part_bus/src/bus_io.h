// SPDX-License-Identifier: MIT
#pragma once
/* What the bus component needs from the platform: the smallest I2C, pin and
 * PWM actions. esp-idf/ implements them on the ESP-IDF drivers, host/ on fake
 * devices and pins that tests program (xiaotai_bus_host.h).
 *
 * Everything else - argument checks, the per-bus lock, read-modify-write,
 * line polarity, duty arithmetic - is portable (src/i2c.c, src/line.c), so
 * the host tests exercise the code that runs on the device.
 *
 * Callers: the I2C transfer functions are called with the bus object's lock
 * held, so they never run concurrently for one bus; open, pin and PWM setup
 * run from the board's Init() only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest register write through xiaotai_i2c_write: the platform sends
 * register and data as one transfer from a stack copy. Chip register writes
 * through this API are a few bytes; bulk data goes through drivers that use
 * xiaotai_i2c_native(). */
#define XIAOTAI_BUS_IO_I2C_WRITE_MAX 32U

/* ---------------------------------------------------------------------- I2C */

typedef struct xiaotai_bus_io_i2c xiaotai_bus_io_i2c_t;

/* Takes the controller. XIAOTAI_ERR_INVALID_STATE when the controller is
 * already in use; config->timeout_ms is already resolved (never 0). */
xiaotai_err_t xiaotai_bus_io_i2c_open(const xiaotai_i2c_config_t *config, xiaotai_bus_io_i2c_t **io);
/* Results: XIAOTAI_OK; XIAOTAI_ERR_NOT_FOUND when the address does not
 * acknowledge; XIAOTAI_ERR_TIMEOUT when the transfer does not end within
 * timeout_ms; anything else as the platform reports it. */
xiaotai_err_t xiaotai_bus_io_i2c_probe(xiaotai_bus_io_i2c_t *io, uint8_t address, uint32_t timeout_ms);
/* Writes reg, then reads size bytes (repeated start). size > 0. */
xiaotai_err_t xiaotai_bus_io_i2c_read(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg, uint8_t *data,
                                      size_t size, uint32_t timeout_ms);
/* Writes reg followed by size bytes (size <= XIAOTAI_BUS_IO_I2C_WRITE_MAX,
 * may be 0) in one transfer. */
xiaotai_err_t xiaotai_bus_io_i2c_write(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg,
                                       const uint8_t *data, size_t size, uint32_t timeout_ms);
void *xiaotai_bus_io_i2c_native(xiaotai_bus_io_i2c_t *io);

/* --------------------------------------------------------------------- pins */

/* Configures config->pin. For an output, level is the electrical level the
 * pin drives, set before the pin starts driving; outputs keep their input
 * buffer so xiaotai_bus_io_pin_get reads back what the pin is at. */
xiaotai_err_t xiaotai_bus_io_pin_setup(const xiaotai_gpio_config_t *config, bool output, bool level);
xiaotai_err_t xiaotai_bus_io_pin_set(int pin, bool level);
xiaotai_err_t xiaotai_bus_io_pin_get(int pin, bool *level);

/* ---------------------------------------------------------------------- PWM */

/* Configures the timer and the channel and starts the output at duty (a raw
 * value of config->resolution_bits bits, already inverted for active_low). */
xiaotai_err_t xiaotai_bus_io_pwm_setup(const xiaotai_pwm_config_t *config, uint32_t duty);
xiaotai_err_t xiaotai_bus_io_pwm_set(uint8_t channel, uint32_t duty);
xiaotai_err_t xiaotai_bus_io_pwm_stop(uint8_t channel, bool idle_high);

#ifdef __cplusplus
}
#endif
