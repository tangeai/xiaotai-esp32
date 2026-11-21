// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

static void pixel(uint8_t *rgb, xiaotai_rgb_t color, uint8_t level)
{
    rgb[0] = ((uint32_t)color.red * level + 127U) / 255U;
    rgb[1] = ((uint32_t)color.green * level + 127U) / 255U;
    rgb[2] = ((uint32_t)color.blue * level + 127U) / 255U;
}

void fb_pattern_rgb(const xiaotai_light_pattern_t *p, uint32_t now, uint8_t *rgb, size_t pixels)
{
    const uint32_t period = p->period_ms;
    const uint32_t phase = period ? now / period : 0U;
    const uint32_t offset = period ? now % period : 0U;
    uint8_t base = 0U;
    switch (p->kind) {
        case XIAOTAI_LIGHT_SOLID: base = p->level_max; break;
        case XIAOTAI_LIGHT_BREATHE: {
            const uint32_t half = period / 2U;
            const uint32_t rise = offset < half ? offset : period - offset;
            const int32_t span = (int32_t)p->level_max - p->level_min;
            base = half ? (uint8_t)(p->level_min + span * (int32_t)rise / (int32_t)half) : p->level_min;
            break;
        }
        case XIAOTAI_LIGHT_BLINK: base = (phase & 1U) ? p->level_max : p->level_min; break;
        case XIAOTAI_LIGHT_FLASHES:
            if (period && p->accent_ms && offset < 2U * p->accent_ms * p->count &&
                !((offset / p->accent_ms) & 1U)) base = p->level_max;
            break;
        default: break;
    }
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t level = base;
        xiaotai_rgb_t color = p->color;
        if (p->kind == XIAOTAI_LIGHT_CHASE && period) {
            const size_t behind = (phase % pixels + pixels - i) % pixels;
            if (behind == 0U) level = 255U;
            else {
                color = p->accent;
                level = behind == 1U ? 150U : behind == 2U ? 55U : 0U;
            }
        } else if (p->kind == XIAOTAI_LIGHT_ALTERNATE) {
            level = ((i + phase) & 1U) ? 0U : p->level_max;
        } else if (p->kind == XIAOTAI_LIGHT_BREATHE || p->kind == XIAOTAI_LIGHT_SOLID) {
            const bool dot = p->accent_mode == XIAOTAI_LIGHT_ACCENT_DOT && p->accent_ms &&
                             (now / p->accent_ms) % pixels == i;
            const bool center = p->accent_mode == XIAOTAI_LIGHT_ACCENT_CENTER && pixels >= 2U &&
                                (i == (pixels - 1U) / 2U || i == pixels / 2U);
            if (dot || center) {
                color = p->accent;
                level = p->accent_level ? p->accent_level : UINT8_MAX;
            }
        }
        pixel(&rgb[3U * i], color, level);
    }
}
