// SPDX-License-Identifier: MIT
#include "xiaotai/parts/output.h"
#include "xiaotai/port/mem.h"

typedef struct {
    xiaotai_output_t part;
    xiaotai_line_t line;
} line_output_t;

static xiaotai_err_t set_line(void *self, uint8_t red, uint8_t green, uint8_t blue)
{
    (void)green;
    (void)blue;
    const line_output_t *output = self;
    return xiaotai_line_set(&output->line, red != 0);
}

xiaotai_err_t xiaotai_output_line_create(const xiaotai_output_line_config_t *config, const xiaotai_output_t **output)
{
    if (output) *output = NULL;
    if (!config || !output || !config->role || !*config->role || !config->line.set ||
        (config->kind != XIAOTAI_OUTPUT_LIGHT && config->kind != XIAOTAI_OUTPUT_VIBRATOR &&
         config->kind != XIAOTAI_OUTPUT_RELAY)) return XIAOTAI_ERR_INVALID_ARG;
    line_output_t *line = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*line));
    if (!line) return XIAOTAI_ERR_NO_MEM;
    line->line = config->line;
    const xiaotai_err_t err = xiaotai_line_set(&line->line, config->initially_active);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(line);
        return err;
    }
    line->part = (xiaotai_output_t){.desc = {.kind = config->kind, .role = config->role, .pixels = 1},
        .set = set_line, .self = line};
    *output = &line->part;
    return XIAOTAI_OK;
}
