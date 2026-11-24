// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

#include <string.h>
#include "xiaotai/framework.h"

fb_engine_t g_feedback;

uint32_t fb_now(void) { return (uint32_t)(xiaotai_time_us() / 1000); }
void fb_lock(void) { (void)xiaotai_mutex_lock(&g_feedback.lock, XIAOTAI_WAIT_FOREVER); }
void fb_unlock(void) { xiaotai_mutex_unlock(&g_feedback.lock); }

bool fb_state_active(size_t i)
{
    return (g_feedback.active[i / 8U] & (1U << (i % 8U))) != 0U;
}

const xiaotai_feedback_state_t *fb_state_row(size_t index)
{
    for (size_t t = 0; t < g_feedback.table_count; ++t) {
        const xiaotai_feedback_table_t *table = g_feedback.tables[t];
        if (index < table->state_count) return &table->states[index];
        index -= table->state_count;
    }
    return NULL;
}

static size_t state_index(const char *name)
{
    if (name) {
        for (size_t i = 0; i < g_feedback.state_count; ++i) {
            if (!strcmp(fb_state_row(i)->name, name)) return i;
        }
    }
    return g_feedback.state_count;
}

static const xiaotai_feedback_effect_t *effect_find(const char *name)
{
    if (!name) return NULL;
    for (size_t t = 0; t < g_feedback.table_count; ++t) {
        const xiaotai_feedback_table_t *table = g_feedback.tables[t];
        for (size_t i = 0; i < table->effect_count; ++i) {
            if (!strcmp(table->effects[i].name, name)) return &table->effects[i];
        }
    }
    return NULL;
}

const char *fb_effect_name(const char *name)
{
    if (!g_feedback.ready) return NULL;
    fb_lock();
    const xiaotai_feedback_effect_t *row = effect_find(name);
    fb_unlock();
    return row ? row->name : NULL;
}

static bool valid_entry(const char *name, const xiaotai_feedback_output_t *outputs, size_t count)
{
    return name && *name && count <= XIAOTAI_FEEDBACK_OUTPUTS_MAX && (!count || outputs);
}

static bool registered(const char *name)
{
    return state_index(name) < g_feedback.state_count || effect_find(name);
}

static void effect_event(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    const xiaotai_feedback_effect_t *effect = ctx;
    /* If registration failed part-way, its hooks cannot fire an unregistered
     * row or another row that happens to have the same name. */
    if (effect_find(effect->name) == effect) (void)xiaotai_feedback_fire(effect->name, NULL);
}

xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table)
{
    if (!table || (table->state_count && !table->states) || (table->effect_count && !table->effects)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!g_feedback.ready || g_feedback.started) return XIAOTAI_ERR_INVALID_STATE;
    if (g_feedback.table_count == XIAOTAI_FEEDBACK_TABLES_MAX ||
        table->state_count > XIAOTAI_FEEDBACK_STATES_MAX - g_feedback.state_count) return XIAOTAI_ERR_NO_MEM;
    for (size_t i = 0; i < table->state_count; ++i) {
        const xiaotai_feedback_state_t *row = &table->states[i];
        if (!valid_entry(row->name, row->outputs, row->output_count)) return XIAOTAI_ERR_INVALID_ARG;
        if (registered(row->name)) return XIAOTAI_ERR_INVALID_STATE;
        for (size_t n = 0; n < i; ++n) {
            if (!strcmp(row->name, table->states[n].name)) return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    for (size_t i = 0; i < table->effect_count; ++i) {
        const xiaotai_feedback_effect_t *row = &table->effects[i];
        if (!valid_entry(row->name, row->outputs, row->output_count)) return XIAOTAI_ERR_INVALID_ARG;
        if (registered(row->name)) return XIAOTAI_ERR_INVALID_STATE;
        for (size_t n = 0; n < table->state_count; ++n) {
            if (!strcmp(row->name, table->states[n].name)) return XIAOTAI_ERR_INVALID_STATE;
        }
        for (size_t n = 0; n < i; ++n) {
            if (!strcmp(row->name, table->effects[n].name)) return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    for (size_t i = 0; i < table->effect_count; ++i) {
        const xiaotai_feedback_effect_t *row = &table->effects[i];
        if (row->event) {
            const xiaotai_err_t err = xiaotai_event_subscribe(row->event, effect_event, (void *)row);
            if (err != XIAOTAI_OK) return err;
        }
    }
    if (!g_feedback.vibration_since) {
        for (size_t i = 0; i < table->state_count; ++i) {
            const xiaotai_vibration_t *v = &table->states[i].vibration;
            if (v->strength && v->on_ms) {
                g_feedback.vibration_since = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_FEEDBACK_STATES_MAX,
                                                               sizeof(*g_feedback.vibration_since));
                if (!g_feedback.vibration_since) return XIAOTAI_ERR_NO_MEM;
                break;
            }
        }
    }
    g_feedback.state_count += table->state_count;
    g_feedback.tables[g_feedback.table_count++] = table;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_engine_feedback_init(void)
{
    if (g_feedback.ready) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xiaotai_mutex_init(&g_feedback.lock);
    if (err != XIAOTAI_OK) return err;
    g_feedback.tables = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_FEEDBACK_TABLES_MAX, sizeof(*g_feedback.tables));
    g_feedback.states = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_FEEDBACK_STATES_MAX, sizeof(*g_feedback.states));
    if (!g_feedback.tables || !g_feedback.states) {
        xiaotai_mem_free(g_feedback.tables);
        xiaotai_mem_free(g_feedback.states);
        xiaotai_mutex_deinit(&g_feedback.lock);
        memset(&g_feedback, 0, sizeof(g_feedback));
        return XIAOTAI_ERR_NO_MEM;
    }
    g_feedback.ready = true;
    fb_sound_init();
    err = fb_commands_register();
    if (err == XIAOTAI_OK) err = fb_default_player_init();
    if (err != XIAOTAI_OK) xiaotai_feedback_reset();
    return err;
}

void xiaotai_engine_feedback_start(void)
{
    if (!g_feedback.ready) return;
    fb_lock();
    if (g_feedback.started) {
        fb_unlock();
        return;
    }
    fb_output_resolve();
    g_feedback.started = true;
    fb_unlock();
    fb_output_start();
    fb_sound_state_changed();
}

void xiaotai_feedback_reset(void)
{
    if (!g_feedback.ready) return;
    fb_output_stop();
    for (size_t i = 0; i < g_feedback.output_count; ++i) xiaotai_mem_free(g_feedback.outputs[i].expanded);
    xiaotai_mem_free(g_feedback.tables);
    xiaotai_mem_free(g_feedback.states);
    xiaotai_mem_free(g_feedback.vibration_since);
    xiaotai_mutex_deinit(&g_feedback.lock);
    memset(&g_feedback, 0, sizeof(g_feedback));
}

static void state_changed(void *ctx)
{
    fb_output_kick(ctx);
    fb_sound_state_changed();
}

xiaotai_err_t xiaotai_feedback_set(const char *name, bool active)
{
    if (!g_feedback.ready) return XIAOTAI_ERR_INVALID_STATE;
    fb_lock();
    const size_t i = state_index(name);
    if (i == g_feedback.state_count) {
        fb_unlock();
        return XIAOTAI_ERR_NOT_FOUND;
    }
    if (fb_state_active(i) == active) {
        fb_unlock();
        return XIAOTAI_OK;
    }
    const bool owner = xiaotai_on_owner();
    const xiaotai_feedback_state_t *row = fb_state_row(i);
    const xiaotai_err_t err = !owner && g_feedback.started && row->sound.name && *row->sound.name
                                  ? xiaotai_run(state_changed, NULL) : fb_output_request_locked();
    const bool started = g_feedback.started;
    if (err == XIAOTAI_OK) {
        const uint8_t mask = 1U << (i % 8U);
        if (active) {
            g_feedback.active[i / 8U] |= mask;
            if (g_feedback.vibration_since) g_feedback.vibration_since[i] = fb_now();
        } else {
            g_feedback.active[i / 8U] &= (uint8_t)~mask;
        }
    }
    fb_unlock();
    if (err == XIAOTAI_OK && owner && started) state_changed(NULL);
    return err;
}

bool xiaotai_feedback_is_set(const char *name)
{
    if (!g_feedback.ready) return false;
    fb_lock();
    const size_t i = state_index(name);
    const bool active = i < g_feedback.state_count && fb_state_active(i);
    fb_unlock();
    return active;
}

const char *xiaotai_feedback_shown(void)
{
    if (!g_feedback.ready) return NULL;
    fb_lock();
    const char *shown = g_feedback.shown;
    fb_unlock();
    return shown;
}

typedef struct {
    fb_prompt_t prompt;
    bool force;
} effect_job_t;

static void finish_fire(void *ctx)
{
    effect_job_t *job = ctx;
    /* Taking the output snapshot also synchronizes the queued fire with its
     * light commit on the caller task. */
    fb_lock();
    const bool started = g_feedback.started;
    fb_unlock();
    fb_output_kick(NULL);
    if (started) (void)fb_sound_fire(&job->prompt, job->force, true);
    else if (job->prompt.done) job->prompt.done(XIAOTAI_ERR_INVALID_STATE, job->prompt.ctx);
    xiaotai_mem_free(job);
}

xiaotai_err_t xiaotai_feedback_fire(const char *name, const xiaotai_feedback_options_t *options)
{
    if (options && (options->then_count >= XIAOTAI_FEEDBACK_SEQUENCE_MAX ||
                    (options->then_count && !options->then))) return XIAOTAI_ERR_INVALID_ARG;
    if (options) {
        for (size_t i = 0; i < options->then_count; ++i) {
            if (!options->then[i] || !*options->then[i]) return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    if (!g_feedback.ready) return XIAOTAI_ERR_INVALID_STATE;
    const bool owner = xiaotai_on_owner();
    fb_lock();
    const xiaotai_feedback_effect_t *effect = effect_find(name);
    if (!effect || !g_feedback.started) {
        fb_unlock();
        return effect ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_NOT_FOUND;
    }
    fb_prompt_t prompt = {.effect = effect, .requested_ms = fb_now(), .id = xiaotai_generation_next()};
    if (effect->sound.name && *effect->sound.name) prompt.names[prompt.count++] = effect->sound.name;
    if (options) {
        for (size_t i = 0; i < options->then_count; ++i) prompt.names[prompt.count++] = options->then[i];
        prompt.generation = options->generation;
        prompt.valid_ms = options->valid_ms;
        prompt.done = options->done;
        prompt.ctx = options->ctx;
    }
    const bool force = options && options->force;
    effect_job_t *job = NULL;
    xiaotai_err_t err;
    if (!owner && (prompt.count || prompt.done)) {
        job = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*job));
        if (!job) {
            fb_unlock();
            return XIAOTAI_ERR_NO_MEM;
        }
        *job = (effect_job_t){prompt, force};
        err = xiaotai_run(finish_fire, job);
    } else {
        err = fb_output_request_locked();
    }
    if (err == XIAOTAI_OK) {
        fb_effect_output_locked(effect, prompt.requested_ms);
        ++g_feedback.stats.fired;
    }
    fb_unlock();
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(job);
        return err;
    }
    if (owner) {
        fb_output_kick(NULL);
        return fb_sound_fire(&prompt, force, false);
    }
    return XIAOTAI_OK;
}

void xiaotai_feedback_stats(xiaotai_feedback_stats_t *out)
{
    if (!out) return;
    if (!g_feedback.ready) {
        *out = (xiaotai_feedback_stats_t){0};
        return;
    }
    fb_lock();
    *out = g_feedback.stats;
    fb_unlock();
}
