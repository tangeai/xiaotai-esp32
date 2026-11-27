// SPDX-License-Identifier: MIT
#include "xiaotai/parts/display.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include <stdatomic.h>

enum { OPEN_IDLE, OPEN_STARTING, OPEN_BINDING };

typedef struct {
    xiaotai_display_t public;
    xiaotai_display_config_t config;
    xiaotai_display_handles_t handles;
    xiaotai_touch_filter_state_t filter;
    uint16_t width, height;
    atomic_uint opens, open_failures, present_errors, touch_errors;
    atomic_int last_error;
    atomic_bool ready, latched;
    atomic_uchar opening;
} part_t;

static void count_error(part_t *p, atomic_uint *count, xiaotai_err_t error)
{
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    atomic_store_explicit(&p->last_error, error, memory_order_relaxed);
}

static bool can_rollback(const xiaotai_display_config_t *config, bool failed)
{
    return !failed || !config->stop_on_rollback_error;
}

static xiaotai_err_t start_display(void *self, xiaotai_display_handles_t *out, xiaotai_display_bind_fn bind,
                                  void *ctx)
{
    part_t *p = self;
    if (atomic_load_explicit(&p->latched, memory_order_acquire)) {
        if (out) *out = (xiaotai_display_handles_t){0};
        atomic_fetch_add_explicit(&p->open_failures, 1, memory_order_relaxed);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&p->ready, memory_order_acquire)) {
        bool released = false;
        if (bind) return bind(ctx, &p->handles, &released);
        if (out) *out = p->handles;
        return XIAOTAI_OK;
    }
    const xiaotai_display_config_t *config = &p->config;
    const bool check_start_cleanup = config->stop_on_rollback_error || config->latch_on_start_cleanup_error;
    xiaotai_display_handles_t handles = {0};
    void *touch_handle = NULL;
    size_t steps = 0;
    bool panel_started = false, touch_started = false, backlight_touched = false, cleanup_failed = false;
    xiaotai_err_t result;
    if (config->backlight_off_before_start && config->backlight.set) {
        backlight_touched = true;
        result = config->backlight.set(config->backlight.self, 0);
        if (result != XIAOTAI_OK) goto failed;
    }
    while (steps < config->power_steps) {
        /* A line write error does not prove that the line stayed unchanged. */
        const xiaotai_display_power_step_t *step = &config->power[steps++];
        result = xiaotai_line_set(&step->line, step->active);
        if (result != XIAOTAI_OK) goto failed;
        if (step->settle_ms) xiaotai_delay_ms(step->settle_ms);
    }
    if (config->touch_first) {
        result = config->touch.start(config->touch.self, &touch_handle);
        if (result != XIAOTAI_OK) {
            cleanup_failed = check_start_cleanup && config->touch.cleanup_failed &&
                             config->touch.cleanup_failed(config->touch.self);
            goto failed;
        }
        touch_started = true;
    }
    result = config->panel.start(config->panel.self, &handles);
    if (result != XIAOTAI_OK) {
        cleanup_failed = check_start_cleanup && config->panel.cleanup_failed &&
                         config->panel.cleanup_failed(config->panel.self);
        goto failed;
    }
    panel_started = true;
    if (config->desc.render == XIAOTAI_RENDER_SCANOUT) {
        for (size_t i = 0; i < config->desc.buffers; ++i) {
            if (!handles.framebuffers[i]) {
                result = XIAOTAI_ERR_INVALID_STATE;
                goto failed;
            }
        }
    }
    if (config->touch.start && !config->touch_first) {
        result = config->touch.start(config->touch.self, &touch_handle);
        if (result != XIAOTAI_OK) {
            cleanup_failed = check_start_cleanup && config->touch.cleanup_failed &&
                             config->touch.cleanup_failed(config->touch.self);
            goto failed;
        }
        touch_started = true;
    }
    if (config->backlight.set && !backlight_touched && !config->backlight_managed_in_power_steps) {
        backlight_touched = true;
        result = config->backlight.set(config->backlight.self, 0);
        if (result != XIAOTAI_OK) goto failed;
    }
    handles.touch = touch_handle;
    p->handles = handles;
    if (bind) {
        bool released = false;
        /* Only the serialized binder may present, without publishing ready. */
        atomic_store_explicit(&p->opening, OPEN_BINDING, memory_order_release);
        result = bind(ctx, &handles, &released);
        atomic_store_explicit(&p->opening, OPEN_STARTING, memory_order_release);
        if (result != XIAOTAI_OK) {
            if (!released) {
                cleanup_failed = true;
                goto report_failure;
            }
            p->handles = (xiaotai_display_handles_t){0};
            p->filter = (xiaotai_touch_filter_state_t){0};
            goto failed;
        }
    }
    atomic_fetch_add_explicit(&p->opens, 1, memory_order_relaxed);
    atomic_store_explicit(&p->ready, true, memory_order_release);
    if (out) *out = handles;
    return XIAOTAI_OK;

