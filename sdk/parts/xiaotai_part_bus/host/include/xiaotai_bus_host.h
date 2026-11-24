// SPDX-License-Identifier: MIT
#pragma once
/* Test controls of the host bus implementation: fake I2C devices, fake pins
 * and fake PWM channels behind xiaotai/parts/bus.h. Tests of every part built
 * on the bus program the devices here, inject failures and read back what
 * the code under test did.
 *
 * Fake I2C device: 256 byte-wide registers, zero after attach. A read
 * returns consecutive registers from reg, a write stores consecutive
 * registers from reg (both wrap at 0xff). A failed transfer changes nothing.
 * A transfer to an address with no device fails with XIAOTAI_ERR_NOT_FOUND.
 *
 * All functions are thread-safe. Nothing here allocates through the port, so
 * the host memory counters see only the code under test. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_BUS_HOST_PORTS 8
#define XIAOTAI_BUS_HOST_PINS 64
#define XIAOTAI_BUS_HOST_PWM_CHANNELS 8
#define XIAOTAI_BUS_HOST_LOG 256      /* transfers kept, oldest dropped first */
#define XIAOTAI_BUS_HOST_DATA 16      /* data bytes kept per logged transfer */

/* Forgets every device, pin, channel, hook, injected failure and the log, and
 * frees every I2C port. Bus objects created before stay allocated (they are
 * never freed) but must not be used afterwards. */
void xiaotai_bus_host_reset(void);

/* --------------------------------------------------------------------- I2C */

typedef enum {
    XIAOTAI_BUS_HOST_PROBE,
    XIAOTAI_BUS_HOST_READ,
    XIAOTAI_BUS_HOST_WRITE,
} xiaotai_bus_host_op_t;

typedef struct {
    xiaotai_bus_host_op_t op;
    int port;
    uint8_t address;
    uint8_t reg;                  /* not used by a probe */
    size_t size;                  /* data bytes read or written */
    /* Written bytes, or the bytes a read returned; the first
     * XIAOTAI_BUS_HOST_DATA. Filled in the log only. */
    uint8_t data[XIAOTAI_BUS_HOST_DATA];
    uint32_t timeout_ms;          /* the limit the transfer was given */
    uintptr_t thread;             /* calling thread, for ordering checks */
    xiaotai_err_t result;         /* filled in the log only */
} xiaotai_bus_host_transfer_t;

/* How the controller was opened, as xiaotai_i2c_create passed it (timeout
 * resolved). False when the port is not open. */
bool xiaotai_bus_host_i2c_port(int port, xiaotai_i2c_config_t *config);

/* A device answers at address from now on. XIAOTAI_ERR_NO_MEM when the
 * table (32 devices) is full; attaching again clears its registers. */
xiaotai_err_t xiaotai_bus_host_i2c_attach(int port, uint8_t address);
/* The address stops acknowledging. */
void xiaotai_bus_host_i2c_detach(int port, uint8_t address);
/* Register access without a transfer (nothing logged). peek of an absent
 * device reads 0; poke of an absent device does nothing. */
uint8_t xiaotai_bus_host_i2c_peek(int port, uint8_t address, uint8_t reg);
void xiaotai_bus_host_i2c_poke(int port, uint8_t address, uint8_t reg, uint8_t value);

/* The nth transfer of kind op to this device from now (1: the next) fails
 * with error, once, and applies nothing. 0 cancels. One pending injection
 * per device and kind. */
void xiaotai_bus_host_i2c_fail(int port, uint8_t address, xiaotai_bus_host_op_t op, uint32_t nth,
                               xiaotai_err_t error);

/* Called inside every transfer on the port, from the calling thread, while
 * the bus component holds the bus and before the transfer applies (an
 * injected failure is decided first and skips the hook). It may block (to
 * hold the bus), call peek and poke, and:
 *   - return an error: the transfer fails with it and applies nothing;
 *   - clear *apply: a write succeeds without storing its bytes (the hook
 *     stored what the device would, with poke: write-one-to-clear bits, a
 *     read-only register). A read always returns the registers as they are
 *     after the hook.
 * NULL removes the hook. */
typedef xiaotai_err_t (*xiaotai_bus_host_i2c_hook_t)(void *ctx, const xiaotai_bus_host_transfer_t *transfer,
                                                     bool *apply);
void xiaotai_bus_host_i2c_hook(int port, xiaotai_bus_host_i2c_hook_t hook, void *ctx);

/* Copies the logged transfers, oldest first, up to max; returns how many were
 * copied. */
size_t xiaotai_bus_host_i2c_log(xiaotai_bus_host_transfer_t *out, size_t max);
/* Transfers since the last reset or log clear, including those that dropped
 * out of the log. */
uint32_t xiaotai_bus_host_i2c_transfers(void);
void xiaotai_bus_host_i2c_log_clear(void);
/* Times a transfer started on a port while another transfer on the same port
 * was still in progress: 0 as long as the bus component serializes. */
uint32_t xiaotai_bus_host_i2c_overlaps(void);

/* -------------------------------------------------------------------- pins */

typedef struct {
    bool configured;
    bool output;
    bool open_drain, pull_up, pull_down;
    bool level;                   /* level the pin is at (driven, or applied to an input) */
    bool level_when_output;       /* level driven at the moment the pin became an output */
    uint32_t sets;                /* successful level writes after setup */
} xiaotai_bus_host_pin_t;

/* State of a pin; all false for a pin outside 0..XIAOTAI_BUS_HOST_PINS-1. */
xiaotai_bus_host_pin_t xiaotai_bus_host_pin(int pin);
/* Level an external circuit applies to the pin; what an input reads. */
void xiaotai_bus_host_pin_drive(int pin, bool level);
/* The next setup, set or get of the pin fails with error, once, and changes
 * nothing. XIAOTAI_OK cancels. */
void xiaotai_bus_host_pin_fail(int pin, xiaotai_err_t error);

/* --------------------------------------------------------------------- PWM */

typedef struct {
    bool configured;
    int pin;
    uint8_t timer;
    uint32_t hz;
    uint8_t resolution_bits;
    bool active_low;              /* the output is inverted */
    uint32_t duty;                /* raw duty now: the active part of the period */
    uint32_t updates;             /* successful duty changes after setup */
} xiaotai_bus_host_pwm_t;

xiaotai_bus_host_pwm_t xiaotai_bus_host_pwm(uint8_t channel);
/* The next setup or duty change of the channel fails with error, once, and
 * changes nothing. XIAOTAI_OK cancels. */
void xiaotai_bus_host_pwm_fail(uint8_t channel, xiaotai_err_t error);

#ifdef __cplusplus
}
#endif
