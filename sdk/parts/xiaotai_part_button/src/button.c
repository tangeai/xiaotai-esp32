// SPDX-License-Identifier: MIT
#include "xiaotai/parts/button.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#define TAG "button"

uint32_t xiaotai_debounce_step(xiaotai_debounce_t *s, bool pressed, uint16_t period_ms, uint16_t debounce_ms)
{
    if (!s) return 0;
    if (!s->initialized) {
        *s = (xiaotai_debounce_t){.initialized = true, .stable = pressed, .candidate = pressed, .armed = !pressed};
        return 0;
    }
    if (s->candidate != pressed) {
        s->candidate = pressed;
        s->held_ms = 0;
    } else if (s->stable != pressed) {
        const uint32_t elapsed = (uint32_t)s->held_ms + period_ms;
        s->held_ms = elapsed > UINT16_MAX ? UINT16_MAX : (uint16_t)elapsed;
    }
    if (s->stable == pressed || s->held_ms < debounce_ms) return 0;
    s->stable = pressed;
    if (!s->armed) {
        /* A momentary button already held at startup has no matching press. */
        if (!pressed) s->armed = true;
        return 0;
    }
    return pressed ? XIAOTAI_INPUT_PRESSED : XIAOTAI_INPUT_RELEASED;
}

typedef struct {
    xiaotai_input_t part;
    xiaotai_line_t line;
    xiaotai_debounce_t debounce;
    uint16_t debounce_ms, release_ms;
    bool open;
} line_button_t;

static xiaotai_err_t open_button(void *self)
{
    line_button_t *button = self;
    bool pressed = false;
    const xiaotai_err_t err = xiaotai_line_get(&button->line, &pressed);
    button->open = err == XIAOTAI_OK;
    if (button->open) {
        const bool contact = button->part.desc.kind == XIAOTAI_INPUT_CONTACT;
        button->debounce = (xiaotai_debounce_t){.initialized = true, .stable = pressed && !contact,
            .candidate = pressed, .armed = !pressed || contact};
    }
    return err;
}

static uint32_t poll_button(void *self)
{
    line_button_t *button = self;
    if (!button->open) return 0;
    bool pressed = false;
    if (xiaotai_line_get(&button->line, &pressed) != XIAOTAI_OK) {
        /* A missing sample cannot prove that the new level stayed stable. */
        button->debounce.candidate = button->debounce.stable;
        button->debounce.held_ms = 0;
        return 0;
    }
    xiaotai_debounce_t *debounce = &button->debounce;
    /* Down again before the release counted, though it had held past the press debounce. */
    if (debounce->stable && !debounce->candidate && pressed && debounce->held_ms >= button->debounce_ms) {
        XIAOTAI_LOGI(TAG, "%s up %u ms, one press", button->part.desc.role,
                     (unsigned)debounce->held_ms + XIAOTAI_INPUT_POLL_MS);
    }
    return xiaotai_debounce_step(debounce, pressed, XIAOTAI_INPUT_POLL_MS,
                                 debounce->stable ? button->release_ms : button->debounce_ms);
}

xiaotai_err_t xiaotai_button_create(const xiaotai_button_config_t *config, const xiaotai_input_t **input)
{
    if (input) *input = NULL;
    if (!config || !input || !config->role || !*config->role || !config->line.get) return XIAOTAI_ERR_INVALID_ARG;
    line_button_t *button = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*button));
    if (!button) return XIAOTAI_ERR_NO_MEM;
    button->line = config->line;
    button->debounce_ms = config->debounce_ms ? config->debounce_ms : XIAOTAI_BUTTON_DEBOUNCE_MS;
    button->release_ms = config->release_ms ? config->release_ms : button->debounce_ms;
    button->part = (xiaotai_input_t){
        .desc = {.kind = config->contact ? XIAOTAI_INPUT_CONTACT : XIAOTAI_INPUT_BUTTON,
                 .role = config->role, .events = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED},
        .open = open_button, .poll = poll_button, .self = button};
    *input = &button->part;
    return XIAOTAI_OK;
}