failed:
    if (backlight_touched && can_rollback(config, cleanup_failed) &&
        config->backlight.set(config->backlight.self, 0) != XIAOTAI_OK) cleanup_failed = true;
    /* A driver cleans up its own failed start. The display owns the reverse
     * order across drivers that did start successfully. */
    if (touch_started && !config->touch_first && can_rollback(config, cleanup_failed) &&
        config->touch.stop(config->touch.self) != XIAOTAI_OK) cleanup_failed = true;
    if (panel_started && can_rollback(config, cleanup_failed) &&
        config->panel.stop(config->panel.self) != XIAOTAI_OK) cleanup_failed = true;
    if (touch_started && config->touch_first && can_rollback(config, cleanup_failed) &&
        config->touch.stop(config->touch.self) != XIAOTAI_OK) cleanup_failed = true;
    const size_t explicit_steps = config->power_rollback_steps;
    const size_t rollback_steps = explicit_steps ? explicit_steps : steps;
    for (size_t i = 0; i < rollback_steps && can_rollback(config, cleanup_failed); ++i) {
        const size_t index = explicit_steps ? config->power_rollback[i] : steps - 1 - i;
        if (index >= steps) continue;
        const xiaotai_display_power_step_t *step = &config->power[index];
        if (xiaotai_line_set(&step->line, !step->active) != XIAOTAI_OK) cleanup_failed = true;
    }
report_failure:
    count_error(p, &p->open_failures, result);
    if (cleanup_failed) atomic_store_explicit(&p->latched, true, memory_order_release);
    if (out) *out = (xiaotai_display_handles_t){0};
    return result;
}

static xiaotai_err_t open_serialized(part_t *p, xiaotai_display_handles_t *out,
                                     xiaotai_display_bind_fn bind, void *ctx)
{
    if (!p->config.bind_before_commit) return start_display(p, out, bind, ctx);
    unsigned char expected = OPEN_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&p->opening, &expected, OPEN_STARTING,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        if (out) *out = (xiaotai_display_handles_t){0};
        return XIAOTAI_ERR_BUSY;
    }
    const xiaotai_err_t err = start_display(p, out, bind, ctx);
    atomic_store_explicit(&p->opening, OPEN_IDLE, memory_order_release);
    return err;
}

static xiaotai_err_t open(void *self, xiaotai_display_handles_t *out)
{
    return open_serialized(self, out, NULL, NULL);
}

