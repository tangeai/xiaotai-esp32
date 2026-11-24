// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

#include <stdatomic.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"

typedef enum { ROLE_ANY, ROLE_LIGHT, ROLE_VIBRATION } role_kind_t;

static bool is_light(const xiaotai_output_t *part)
{
    return part->desc.kind == XIAOTAI_OUTPUT_LIGHT || part->desc.kind == XIAOTAI_OUTPUT_RGB_LIGHT;
}

static bool paints(const xiaotai_output_t *part)
{
    return (is_light(part) || part->desc.kind == XIAOTAI_OUTPUT_VIBRATOR) &&
           !xiaotai_hal_output_owner(part) && part->desc.pixels &&
           (part->desc.pixels == 1U ? part->set != NULL : part->write != NULL);
}

static const xiaotai_output_t *find_part(const char *role, role_kind_t kind, bool count_skipped)
{
    for (size_t i = 0; ; ++i) {
        const xiaotai_output_t *part = xiaotai_hal_output(i);
        if (!part) return NULL;
        const bool compatible = kind == ROLE_ANY || (kind == ROLE_LIGHT && is_light(part)) ||
                                (kind == ROLE_VIBRATION && part->desc.kind == XIAOTAI_OUTPUT_VIBRATOR);
        if (!role) {
            if (kind != ROLE_ANY && compatible && paints(part)) return part;
            continue;
        }
        if (!part->desc.role || strcmp(role, part->desc.role)) continue;
        if (compatible && paints(part)) return part;
        if (count_skipped) ++g_feedback.stats.skipped_outputs;
        return NULL;
    }
}

static fb_output_t *output_for(const xiaotai_output_t *part)
{
    if (!part) return NULL;
    for (size_t n = 0; n < g_feedback.output_count; ++n) {
        if (g_feedback.outputs[n].part == part) return &g_feedback.outputs[n];
    }
    if (g_feedback.output_count == FB_PARTS_MAX) {
        ++g_feedback.stats.skipped_outputs;
        return NULL;
    }
    fb_output_t *out = &g_feedback.outputs[g_feedback.output_count];
    if (part->desc.pixels > XIAOTAI_FEEDBACK_PIXELS_MAX) {
        out->expanded = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)part->desc.pixels * 3U);
        if (!out->expanded) {
            ++g_feedback.stats.skipped_outputs;
            return NULL;
        }
    }
    out->part = part;
    ++g_feedback.output_count;
    return out;
}

static void route(uint8_t *routes, const xiaotai_output_t *part, uint8_t entry)
{
    fb_output_t *output = output_for(part);
    if (!output) return;
    const size_t i = (size_t)(output - g_feedback.outputs);
    if (!routes[i]) routes[i] = entry;
}

static void resolve(uint8_t *routes, const char *light, bool use_light, bool vibration,
                     const xiaotai_feedback_output_t *extras, size_t count)
{
    if (use_light) route(routes, light ? find_part(light, ROLE_LIGHT, true) : g_feedback.first_light, FB_ROUTE_LIGHT);
    if (vibration) route(routes, g_feedback.first_vibrator, FB_ROUTE_VIBRATION);
    for (size_t n = 0; n < count; ++n) {
        if (extras[n].role) {
            route(routes, find_part(extras[n].role, ROLE_ANY, true), FB_ROUTE_EXTRA + n);
        }
    }
}

static bool pulses(const xiaotai_vibration_t *vibration)
{
    return vibration->on_ms && vibration->strength;
}

static bool effect_has_light(const xiaotai_feedback_effect_t *effect)
{
    return effect->duration_ms && (effect->pattern.kind != XIAOTAI_LIGHT_OFF || effect->light);
}

void fb_output_resolve(void)
{
    g_feedback.first_light = find_part(NULL, ROLE_LIGHT, false);
    g_feedback.first_vibrator = find_part(NULL, ROLE_VIBRATION, false);
    for (size_t n = 0; n < g_feedback.state_count; ++n) {
        fb_state_t *item = &g_feedback.states[n];
        const xiaotai_feedback_state_t *state = fb_state_row(n);
        resolve(item->routes, state->light, state->pattern.kind != XIAOTAI_LIGHT_OFF || state->light,
                pulses(&state->vibration), state->outputs, state->output_count);
    }
    for (size_t t = 0; t < g_feedback.table_count; ++t) {
        const xiaotai_feedback_table_t *table = g_feedback.tables[t];
        for (size_t n = 0; n < table->effect_count; ++n) {
            uint8_t routes[FB_PARTS_MAX] = {0};
            const xiaotai_feedback_effect_t *effect = &table->effects[n];
            resolve(routes, effect->light, effect_has_light(effect), pulses(&effect->vibration),
                    effect->outputs, effect->duration_ms ? effect->output_count : 0U);
        }
    }
}

