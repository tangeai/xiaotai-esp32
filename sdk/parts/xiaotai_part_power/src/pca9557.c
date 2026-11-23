// SPDX-License-Identifier: MIT
#include "power_io.h"

enum { PCA_INPUT = 0x00, PCA_OUTPUT = 0x01, PCA_CONFIG = 0x03 };

typedef struct {
    struct xiaotai_pca9557 *chip;
    uint8_t mask;
    bool active_low;
} pca_line_t;

struct xiaotai_pca9557 {
    power_io_t io;
    uint8_t outputs, shadow;
    pca_line_t lines[8][2];
};

static xiaotai_err_t set_output(void *self, bool active)
{
    const pca_line_t *line = self;
    xiaotai_pca9557_t *chip = line->chip;
    if (!xiaotai_mutex_lock(&chip->io.lock, chip->io.lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    const uint8_t value = active != line->active_low ? (uint8_t)(chip->shadow | line->mask) :
                                                     (uint8_t)(chip->shadow & (uint8_t)~line->mask);
    const xiaotai_err_t err = power_io_write(&chip->io, PCA_OUTPUT, value);
    if (err == XIAOTAI_OK) chip->shadow = value;
    xiaotai_mutex_unlock(&chip->io.lock);
    return err;
}

static xiaotai_err_t get_output(void *self, bool *active)
{
    if (!active) return XIAOTAI_ERR_INVALID_ARG;
    const pca_line_t *line = self;
    xiaotai_pca9557_t *chip = line->chip;
    *active = false;
    if (!xiaotai_mutex_lock(&chip->io.lock, chip->io.lock_ms)) return XIAOTAI_ERR_TIMEOUT;
    *active = ((chip->shadow & line->mask) != 0) != line->active_low;
    xiaotai_mutex_unlock(&chip->io.lock);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_pca9557_create(const xiaotai_pca9557_config_t *config, xiaotai_pca9557_t **chip)
{
    if (chip) *chip = NULL;
    if (!config || !chip || !config->bus || config->address > POWER_ADDRESS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_pca9557_t *pca = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*pca));
    if (!pca) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = power_io_init(&pca->io, config->bus, config->address, config->lock_timeout_ms);
    if (err == XIAOTAI_OK) {
        /* Safe levels must already be latched when direction becomes output. */
        err = power_io_write(&pca->io, PCA_OUTPUT, config->initial);
        if (err == XIAOTAI_OK) err = power_io_write(&pca->io, PCA_CONFIG, (uint8_t)~config->outputs);
        if (err != XIAOTAI_OK) xiaotai_mutex_deinit(&pca->io.lock);
    }
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(pca);
        return err;
    }
    pca->shadow = config->initial;
    pca->outputs = config->outputs;
    power_io_track(&pca->io, PCA_INPUT);
    power_io_track(&pca->io, PCA_OUTPUT);
    power_io_track(&pca->io, PCA_CONFIG);
    *chip = pca;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_pca9557_line(xiaotai_pca9557_t *chip, uint8_t pin, bool active_low, xiaotai_line_t *line)
{
    if (line) *line = (xiaotai_line_t){0};
    if (!chip || !line || pin >= 8U || !(chip->outputs & (1U << pin))) return XIAOTAI_ERR_INVALID_ARG;
    pca_line_t *output = &chip->lines[pin][active_low];
    *output = (pca_line_t){.chip = chip, .mask = (uint8_t)(1U << pin), .active_low = active_low};
    *line = (xiaotai_line_t){.set = set_output, .get = get_output, .self = output};
    return XIAOTAI_OK;
}

void xiaotai_pca9557_report(xiaotai_pca9557_t *chip, xiaotai_hal_write_fn_t write, void *ctx)
{
    if (chip) power_io_report(&chip->io, "PCA9557", NULL, write, ctx);
}