static xiaotai_err_t open_bound(void *self, xiaotai_display_bind_fn bind, void *ctx)
{
    return bind ? open_serialized(self, NULL, bind, ctx) : XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t set_backlight(void *self, uint8_t percent)
{
    part_t *p = self;
    if (!atomic_load_explicit(&p->ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    if (percent > 100) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_level_t *level = &p->config.backlight;
    if (level->set)
        return level->set(level->self, level->dimmable || !percent ? percent : 100);
    return p->config.panel.set_on(p->config.panel.self, percent != 0);
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    part_t *p = self;
    if (!atomic_load_explicit(&p->ready, memory_order_acquire) &&
        atomic_load_explicit(&p->opening, memory_order_acquire) != OPEN_BINDING)
        return XIAOTAI_ERR_INVALID_STATE;
    if (!area || !pixels || !area->width || !area->height ||
        (uint32_t)area->x + area->width > p->width || (uint32_t)area->y + area->height > p->height)
        return XIAOTAI_ERR_INVALID_ARG;
    if ((p->public.desc.pixel == XIAOTAI_PIXEL_MONO1_VLSB || p->public.desc.render == XIAOTAI_RENDER_SCANOUT) &&
        (area->x || area->y || area->width != p->width || area->height != p->height))
        return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t result = p->config.panel.present(p->config.panel.self, area, pixels);
    if (result != XIAOTAI_OK) count_error(p, &p->present_errors, result);
    return result;
}

static xiaotai_err_t read_touch(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    part_t *p = self;
    if (!count || (capacity && !points)) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    if (!atomic_load_explicit(&p->ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    if (!capacity) return XIAOTAI_OK;
    const xiaotai_touch_t *touch = &p->config.touch;
    xiaotai_err_t result = touch->read(touch->self, points, capacity, count);
    if (result == XIAOTAI_OK && *count > capacity) result = XIAOTAI_ERR_INVALID_SIZE;
    if (result != XIAOTAI_OK) {
        *count = 0;
        count_error(p, &p->touch_errors, result);
        return result;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (points[i].x >= p->width) points[i].x = p->width - 1;
        if (points[i].y >= p->height) points[i].y = p->height - 1;
    }
    xiaotai_touch_filter_step(&p->config.touch_filter, &p->filter, xiaotai_time_us() / 1000,
                              *count ? points : NULL);
    return XIAOTAI_OK;
}

static bool descriptor_valid(const xiaotai_display_config_t *config)
{
    const xiaotai_display_desc_t *d = &config->desc;
    return d->width && d->height && d->rotation <= 3 &&
        (unsigned)d->pixel <= XIAOTAI_PIXEL_MONO1_VLSB && (unsigned)d->render <= XIAOTAI_RENDER_SCANOUT &&
        (d->render == XIAOTAI_RENDER_SCANOUT ? d->buffers >= 1 && d->buffers <= 2 : !d->buffers) &&
        (d->render != XIAOTAI_RENDER_PARTIAL || d->transfer_pixels != 0) &&
        (d->pixel != XIAOTAI_PIXEL_MONO1_VLSB || d->render == XIAOTAI_RENDER_FULL);
}

xiaotai_err_t xiaotai_display_create(const xiaotai_display_config_t *config, const xiaotai_display_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    if (!config || !descriptor_valid(config) || config->power_steps > XIAOTAI_DISPLAY_POWER_STEPS_MAX ||
        config->power_rollback_steps > XIAOTAI_DISPLAY_POWER_STEPS_MAX ||
        (config->backlight_off_before_start && config->backlight_managed_in_power_steps) ||
        !config->panel.start || !config->panel.stop || !config->panel.present)
        return XIAOTAI_ERR_INVALID_ARG;
    uint32_t seen = 0;
    for (size_t i = 0; i < config->power_rollback_steps; ++i) {
        const uint8_t step = config->power_rollback[i];
        if (step >= config->power_steps || (seen & (1U << step))) return XIAOTAI_ERR_INVALID_ARG;
        seen |= 1U << step;
    }
    const bool touch = config->touch.start != NULL;
    if (touch != (config->touch.stop != NULL) || touch != (config->touch.read != NULL) ||
        (config->touch_first && !touch)) return XIAOTAI_ERR_INVALID_ARG;
    part_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->config = *config;
    p->public = (xiaotai_display_t){.desc = config->desc, .self = p, .open = open, .present = present,
        .set_backlight = config->backlight.set || config->panel.set_on ? set_backlight : NULL,
        .read_touch = touch ? read_touch : NULL, .open_bound = config->bind_before_commit ? open_bound : NULL};
    p->public.desc.touch = touch;
    p->width = p->public.desc.rotation & 1 ? p->public.desc.height : p->public.desc.width;
    p->height = p->public.desc.rotation & 1 ? p->public.desc.width : p->public.desc.height;
    atomic_init(&p->opens, 0);
    atomic_init(&p->open_failures, 0);
    atomic_init(&p->present_errors, 0);
    atomic_init(&p->touch_errors, 0);
    atomic_init(&p->last_error, XIAOTAI_OK);
    atomic_init(&p->ready, false);
    atomic_init(&p->latched, false);
    atomic_init(&p->opening, OPEN_IDLE);
    *out = &p->public;
    return XIAOTAI_OK;
}

void xiaotai_display_part_stats(const xiaotai_display_t *display, xiaotai_display_part_stats_t *out)
{
    if (!out) return;
    *out = (xiaotai_display_part_stats_t){0};
    if (!display || display->open != open || !display->self) return;
    const part_t *p = display->self;
    *out = (xiaotai_display_part_stats_t){
        .open = atomic_load_explicit(&p->ready, memory_order_acquire),
        .latched = atomic_load_explicit(&p->latched, memory_order_acquire),
        .opens = atomic_load_explicit(&p->opens, memory_order_relaxed),
        .open_failures = atomic_load_explicit(&p->open_failures, memory_order_relaxed),
        .present_errors = atomic_load_explicit(&p->present_errors, memory_order_relaxed),
        .touch_errors = atomic_load_explicit(&p->touch_errors, memory_order_relaxed),
        .last_error = atomic_load_explicit(&p->last_error, memory_order_relaxed),
    };
}