static bool has_role(const xiaotai_output_t *part, const char *role)
{
    return role && part->desc.role && !strcmp(role, part->desc.role);
}

static uint8_t effect_route(const xiaotai_feedback_effect_t *effect, const xiaotai_output_t *part)
{
    if (effect_has_light(effect) && is_light(part) &&
        (effect->light ? has_role(part, effect->light) : part == g_feedback.first_light)) return FB_ROUTE_LIGHT;
    if (part == g_feedback.first_vibrator && pulses(&effect->vibration)) return FB_ROUTE_VIBRATION;
    if (effect->duration_ms) {
        for (size_t n = 0; n < effect->output_count; ++n) {
            if (has_role(part, effect->outputs[n].role)) return FB_ROUTE_EXTRA + n;
        }
    }
    return FB_ROUTE_NONE;
}

void fb_effect_output_locked(const xiaotai_feedback_effect_t *effect, uint32_t now)
{
    for (size_t n = 0; n < g_feedback.output_count; ++n) {
        fb_output_t *out = &g_feedback.outputs[n];
        const uint8_t route = effect_route(effect, out->part);
        if (!route) continue;
        out->effect = effect;
        out->effect_start_ms = now;
        out->effect_route = route;
    }
}

typedef struct {
    const xiaotai_light_pattern_t *pattern;
    const char *name;
    uint8_t strength;
    bool active;
} frame_t;

static bool vibration_frame(frame_t *frame, const xiaotai_vibration_t *v, uint32_t since, uint32_t now, bool state)
{
    if (!pulses(v)) return false;
    const uint32_t age = now - since;
    const uint32_t period = (uint32_t)v->on_ms + v->off_ms;
    const uint8_t repeats = v->repeat ? v->repeat : 1U;
    if ((!state || v->repeat) && age >= (uint32_t)(repeats - 1U) * period + v->on_ms) return false;
    frame->strength = age % period < v->on_ms ? v->strength : 0U;
    return true;
}

static bool effect_frame(fb_output_t *out, uint32_t now, frame_t *frame)
{
    if (!out->effect) return false;
    const xiaotai_feedback_effect_t *effect = out->effect;
    if (out->effect_route == FB_ROUTE_VIBRATION
        ? vibration_frame(frame, &effect->vibration, out->effect_start_ms, now, false)
        : (uint32_t)(now - out->effect_start_ms) < effect->duration_ms) {
        if (out->effect_route == FB_ROUTE_LIGHT) frame->pattern = &effect->pattern;
        else if (out->effect_route >= FB_ROUTE_EXTRA) {
            frame->pattern = &effect->outputs[out->effect_route - FB_ROUTE_EXTRA].pattern;
        }
        return true;
    }
    out->effect = NULL;
    return false;
}

static bool state_frame(size_t i, uint32_t now, frame_t *frame)
{
    const xiaotai_feedback_state_t *winner = NULL;
    for (size_t n = 0; n < g_feedback.state_count; ++n) {
        const fb_state_t *entry = &g_feedback.states[n];
        const uint8_t code = entry->routes[i];
        if (!code || !fb_state_active(n)) continue;
        const xiaotai_feedback_state_t *state = fb_state_row(n);
        if (winner && state->priority <= winner->priority) continue;
        frame_t next = {.name = state->name};
        if (code == FB_ROUTE_LIGHT) next.pattern = &state->pattern;
        else if (code == FB_ROUTE_VIBRATION) {
            if (!vibration_frame(&next, &state->vibration, g_feedback.vibration_since[n], now, true)) continue;
        } else {
            next.pattern = &state->outputs[code - FB_ROUTE_EXTRA].pattern;
        }
        winner = state;
        *frame = next;
    }
    return winner != NULL;
}

