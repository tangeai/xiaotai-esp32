// SPDX-License-Identifier: MIT
#include "power_io.h"

enum { AW_OUTPUT_0 = 0x02 };

typedef struct {
    struct xiaotai_aw9523 *chip;
    uint8_t reg, mask;
    bool active_low;
} aw_line_t;

struct xiaotai_aw9523 {
    power_io_t io;
    aw_line_t lines[2][8][2];
};

static xiaotai_err_t set_output(void *self, bool active)
{
    const aw_line_t *line = self;
    power_io_t *io = &line->chip->io;
    if (!xiaotai_mutex_lock(&io->lock, io->lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    const xiaotai_err_t err = power_io_update(io, line->reg, line->mask, active != line->active_low);
    xiaotai_mutex_unlock(&io->lock);
    return err;
}

static xiaotai_err_t get_output(void *self, bool *active)
{
    if (!active) return XIAOTAI_ERR_INVALID_ARG;
    const aw_line_t *line = self;
    power_io_t *io = &line->chip->io;
    *active = false;
    if (!xiaotai_mutex_lock(&io->lock, io->lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    uint8_t value = 0;
    const xiaotai_err_t err = power_io_read(io, line->reg, &value);
    if (err == XIAOTAI_OK) *active = ((value & line->mask) != 0) != line->active_low;
    xiaotai_mutex_unlock(&io->lock);
    return err;
}

xiaotai_err_t xiaotai_aw9523_create(const xiaotai_aw9523_config_t *config, xiaotai_aw9523_t **chip)
{
    if (chip) *chip = NULL;
    if (!config || !chip || !config->bus || config->address > POWER_ADDRESS_MAX ||
        (config->setup_count && !config->setup)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_aw9523_t *aw = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*aw));
    if (!aw) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = power_io_init(&aw->io, config->bus, config->address, 0);
    if (err == XIAOTAI_OK) {
        err = power_io_setup(&aw->io, config->setup, config->setup_count);
        if (err != XIAOTAI_OK) xiaotai_mutex_deinit(&aw->io.lock);
    }
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(aw);
        return err;
    }
    *chip = aw;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_aw9523_line(xiaotai_aw9523_t *chip, uint8_t port, uint8_t pin, bool active_low,
                                  xiaotai_line_t *line)
{
    if (line) *line = (xiaotai_line_t){0};
    if (!chip || !line || port > 1 || pin >= 8U) return XIAOTAI_ERR_INVALID_ARG;
    aw_line_t *output = &chip->lines[port][pin][active_low];
    *output = (aw_line_t){.chip = chip, .reg = (uint8_t)(AW_OUTPUT_0 + port),
        .mask = (uint8_t)(1U << pin), .active_low = active_low};
    power_io_track(&chip->io, output->reg);
    *line = (xiaotai_line_t){.set = set_output, .get = get_output, .self = output};
    return XIAOTAI_OK;
}

void xiaotai_aw9523_report(xiaotai_aw9523_t *chip, xiaotai_hal_write_fn_t write, void *ctx)
{
    if (chip) power_io_report(&chip->io, "AW9523", NULL, write, ctx);
}
