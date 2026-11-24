// SPDX-License-Identifier: MIT
/* Host implementation of bus_io: fake I2C devices, pins and PWM channels that
 * tests program through xiaotai_bus_host.h. One global lock guards the fake
 * state; it is never held while a test hook runs, so a hook may block. */
#include "bus_io.h"
#include "xiaotai_bus_host.h"

#include <pthread.h>
#include <string.h>

#define DEVICES 32
#define REGISTERS 256

struct xiaotai_bus_io_i2c {
    int port;
    bool open;
    xiaotai_i2c_config_t config;
    uint32_t busy;                     /* transfers in progress on this port */
    xiaotai_bus_host_i2c_hook_t hook;
    void *hook_ctx;
};

typedef struct {
    uint32_t nth;
    xiaotai_err_t error;
} injection_t;

typedef struct {
    bool used;
    int port;
    uint8_t address;
    uint8_t registers[REGISTERS];
    injection_t fail[3];               /* by xiaotai_bus_host_op_t */
} device_t;

typedef struct {
    xiaotai_bus_host_pin_t state;
    bool external;                     /* level applied from outside */
    xiaotai_err_t fail;
} pin_t;

typedef struct {
    xiaotai_bus_host_pwm_t state;
    xiaotai_err_t fail;
} pwm_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct xiaotai_bus_io_i2c s_ports[XIAOTAI_BUS_HOST_PORTS];
static device_t s_devices[DEVICES];
static pin_t s_pins[XIAOTAI_BUS_HOST_PINS];
static pwm_t s_pwm[XIAOTAI_BUS_HOST_PWM_CHANNELS];
static xiaotai_bus_host_transfer_t s_log[XIAOTAI_BUS_HOST_LOG];
static size_t s_log_next, s_log_count;
static uint32_t s_transfers, s_overlaps;

void xiaotai_bus_host_reset(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_ports, 0, sizeof(s_ports));
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_pins, 0, sizeof(s_pins));
    memset(s_pwm, 0, sizeof(s_pwm));
    s_log_next = s_log_count = 0;
    s_transfers = s_overlaps = 0;
    pthread_mutex_unlock(&s_lock);
}

/* --------------------------------------------------------------------- I2C */

static bool valid_port(int port)
{
    return port >= 0 && port < XIAOTAI_BUS_HOST_PORTS;
}

/* Caller holds s_lock. */
static device_t *find_device(int port, uint8_t address)
{
    for (size_t i = 0; i < DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].port == port && s_devices[i].address == address) {
            return &s_devices[i];
        }
    }
    return NULL;
}

