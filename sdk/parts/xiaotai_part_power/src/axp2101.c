// SPDX-License-Identifier: MIT
#include "power_io.h"
#include <stdatomic.h>
#include <stdio.h>

enum { AXP_IRQ_KEY = 0x49, AXP_KEY_SHORT = 0x08, AXP_KEY_MASK = 0x0c, AXP_KEY_DIVIDER = 5 };

struct xiaotai_axp2101 {
    power_io_t io;
    xiaotai_input_t key;
    uint8_t key_polls;
    bool key_open;
    _Atomic uint32_t key_errors;
};

typedef struct {
    xiaotai_axp2101_t *chip;
    xiaotai_axp2101_rail_t rail;
} rail_line_t;

static xiaotai_err_t set_rail(void *self, bool active)
{
    const rail_line_t *line = self;
    power_io_t *io = &line->chip->io;
    if (!xiaotai_mutex_lock(&io->lock, io->lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = XIAOTAI_OK;
    if (active && line->rail.voltage_reg)
        err = power_io_write(io, line->rail.voltage_reg, line->rail.voltage_code);
    if (err == XIAOTAI_OK) err = power_io_update(io, line->rail.enable_reg, line->rail.enable_mask, active);
    xiaotai_mutex_unlock(&io->lock);
    return err;
}

static xiaotai_err_t get_rail(void *self, bool *active)
{
    if (!active) return XIAOTAI_ERR_INVALID_ARG;
    const rail_line_t *line = self;
    power_io_t *io = &line->chip->io;
    *active = false;
    if (!xiaotai_mutex_lock(&io->lock, io->lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    uint8_t value = 0;
    const xiaotai_err_t err = power_io_read(io, line->rail.enable_reg, &value);
    if (err == XIAOTAI_OK) *active = (value & line->rail.enable_mask) == line->rail.enable_mask;
    xiaotai_mutex_unlock(&io->lock);
    return err;
}

xiaotai_err_t xiaotai_axp2101_create(const xiaotai_axp2101_config_t *config, xiaotai_axp2101_t **chip)
{
    if (chip) *chip = NULL;
    if (!config || !chip || !config->bus || config->address > POWER_ADDRESS_MAX ||
        (config->setup_count && !config->setup)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_axp2101_t *axp = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*axp));
    if (!axp) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = power_io_init(&axp->io, config->bus, config->address, 0);
    if (err == XIAOTAI_OK) {
        err = power_io_setup(&axp->io, config->setup, config->setup_count);
        if (err != XIAOTAI_OK) xiaotai_mutex_deinit(&axp->io.lock);
    }
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(axp);
        return err;
    }
    atomic_init(&axp->key_errors, 0);
    *chip = axp;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_axp2101_line(xiaotai_axp2101_t *chip, const xiaotai_axp2101_rail_t *rail,
                                   xiaotai_line_t *line)
{
    if (line) *line = (xiaotai_line_t){0};
    if (!chip || !rail || !line || !rail->enable_mask ||
        (rail->voltage_reg && rail->voltage_reg == rail->enable_reg)) return XIAOTAI_ERR_INVALID_ARG;
    rail_line_t *output = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*output));
    if (!output) return XIAOTAI_ERR_NO_MEM;
    *output = (rail_line_t){.chip = chip, .rail = *rail};
    power_io_track(&chip->io, rail->enable_reg);
    if (rail->voltage_reg) power_io_track(&chip->io, rail->voltage_reg);
    *line = (xiaotai_line_t){.set = set_rail, .get = get_rail, .self = output};
    return XIAOTAI_OK;
}

static xiaotai_err_t read_key(xiaotai_axp2101_t *chip, uint32_t lock_ms, uint8_t *pressed)
{
    *pressed = 0;
    power_io_t *io = &chip->io;
    if (!xiaotai_mutex_lock(&io->lock, lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    uint8_t value = 0;
    xiaotai_err_t err = power_io_read(io, AXP_IRQ_KEY, &value);
    value &= AXP_KEY_MASK;
    if (err == XIAOTAI_OK && value) err = power_io_write(io, AXP_IRQ_KEY, value);
    if (err == XIAOTAI_OK) *pressed = value;
    xiaotai_mutex_unlock(&io->lock);
    return err;
}

static xiaotai_err_t open_key(void *self)
{
    xiaotai_axp2101_t *chip = self;
    uint8_t stale;
    const xiaotai_err_t err = read_key(chip, chip->io.lock_ms, &stale);
    chip->key_polls = 0;
    chip->key_open = err == XIAOTAI_OK;
    return err;
}

static uint32_t poll_key(void *self)
{
    xiaotai_axp2101_t *chip = self;
    if (!chip->key_open || ++chip->key_polls < AXP_KEY_DIVIDER) return 0;
    chip->key_polls = 0;
    uint8_t pressed;
    if (read_key(chip, 0, &pressed) != XIAOTAI_OK) {
        atomic_fetch_add(&chip->key_errors, 1U);
        return 0;
    }
    return pressed & AXP_KEY_SHORT ? XIAOTAI_INPUT_CLICK : 0U;
}

xiaotai_err_t xiaotai_axp2101_power_key(xiaotai_axp2101_t *chip, const char *role, const xiaotai_input_t **input)
{
    if (input) *input = NULL;
    if (!chip || !role || !*role || !input) return XIAOTAI_ERR_INVALID_ARG;
    if (chip->key.desc.role) return XIAOTAI_ERR_INVALID_STATE; /* One owner of the latch. */
    chip->key = (xiaotai_input_t){.desc = {.kind = XIAOTAI_INPUT_BUTTON, .role = role, .events = XIAOTAI_INPUT_CLICK},
        .open = open_key, .poll = poll_key, .self = chip};
    power_io_track(&chip->io, AXP_IRQ_KEY);
    *input = &chip->key;
    return XIAOTAI_OK;
}

void xiaotai_axp2101_report(xiaotai_axp2101_t *chip, xiaotai_hal_write_fn_t write, void *ctx)
{
    if (!chip) return;
    char tail[24];
    (void)snprintf(tail, sizeof(tail), " key_errors=%lu", (unsigned long)atomic_load(&chip->key_errors));
    power_io_report(&chip->io, "AXP2101", tail, write, ctx);
}