static xiaotai_err_t write_frame(fb_output_t *out, const frame_t *frame, uint32_t now)
{
    const xiaotai_output_t *part = out->part;
    const size_t pixels = part->desc.pixels;
    const size_t base = pixels < XIAOTAI_FEEDBACK_PIXELS_MAX ? pixels : XIAOTAI_FEEDBACK_PIXELS_MAX;
    if (frame->pattern) fb_pattern_rgb(frame->pattern, now, out->rgb, base);
    else memset(out->rgb, frame->strength, base * 3U);
    if (part->desc.kind != XIAOTAI_OUTPUT_RGB_LIGHT) {
        for (size_t i = 0; i < base * 3U; i += 3U) {
            uint8_t max = out->rgb[i] > out->rgb[i + 1U] ? out->rgb[i] : out->rgb[i + 1U];
            if (out->rgb[i + 2U] > max) max = out->rgb[i + 2U];
            out->rgb[i] = max;
            out->rgb[i + 1U] = out->rgb[i + 2U] = 0U;
        }
    }
    if (pixels == 1U) return part->set(part->self, out->rgb[0], out->rgb[1], out->rgb[2]);
    if (!out->expanded) return part->write(part->self, out->rgb, pixels);
    const size_t stride = base * 3U;
    for (size_t offset = 0; offset < pixels * 3U; offset += stride) {
        const size_t left = pixels * 3U - offset;
        memcpy(out->expanded + offset, out->rgb, left < stride ? left : stride);
    }
    return part->write(part->self, out->expanded, pixels);
}

bool fb_output_step(uint32_t now, uint32_t *due_ms)
{
    frame_t frame[FB_PARTS_MAX] = {{0}};
    bool write[FB_PARTS_MAX] = {false};
    bool active = false, any_write = false;
    fb_lock();
    if (!g_feedback.started) {
        fb_unlock();
        return false;
    }
    if (g_feedback.scheduled && (int32_t)(now - g_feedback.next_frame_ms) < 0) {
        *due_ms = g_feedback.next_frame_ms;
        fb_unlock();
        return true;
    }
    for (size_t i = 0; i < g_feedback.output_count; ++i) {
        fb_output_t *out = &g_feedback.outputs[i];
        const bool showing = effect_frame(out, now, &frame[i]) || state_frame(i, now, &frame[i]);
        write[i] = showing || out->was_active;
        any_write = any_write || write[i];
        frame[i].active = showing;
        active = active || showing;
    }
    if (any_write && g_feedback.have_frame && now - g_feedback.last_frame_ms < XIAOTAI_FEEDBACK_FRAME_MS) {
        g_feedback.scheduled = true;
        g_feedback.next_frame_ms = g_feedback.last_frame_ms + XIAOTAI_FEEDBACK_FRAME_MS;
        *due_ms = g_feedback.next_frame_ms;
        fb_unlock();
        return true;
    }
    for (size_t i = 0; i < g_feedback.output_count; ++i) {
        fb_output_t *out = &g_feedback.outputs[i];
        if (out->part == g_feedback.first_light) g_feedback.shown = frame[i].name;
        out->was_active = frame[i].active;
    }
    const bool late = g_feedback.scheduled && now - g_feedback.next_frame_ms > XIAOTAI_FEEDBACK_FRAME_MS;
    if (any_write && late) ++g_feedback.stats.late_frames;
    const uint32_t base = g_feedback.scheduled && !late ? g_feedback.next_frame_ms : now;
    g_feedback.next_frame_ms = base + XIAOTAI_FEEDBACK_FRAME_MS;
    g_feedback.scheduled = active;
    if (any_write) {
        g_feedback.last_frame_ms = now;
        g_feedback.have_frame = true;
    }
    *due_ms = g_feedback.next_frame_ms;
    fb_unlock();

    uint32_t failed = 0U;
    for (size_t i = 0; i < g_feedback.output_count; ++i) {
        if (write[i] && write_frame(&g_feedback.outputs[i], &frame[i], now) != XIAOTAI_OK) ++failed;
    }
    if (failed) {
        fb_lock();
        g_feedback.stats.light_errors += failed;
        fb_unlock();
    }
    return active;
}

#if XIAOTAI_FEEDBACK_TASK
static void worker(void *ctx)
{
    (void)ctx;
    for (;;) {
        fb_lock();
        const bool running = g_feedback.started;
        fb_unlock();
        if (!running) break;
        uint32_t due;
        uint32_t wait = XIAOTAI_WAIT_FOREVER;
        if (fb_output_step(fb_now(), &due)) {
            const int32_t remaining = (int32_t)(due - fb_now());
            wait = remaining > 0 ? (uint32_t)remaining : 0U;
        }
        (void)xiaotai_task_wait_notify(wait);
    }
    xiaotai_signal_give(&g_feedback.task_done);
}
#else
static void owner_timer(void *ctx)
{
    g_feedback.timer_id = 0U;
    fb_output_kick(ctx);
}
#endif

