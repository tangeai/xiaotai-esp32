// SPDX-License-Identifier: MIT
#include "xiaotai/parts/panel_ili9341.h"
#include "xiaotai/port/mem.h"
#include "esp_lcd_ili9341.h"

typedef struct {
    ili9341_vendor_config_t config;
    ili9341_lcd_init_cmd_t commands[];
} vendor_t;

static xiaotai_err_t prepare(const xiaotai_panel_spi_config_t *config, void **storage)
{
    vendor_t *v = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1,
        sizeof(*v) + config->init_count * sizeof(v->commands[0]));
    if (!v) return XIAOTAI_ERR_NO_MEM;
    for (size_t i = 0; i < config->init_count; ++i) {
        const xiaotai_panel_command_t *c = &config->init[i];
        v->commands[i] = (ili9341_lcd_init_cmd_t){.cmd = c->command, .data = c->data,
            .data_bytes = c->size, .delay_ms = c->delay_ms};
    }
    v->config.init_cmds = v->commands;
    v->config.init_cmds_size = (uint16_t)config->init_count;
    *storage = &v->config;
    return XIAOTAI_OK;
}

static xiaotai_err_t create(void *io, const void *dev, const xiaotai_panel_spi_config_t *config, void **out)
{
    (void)config;
    esp_lcd_panel_handle_t panel = NULL;
    const esp_err_t err = esp_lcd_new_panel_ili9341(io, dev, &panel);
    if (err == ESP_OK) *out = panel;
    return err;
}

const xiaotai_panel_spi_controller_t *xiaotai_panel_ili9341(void)
{
    static const xiaotai_panel_spi_controller_t controller = {
        .name = "ili9341", .create = create, .prepare_vendor = prepare,
    };
    return &controller;
}
