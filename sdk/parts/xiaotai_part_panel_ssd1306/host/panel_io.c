// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "panel_fake.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool panel_allocated;
} fake_io_t;

static xt_oled_fake_t state;
static xiaotai_err_t errors[XT_OLED_OPS];

void xt_oled_fake_reset(void)
{
    state = (xt_oled_fake_t){0};
    memset(errors, 0, sizeof(errors));
}

void xt_oled_fake_fail(xt_oled_operation_t op, xiaotai_err_t error)
{
    errors[op] = error;
}

const xt_oled_fake_t *xt_oled_fake_state(void)
{
    return &state;
}

static xiaotai_err_t record(xt_oled_operation_t op)
{
    if (state.count < sizeof(state.log) / sizeof(state.log[0])) state.log[state.count++] = op;
    const xiaotai_err_t result = errors[op];
    errors[op] = XIAOTAI_OK;
    return result;
}

xiaotai_err_t xiaotai_ssd1306_probe(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms)
{
    const xiaotai_err_t err = record(XT_OLED_PROBE);
    state.probe_timeout_ms = timeout_ms;
    return err == XIAOTAI_OK ? xiaotai_i2c_probe(config->bus, config->address) : err;
}

xiaotai_err_t xiaotai_ssd1306_new_io(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms, void **out)
{
    const xiaotai_err_t err = record(XT_OLED_NEW_IO);
    state.config = *config;
    state.io_timeout_ms = timeout_ms;
    if (err != XIAOTAI_OK) return err;
    *out = calloc(1, sizeof(fake_io_t));
    return *out ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

xiaotai_err_t xiaotai_ssd1306_new_panel(void *io, uint8_t height, void **out)
{
    const xiaotai_err_t err = record(XT_OLED_NEW_PANEL);
    fake_io_t *fake = io;
    if (err != XIAOTAI_OK) return err;
    if (fake->panel_allocated || height != state.config.height) return XIAOTAI_ERR_INVALID_STATE;
    fake->panel_allocated = true;
    *out = io;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ssd1306_configure(void *panel, bool mirror_x, bool mirror_y, bool invert)
{
    const xiaotai_err_t err = record(XT_OLED_CONFIGURE);
    if (!((fake_io_t *)panel)->panel_allocated) return XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) return err;
    state.mirror_x = mirror_x;
    state.mirror_y = mirror_y;
    state.invert = invert;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ssd1306_draw(void *panel, uint8_t width, uint8_t height, const void *pixels)
{
    const xiaotai_err_t err = record(XT_OLED_DRAW);
    if (!((fake_io_t *)panel)->panel_allocated) return XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) return err;
    const size_t bytes = (size_t)width * height / 8;
    if (bytes > sizeof(state.frame)) return XIAOTAI_ERR_INVALID_SIZE;
    state.frame_bytes = bytes;
    memcpy(state.frame, pixels, bytes);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ssd1306_switch(void *panel, bool on)
{
    const xiaotai_err_t err = record(XT_OLED_SWITCH);
    if (!((fake_io_t *)panel)->panel_allocated) return XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) return err;
    state.on = on;
    return XIAOTAI_OK;
}

void xiaotai_ssd1306_native_handles(void *panel, void *io, xiaotai_display_handles_t *handles)
{
    (void)panel;
    (void)io;
    (void)handles; /* fake objects are not native handles */
}

xiaotai_err_t xiaotai_ssd1306_del_panel(void *panel)
{
    const xiaotai_err_t err = record(XT_OLED_DEL_PANEL);
    fake_io_t *fake = panel;
    if (!fake->panel_allocated) return XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) return err;
    fake->panel_allocated = false;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ssd1306_del_io(void *io)
{
    const xiaotai_err_t err = record(XT_OLED_DEL_IO);
    fake_io_t *fake = io;
    if (fake->panel_allocated) return XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) free(io);
    return err;
}