xiaotai_err_t xiaotai_bus_io_i2c_open(const xiaotai_i2c_config_t *config, xiaotai_bus_io_i2c_t **io)
{
    if (!valid_port(config->port)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    struct xiaotai_bus_io_i2c *port = &s_ports[config->port];
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (!port->open) {
        *port = (struct xiaotai_bus_io_i2c){.port = config->port, .open = true, .config = *config};
        *io = port;
        err = XIAOTAI_OK;
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

static void log_transfer(const xiaotai_bus_host_transfer_t *transfer)
{
    s_log[s_log_next] = *transfer;
    s_log_next = (s_log_next + 1) % XIAOTAI_BUS_HOST_LOG;
    if (s_log_count < XIAOTAI_BUS_HOST_LOG) ++s_log_count;
}

static xiaotai_err_t transfer(xiaotai_bus_io_i2c_t *io, xiaotai_bus_host_op_t op, uint8_t address, uint8_t reg,
                              uint8_t *read, const uint8_t *write, size_t size, uint32_t timeout_ms)
{
    xiaotai_bus_host_transfer_t record = {
        .op = op, .port = io->port, .address = address, .reg = reg, .size = size,
        .timeout_ms = timeout_ms, .thread = (uintptr_t)pthread_self(),
    };
    if (write && size > 0) {
        memcpy(record.data, write, size < XIAOTAI_BUS_HOST_DATA ? size : XIAOTAI_BUS_HOST_DATA);
    }

    pthread_mutex_lock(&s_lock);
    if (io->busy > 0) ++s_overlaps;
    ++io->busy;
    ++s_transfers;
    xiaotai_err_t err = XIAOTAI_OK;
    device_t *device = find_device(io->port, address);
    if (!device) {
        err = XIAOTAI_ERR_NOT_FOUND;
    } else if (device->fail[op].nth > 0 && --device->fail[op].nth == 0) {
        err = device->fail[op].error;
    }
    const xiaotai_bus_host_i2c_hook_t hook = io->hook;
    void *const hook_ctx = io->hook_ctx;
    pthread_mutex_unlock(&s_lock);

    bool apply = true;
    if (err == XIAOTAI_OK && hook) err = hook(hook_ctx, &record, &apply);

    pthread_mutex_lock(&s_lock);
    device = find_device(io->port, address);
    if (err == XIAOTAI_OK && !device) err = XIAOTAI_ERR_NOT_FOUND;
    if (err == XIAOTAI_OK && op == XIAOTAI_BUS_HOST_WRITE && apply) {
        for (size_t i = 0; i < size; ++i) device->registers[(uint8_t)(reg + i)] = write[i];
    }
    if (err == XIAOTAI_OK && op == XIAOTAI_BUS_HOST_READ) {
        for (size_t i = 0; i < size; ++i) read[i] = device->registers[(uint8_t)(reg + i)];
        memcpy(record.data, read, size < XIAOTAI_BUS_HOST_DATA ? size : XIAOTAI_BUS_HOST_DATA);
    }
    record.result = err;
    log_transfer(&record);
    --io->busy;
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_err_t xiaotai_bus_io_i2c_probe(xiaotai_bus_io_i2c_t *io, uint8_t address, uint32_t timeout_ms)
{
    return transfer(io, XIAOTAI_BUS_HOST_PROBE, address, 0, NULL, NULL, 0, timeout_ms);
}

xiaotai_err_t xiaotai_bus_io_i2c_read(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg, uint8_t *data,
                                      size_t size, uint32_t timeout_ms)
{
    return transfer(io, XIAOTAI_BUS_HOST_READ, address, reg, data, NULL, size, timeout_ms);
}

xiaotai_err_t xiaotai_bus_io_i2c_write(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg,
                                       const uint8_t *data, size_t size, uint32_t timeout_ms)
{
    return transfer(io, XIAOTAI_BUS_HOST_WRITE, address, reg, NULL, data, size, timeout_ms);
}

void *xiaotai_bus_io_i2c_native(xiaotai_bus_io_i2c_t *io)
{
    return io;
}

bool xiaotai_bus_host_i2c_port(int port, xiaotai_i2c_config_t *config)
{
    if (!valid_port(port)) return false;
    pthread_mutex_lock(&s_lock);
    const bool open = s_ports[port].open;
    if (open && config) *config = s_ports[port].config;
    pthread_mutex_unlock(&s_lock);
    return open;
}

xiaotai_err_t xiaotai_bus_host_i2c_attach(int port, uint8_t address)
{
    if (!valid_port(port)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    device_t *device = find_device(port, address);
    for (size_t i = 0; !device && i < DEVICES; ++i) {
        if (!s_devices[i].used) device = &s_devices[i];
    }
    if (device) *device = (device_t){.used = true, .port = port, .address = address};
    pthread_mutex_unlock(&s_lock);
    return device ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

void xiaotai_bus_host_i2c_detach(int port, uint8_t address)
{
    pthread_mutex_lock(&s_lock);
    device_t *device = find_device(port, address);
    if (device) device->used = false;
    pthread_mutex_unlock(&s_lock);
}

uint8_t xiaotai_bus_host_i2c_peek(int port, uint8_t address, uint8_t reg)
{
    pthread_mutex_lock(&s_lock);
    const device_t *device = find_device(port, address);
    const uint8_t value = device ? device->registers[reg] : 0;
    pthread_mutex_unlock(&s_lock);
    return value;
}

void xiaotai_bus_host_i2c_poke(int port, uint8_t address, uint8_t reg, uint8_t value)
{
    pthread_mutex_lock(&s_lock);
    device_t *device = find_device(port, address);
    if (device) device->registers[reg] = value;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_bus_host_i2c_fail(int port, uint8_t address, xiaotai_bus_host_op_t op, uint32_t nth,
                               xiaotai_err_t error)
{
    if ((unsigned)op > XIAOTAI_BUS_HOST_WRITE) return;
    pthread_mutex_lock(&s_lock);
    device_t *device = find_device(port, address);
    if (device) device->fail[op] = (injection_t){.nth = nth, .error = error};
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_bus_host_i2c_hook(int port, xiaotai_bus_host_i2c_hook_t hook, void *ctx)
{
    if (!valid_port(port)) return;
    pthread_mutex_lock(&s_lock);
    s_ports[port].hook = hook;
    s_ports[port].hook_ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

size_t xiaotai_bus_host_i2c_log(xiaotai_bus_host_transfer_t *out, size_t max)
{
    pthread_mutex_lock(&s_lock);
    const size_t count = s_log_count < max ? s_log_count : max;
    const size_t oldest = (s_log_next + XIAOTAI_BUS_HOST_LOG - s_log_count) % XIAOTAI_BUS_HOST_LOG;
    for (size_t i = 0; i < count; ++i) out[i] = s_log[(oldest + i) % XIAOTAI_BUS_HOST_LOG];
    pthread_mutex_unlock(&s_lock);
    return count;
}

uint32_t xiaotai_bus_host_i2c_transfers(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t transfers = s_transfers;
    pthread_mutex_unlock(&s_lock);
    return transfers;
}

void xiaotai_bus_host_i2c_log_clear(void)
{
    pthread_mutex_lock(&s_lock);
    s_log_next = s_log_count = 0;
    s_transfers = 0;
    pthread_mutex_unlock(&s_lock);
}

uint32_t xiaotai_bus_host_i2c_overlaps(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t overlaps = s_overlaps;
    pthread_mutex_unlock(&s_lock);
    return overlaps;
}

/* -------------------------------------------------------------------- pins */

static bool valid_pin(int pin)
{
    return pin >= 0 && pin < XIAOTAI_BUS_HOST_PINS;
}

/* Caller holds s_lock. Consumes the pin's injected failure, if any. */
static xiaotai_err_t pin_injected(pin_t *pin)
{
    const xiaotai_err_t err = pin->fail;
    pin->fail = XIAOTAI_OK;
    return err;
}

xiaotai_err_t xiaotai_bus_io_pin_setup(const xiaotai_gpio_config_t *config, bool output, bool level)
{
    if (!valid_pin(config->pin)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pin_t *pin = &s_pins[config->pin];
    xiaotai_err_t err = pin_injected(pin);
    if (err == XIAOTAI_OK) {
        pin->state = (xiaotai_bus_host_pin_t){
            .configured = true, .output = output, .open_drain = config->open_drain,
            .pull_up = config->pull_up, .pull_down = config->pull_down,
            .level = output ? level : pin->external,
            .level_when_output = output && level,
        };
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_err_t xiaotai_bus_io_pin_set(int pin_number, bool level)
{
    if (!valid_pin(pin_number)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pin_t *pin = &s_pins[pin_number];
    xiaotai_err_t err = pin_injected(pin);
    if (err == XIAOTAI_OK && !(pin->state.configured && pin->state.output)) err = XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) {
        pin->state.level = level;
        ++pin->state.sets;
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_err_t xiaotai_bus_io_pin_get(int pin_number, bool *level)
{
    if (!valid_pin(pin_number)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pin_t *pin = &s_pins[pin_number];
    xiaotai_err_t err = pin_injected(pin);
    if (err == XIAOTAI_OK && !pin->state.configured) err = XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) *level = pin->state.level;
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_bus_host_pin_t xiaotai_bus_host_pin(int pin_number)
{
    xiaotai_bus_host_pin_t state = {0};
    if (!valid_pin(pin_number)) return state;
    pthread_mutex_lock(&s_lock);
    state = s_pins[pin_number].state;
    pthread_mutex_unlock(&s_lock);
    return state;
}

void xiaotai_bus_host_pin_drive(int pin_number, bool level)
{
    if (!valid_pin(pin_number)) return;
    pthread_mutex_lock(&s_lock);
    pin_t *pin = &s_pins[pin_number];
    pin->external = level;
    if (!pin->state.output) pin->state.level = level;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_bus_host_pin_fail(int pin_number, xiaotai_err_t error)
{
    if (!valid_pin(pin_number)) return;
    pthread_mutex_lock(&s_lock);
    s_pins[pin_number].fail = error;
    pthread_mutex_unlock(&s_lock);
}

/* --------------------------------------------------------------------- PWM */

static xiaotai_err_t pwm_injected(pwm_t *pwm)
{
    const xiaotai_err_t err = pwm->fail;
    pwm->fail = XIAOTAI_OK;
    return err;
}

xiaotai_err_t xiaotai_bus_io_pwm_setup(const xiaotai_pwm_config_t *config, uint32_t duty)
{
    if (config->channel >= XIAOTAI_BUS_HOST_PWM_CHANNELS) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pwm_t *pwm = &s_pwm[config->channel];
    xiaotai_err_t err = pwm_injected(pwm);
    if (err == XIAOTAI_OK) {
        pwm->state = (xiaotai_bus_host_pwm_t){
            .configured = true, .pin = config->pin, .timer = config->timer, .hz = config->hz,
            .resolution_bits = config->resolution_bits, .active_low = config->active_low, .duty = duty,
        };
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_err_t xiaotai_bus_io_pwm_set(uint8_t channel, uint32_t duty)
{
    if (channel >= XIAOTAI_BUS_HOST_PWM_CHANNELS) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pwm_t *pwm = &s_pwm[channel];
    xiaotai_err_t err = pwm_injected(pwm);
    if (err == XIAOTAI_OK && !pwm->state.configured) err = XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK && duty >= (1UL << pwm->state.resolution_bits)) err = XIAOTAI_ERR_INVALID_ARG;
    if (err == XIAOTAI_OK) {
        pwm->state.duty = duty;
        ++pwm->state.updates;
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_err_t xiaotai_bus_io_pwm_stop(uint8_t channel)
{
    if (channel >= XIAOTAI_BUS_HOST_PWM_CHANNELS) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    pwm_t *pwm = &s_pwm[channel];
    xiaotai_err_t err = pwm_injected(pwm);
    if (err == XIAOTAI_OK && !pwm->state.configured) err = XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) {
        pwm->state.duty = 0;
        pwm->state.configured = false;
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

xiaotai_bus_host_pwm_t xiaotai_bus_host_pwm(uint8_t channel)
{
    xiaotai_bus_host_pwm_t state = {0};
    if (channel >= XIAOTAI_BUS_HOST_PWM_CHANNELS) return state;
    pthread_mutex_lock(&s_lock);
    state = s_pwm[channel].state;
    pthread_mutex_unlock(&s_lock);
    return state;
}

void xiaotai_bus_host_pwm_fail(uint8_t channel, xiaotai_err_t error)
{
    if (channel >= XIAOTAI_BUS_HOST_PWM_CHANNELS) return;
    pthread_mutex_lock(&s_lock);
    s_pwm[channel].fail = error;
    pthread_mutex_unlock(&s_lock);
}
