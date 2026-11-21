#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Debounced level button polled at a fixed period, for boards whose user button
 * is a plain GPIO. A press counts once, after the level has been stable for the
 * debounce time; a button already held at startup must be released first, so a
 * reset with the key held never fires an action. */
#define XIAOTAI_LEVEL_BUTTON_DEBOUNCE_MS 50U

typedef struct {
    bool initialized, stable, candidate, armed;
    uint32_t candidate_ms;
} xiaotai_level_button_t;

static inline bool xiaotai_level_button_poll(xiaotai_level_button_t *button, bool pressed,
                                             uint32_t period_ms)
{
    if (!button->initialized) {
        button->initialized = true;
        button->stable = pressed;
        button->candidate = pressed;
        button->armed = !pressed;
        button->candidate_ms = 0;
    }
    if (pressed != button->candidate) {
        button->candidate = pressed;
        button->candidate_ms = 0;
        return false;
    }
    if (button->candidate_ms < XIAOTAI_LEVEL_BUTTON_DEBOUNCE_MS) {
        button->candidate_ms += period_ms;
        return false;
    }
    if (button->stable == button->candidate) return false;
    button->stable = button->candidate;
    if (!button->stable) {
        button->armed = true;
        return false;
    }
    if (!button->armed) return false;
    button->armed = false;
    return true;
}
