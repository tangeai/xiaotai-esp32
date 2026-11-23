// SPDX-License-Identifier: MIT
/* Lines on chip pins and levels on PWM outputs or lines: polarity, argument
 * checks and duty arithmetic. The pin and PWM work is in bus_io. */
#include "xiaotai/parts/bus.h"

#include "bus_io.h"
#include "xiaotai/port/mem.h"

#define PERCENT_MAX 100U
#define PWM_BITS_MAX 20U

/* ---------------------------------------------------------------- pin lines */

/* A pin line carries its pin and polarity in self, so making one allocates
 * nothing: bit 0 is active_low, the bits above are the pin number. */
static void *pin_self(int pin, bool active_low)
{
    return (void *)(((uintptr_t)(unsigned)pin << 1) | (active_low ? 1U : 0U));
}

static int pin_of(void *self)
{
    return (int)((uintptr_t)self >> 1);
}

static bool active_low_of(void *self)
{
    return ((uintptr_t)self & 1U) != 0;
}

static xiaotai_err_t pin_line_set(void *self, bool active)
{
    return xiaotai_bus_io_pin_set(pin_of(self), active != active_low_of(self));
}

static xiaotai_err_t pin_line_get(void *self, bool *active)
{
    if (!active) return XIAOTAI_ERR_INVALID_ARG;
    bool level = false;
    xiaotai_err_t err = xiaotai_bus_io_pin_get(pin_of(self), &level);
    *active = err == XIAOTAI_OK && level != active_low_of(self);
    return err;
}

/* An input line refuses to be driven instead of ignoring it, so a board that
 * wires an input where an output belongs fails at its first use. */
static xiaotai_err_t input_line_set(void *self, bool active)
{
    (void)self;
    (void)active;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t make_pin_line(const xiaotai_gpio_config_t *config, bool output, xiaotai_line_t *line)
{
    /* The line stays "not connected" unless the pin was set up. */
    if (line) *line = (xiaotai_line_t){0};
    if (!config || !line || config->pin < 0) return XIAOTAI_ERR_INVALID_ARG;
    const bool level = output && (config->initially_active != config->active_low);
    xiaotai_err_t err = xiaotai_bus_io_pin_setup(config, output, level);
    if (err != XIAOTAI_OK) return err;
    *line = (xiaotai_line_t){
        .set = output ? pin_line_set : input_line_set,
        .get = pin_line_get,
        .self = pin_self(config->pin, config->active_low),
    };
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_line_gpio_output(const xiaotai_gpio_config_t *config, xiaotai_line_t *line)
{
    return make_pin_line(config, true, line);
}

xiaotai_err_t xiaotai_line_gpio_input(const xiaotai_gpio_config_t *config, xiaotai_line_t *line)
{
    return make_pin_line(config, false, line);
}

/* ------------------------------------------------------------------- levels */

typedef struct {
    uint32_t full;          /* duty at 100 %: 2^bits - 1 */
    xiaotai_pwm_config_t config;
    bool running;
} pwm_level_t;

/* Duty for percent, rounded down as the existing backlights compute it
 * (full * percent / 100), then inverted for an active-low output. */
static uint32_t pwm_duty(const pwm_level_t *pwm, uint8_t percent)
{
    const uint32_t duty = (uint32_t)(((uint64_t)pwm->full * percent) / PERCENT_MAX);
    return pwm->config.active_low ? pwm->full - duty : duty;
}

static xiaotai_err_t pwm_level_set(void *self, uint8_t percent)
{
    if (percent > PERCENT_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const pwm_level_t *pwm = self;
    return pwm->running ? xiaotai_bus_io_pwm_set(pwm->config.channel, pwm_duty(pwm, percent))
                        : XIAOTAI_ERR_INVALID_STATE;
}

static xiaotai_err_t pwm_enable(void *self, bool active)
{
    pwm_level_t *pwm = self;
    if (pwm->running == active) return XIAOTAI_OK;
    const xiaotai_err_t err = active ? xiaotai_bus_io_pwm_setup(&pwm->config, pwm_duty(pwm, 0))
        : xiaotai_bus_io_pwm_stop(pwm->config.channel, pwm->config.active_low);
    if (err == XIAOTAI_OK) pwm->running = active;
    return err;
}

xiaotai_err_t xiaotai_level_pwm_managed(const xiaotai_pwm_config_t *config, xiaotai_level_t *level,
                                         xiaotai_line_t *enable)
{
    if (level) *level = (xiaotai_level_t){0};
    if (enable) *enable = (xiaotai_line_t){0};
    if (!config || !level || !enable || config->pin < 0 || config->hz == 0 || config->resolution_bits == 0 ||
        config->resolution_bits > PWM_BITS_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    pwm_level_t *pwm = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*pwm));
    if (!pwm) return XIAOTAI_ERR_NO_MEM;
    pwm->full = (1UL << config->resolution_bits) - 1U;
    pwm->config = *config;
    *level = (xiaotai_level_t){.set = pwm_level_set, .dimmable = true, .self = pwm};
    *enable = (xiaotai_line_t){.set = pwm_enable, .self = pwm};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_level_pwm(const xiaotai_pwm_config_t *config, xiaotai_level_t *level)
{
    xiaotai_line_t enable;
    xiaotai_err_t err = xiaotai_level_pwm_managed(config, level, &enable);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_line_set(&enable, true);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(enable.self);
        *level = (xiaotai_level_t){0};
    }
    return err;
}

static xiaotai_err_t line_level_set(void *self, uint8_t percent)
{
    if (percent > PERCENT_MAX) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_line_set(self, percent > 0);
}

void xiaotai_level_from_line(const xiaotai_line_t *line, xiaotai_level_t *level)
{
    if (!level) return;
    /* A NULL or zeroed line gives a level that checks its argument and
     * otherwise does nothing, like the line itself. */
    *level = (xiaotai_level_t){.set = line_level_set, .dimmable = false, .self = (void *)line};
}