void fb_output_kick(void *ctx)
{
    (void)ctx;
    if (!g_feedback.ready) return;
#if XIAOTAI_FEEDBACK_TASK
    fb_lock();
    if (g_feedback.task_ready) xiaotai_task_notify(&g_feedback.task);
    fb_unlock();
#else
    fb_lock();
    g_feedback.timer_kick = false;
    const bool running = g_feedback.started && g_feedback.output_count;
    fb_unlock();
    if (!running || g_feedback.timer_id) return;
    uint32_t due;
    if (fb_output_step(fb_now(), &due)) {
        const int32_t remaining = (int32_t)(due - fb_now());
        const xiaotai_err_t err = xiaotai_timer_start(remaining > 0 ? (uint32_t)remaining : 0U,
                                                      owner_timer, NULL, &g_feedback.timer_id);
        if (err != XIAOTAI_OK) xiaotai_eventlog_warn("feedback", "timer.failed", 0U, err);
    }
#endif
}

xiaotai_err_t fb_output_request_locked(void)
{
    if (!g_feedback.started || !g_feedback.output_count || xiaotai_on_owner()) return XIAOTAI_OK;
#if XIAOTAI_FEEDBACK_TASK
    if (g_feedback.task_ready) xiaotai_task_notify(&g_feedback.task);
    return XIAOTAI_OK;
#else
    if (g_feedback.timer_kick) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_run(fb_output_kick, NULL);
    if (err == XIAOTAI_OK) g_feedback.timer_kick = true;
    return err;
#endif
}

void fb_output_start(void)
{
    if (!g_feedback.output_count) return;
#if XIAOTAI_FEEDBACK_TASK
    xiaotai_err_t err = xiaotai_signal_init(&g_feedback.task_done);
    if (err == XIAOTAI_OK) {
        const xiaotai_task_config_t config = {
            .name = "xt_feedback", .entry = worker, .stack_bytes = XIAOTAI_FEEDBACK_TASK_STACK_BYTES,
            .stack = XIAOTAI_MEM_BULK, .priority = XIAOTAI_FEEDBACK_TASK_PRIORITY, .core = XIAOTAI_CORE_ANY,
        };
        err = xiaotai_task_create(&config, &g_feedback.task);
        if (err == XIAOTAI_OK) {
            fb_lock();
            g_feedback.task_ready = true;
            xiaotai_task_notify(&g_feedback.task);
            fb_unlock();
            return;
        }
        xiaotai_signal_deinit(&g_feedback.task_done);
    }
    xiaotai_eventlog_warn("feedback", "task.failed", 0U, err);
#else
    fb_output_kick(NULL);
#endif
}

static void stop_on_owner(void *ctx)
{
#if !XIAOTAI_FEEDBACK_TASK
    if (g_feedback.timer_id) xiaotai_timer_cancel(g_feedback.timer_id);
    g_feedback.timer_id = 0U;
#endif
    fb_sound_reset();
    if (ctx) atomic_store((atomic_bool *)ctx, true);
}

void fb_output_stop(void)
{
    fb_lock();
    g_feedback.started = false;
#if XIAOTAI_FEEDBACK_TASK
    const bool running = g_feedback.task_ready;
    if (running) xiaotai_task_notify(&g_feedback.task);
#endif
    fb_unlock();
#if XIAOTAI_FEEDBACK_TASK
    if (running) {
        (void)xiaotai_signal_take(&g_feedback.task_done, XIAOTAI_WAIT_FOREVER);
        xiaotai_signal_deinit(&g_feedback.task_done);
    }
#endif
    if (xiaotai_on_owner()) {
        stop_on_owner(NULL);
        return;
    }
    /* Host reset may be called with the framework still running. Drain jobs
     * queued before reset, and cancel the owner timer on its own task. */
    atomic_bool stopped = false;
    xiaotai_err_t err;
    do {
        err = xiaotai_run(stop_on_owner, &stopped);
        if (err == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1U);
    } while (err == XIAOTAI_ERR_NO_MEM);
    if (err == XIAOTAI_OK) {
        while (!atomic_load(&stopped)) xiaotai_delay_ms(1U);
    } else fb_sound_reset(); /* The framework has already stopped. */
}
