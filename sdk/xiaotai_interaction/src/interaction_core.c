// SPDX-License-Identifier: MIT
#include "interaction_internal.h"

#include <string.h>
#include "xiaotai/port/mem.h"

struct interaction_context xt_input;

bool interaction_lock(void)
{
    return xt_input.registry && xiaotai_mutex_lock(&xt_input.lock, XIAOTAI_WAIT_FOREVER);
}

void interaction_unlock(void) { xiaotai_mutex_unlock(&xt_input.lock); }

static void copy_word(char *dest, size_t capacity, const char *src)
{
    size_t n = 0;
    if (src) {
        while (n < capacity - 1U && src[n]) ++n;
        while (n && ((unsigned char)src[n] & 0xc0U) == 0x80U) --n;
        memcpy(dest, src, n);
    }
    dest[n] = '\0';
}

static uint16_t click_time(const xiaotai_interaction_profile_t *profile)
{
    return profile->multi_click_ms ? profile->multi_click_ms : XIAOTAI_INTERACTION_MULTI_CLICK_MS;
}

static uint16_t long_time(const xiaotai_interaction_profile_t *profile)
{
    return profile->long_press_ms ? profile->long_press_ms : XIAOTAI_INTERACTION_LONG_PRESS_MS;
}

static bool valid_profile(const xiaotai_interaction_profile_t *profile)
{
    if (!profile || !profile->rules || !profile->rule_count || !profile->name || !profile->name[0] ||
        strlen(profile->name) >= sizeof(((xiaotai_interaction_input_t *)0)->profile) ||
        (profile->application && (!profile->application[0] || strlen(profile->application) >= sizeof(xt_input.application))) ||
        click_time(profile) >= long_time(profile)) return false;
    for (size_t i = 0; i < profile->rule_count; ++i) {
        const xiaotai_interaction_rule_t *rule = &profile->rules[i];
        if (rule->gesture > XIAOTAI_GESTURE_LEVEL || ((rule->gesture == XIAOTAI_GESTURE_CLICK) != (rule->count != 0)))
            return false;
    }
    return true;
}

/* Newest temporary layer, then the foreground application, then global rules. */
static const xiaotai_interaction_profile_t *profile_at(size_t index)
{
    const input_registry_t *registry = xt_input.registry;
    if (index < registry->layers_used) return registry->layers[registry->layers_used - 1U - index].profile;
    index -= registry->layers_used;
    if (index < registry->profiles_used) {
        const xiaotai_interaction_profile_t *p = registry->profiles[index];
        return p->application && strcmp(p->application, xt_input.application) == 0 ? p : NULL;
    }
    index -= registry->profiles_used;
    if (index >= registry->profiles_used) return NULL;
    const xiaotai_interaction_profile_t *p = registry->profiles[index];
    return p->application ? NULL : p;
}

static size_t profile_slots(void)
{
    return xt_input.registry->layers_used + xt_input.registry->profiles_used * 2U;
}

static bool role_matches(const xiaotai_interaction_rule_t *rule, const char *role)
{
    return !rule->role || strcmp(rule->role, role) == 0;
}

input_timing_t interaction_timing(const char *role)
{
    input_timing_t timing = {.click_ms = XIAOTAI_INTERACTION_MULTI_CLICK_MS,
                             .long_ms = XIAOTAI_INTERACTION_LONG_PRESS_MS};
    for (size_t i = 0; i < profile_slots(); ++i) {
        const xiaotai_interaction_profile_t *p = profile_at(i);
        for (size_t j = 0; p && j < p->rule_count; ++j) {
            const xiaotai_interaction_rule_t *r = &p->rules[j];
            if (!role_matches(r, role)) continue;
            if (!timing.profile) {
                timing.profile = p->name;
                timing.flags = p->flags;
                timing.click_ms = click_time(p);
                timing.long_ms = long_time(p);
            }
            if (r->gesture == XIAOTAI_GESTURE_CLICK && timing.max_clicks < r->count) timing.max_clicks = r->count;
        }
    }
    return timing;
}

input_state_t *interaction_find(const char *role)
{
    for (size_t i = 0; i < xt_input.input_count; ++i)
        if (strcmp(xt_input.inputs[i].part->desc.role, role) == 0) return &xt_input.inputs[i];
    return NULL;
}

bool interaction_began(input_state_t *state)
{
    xiaotai_args_t args = {.value = (int32_t)xiaotai_generation_next()};
    state->number = (uint32_t)args.value;
    copy_word(args.text, sizeof(args.text), state->part->desc.role);
    if (xiaotai_event_emit(XIAOTAI_EVENT_INPUT_BEGAN, &args, NULL, 0) == XIAOTAI_OK) return true;
    ++xt_input.began_lost;
    return false;
}

static void remove_rejection(void)
{
    xt_input.rejection_head = (xt_input.rejection_head + 1U) % xt_input.input_count;
    --xt_input.rejection_count;
}

void interaction_flush_rejections(void)
{
    while (xt_input.rejection_count) {
        const xiaotai_args_t *args = &xt_input.rejections[xt_input.rejection_head];
        const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, args, NULL, 0);
        if (err == XIAOTAI_ERR_NO_MEM) return;
        if (err != XIAOTAI_OK) ++xt_input.rejections_lost;
        remove_rejection();
    }
}

static void emit_rejection(const xiaotai_args_t *args)
{
    if (!xt_input.rejection_count) {
        const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, args, NULL, 0);
        if (err == XIAOTAI_OK) return;
        if (err != XIAOTAI_ERR_NO_MEM) {
            ++xt_input.rejections_lost;
            return;
        }
    }

    /* The failed intent is never replayed. Keep just its warning, including
     * the name copy: a temporary interaction layer can be popped meanwhile. */
    if (xt_input.rejection_count == xt_input.input_count) {
        ++xt_input.rejections_lost;
        remove_rejection();
    }
    const size_t tail = (xt_input.rejection_head + xt_input.rejection_count) % xt_input.input_count;
    xt_input.rejections[tail] = *args;
    ++xt_input.rejection_count;
}

bool interaction_match(input_state_t *state, xiaotai_gesture_t gesture, uint8_t count, const char *text)
{
    if (!xt_input.enabled) return false;
    const char *role = state->part->desc.role;
    ++xt_input.stats.gestures;
    for (size_t i = 0; i < profile_slots(); ++i) {
        const xiaotai_interaction_profile_t *p = profile_at(i);
        for (size_t j = 0; p && j < p->rule_count; ++j) {
            const xiaotai_interaction_rule_t *r = &p->rules[j];
            if (!role_matches(r, role) || r->gesture != gesture || r->count != count ||
                (r->context && strcmp(r->context, xt_input.context) != 0)) continue;
            if (r->event) {
                xiaotai_args_t args = {.value = (int32_t)state->number};
                copy_word(args.text, sizeof(args.text), role);
                (void)xiaotai_event_emit(r->event, &args, NULL, 0);
            }
            if (r->intent) {
                xiaotai_args_t args = {.value = gesture == XIAOTAI_GESTURE_LEVEL ? state->level : r->value};
                if (text) copy_word(args.text, sizeof(args.text), text);
                if (xiaotai_intent_post(r->intent, &args) == XIAOTAI_OK) {
                    ++xt_input.stats.posted;
                } else {
                    ++xt_input.stats.rejected;
                    args.value = (int32_t)state->number;
                    copy_word(args.text, sizeof(args.text), r->intent);
                    emit_rejection(&args);
                }
            }
            return true;
        }
    }
    ++xt_input.stats.unmatched;
    return false;
}

static void invalidate(uint32_t flag)
{
    for (size_t i = 0; i < xt_input.input_count; ++i)
        if (!flag || (xt_input.inputs[i].timing.flags & flag)) interaction_void(&xt_input.inputs[i]);
}

xiaotai_err_t xiaotai_interaction_register(const xiaotai_interaction_profile_t *profile)
{
    if (!valid_profile(profile)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_command_scope_t scope;
    if (xiaotai_command_scope_capture(&scope) != XIAOTAI_OK || !interaction_lock()) return XIAOTAI_ERR_INVALID_STATE;
    input_registry_t *reg = xt_input.registry;
    xiaotai_err_t error = XIAOTAI_ERR_INVALID_STATE;
    if (!xt_input.started) {
        error = XIAOTAI_ERR_NO_MEM;
        if (reg->profiles_used < XIAOTAI_INTERACTION_PROFILES_MAX) {
            reg->profiles[reg->profiles_used++] = profile;
            error = XIAOTAI_OK;
        }
    }
    interaction_unlock();
    return error;
}

xiaotai_err_t xiaotai_interaction_push(const xiaotai_interaction_profile_t *profile, uint32_t *id)
{
    if (!id || !valid_profile(profile)) return XIAOTAI_ERR_INVALID_ARG;
    if (!interaction_lock()) return XIAOTAI_ERR_INVALID_STATE;
    input_registry_t *reg = xt_input.registry;
    if (reg->layers_used == XIAOTAI_INTERACTION_LAYERS_MAX) {
        interaction_unlock();
        return XIAOTAI_ERR_NO_MEM;
    }
    for (bool duplicate = true; duplicate;) {
        duplicate = false;
        if (!++reg->next_layer) ++reg->next_layer;
        for (size_t i = 0; i < reg->layers_used; ++i) {
            if (reg->layers[i].id == reg->next_layer) duplicate = true;
        }
    }
    reg->layers[reg->layers_used++] = (input_layer_t){.profile = profile, .id = reg->next_layer};
    *id = reg->next_layer;
    invalidate(0);
    interaction_unlock();
    return XIAOTAI_OK;
}

void xiaotai_interaction_pop(uint32_t id)
{
    if (!id || !interaction_lock()) return;
    input_registry_t *reg = xt_input.registry;
    for (size_t i = 0; i < reg->layers_used; ++i) {
        if (reg->layers[i].id != id) continue;
        --reg->layers_used;
        for (size_t j = i; j < reg->layers_used; ++j) reg->layers[j] = reg->layers[j + 1U];
        invalidate(0);
        break;
    }
    interaction_unlock();
}

void xiaotai_interaction_set_context(const char *context)
{
    if (!interaction_lock()) return;
    char next[sizeof(xt_input.context)];
    copy_word(next, sizeof(next), context);
    if (strcmp(next, xt_input.context)) {
        memcpy(xt_input.context, next, strlen(next) + 1U);
        invalidate(XIAOTAI_INTERACTION_VOID_ON_CONTEXT);
    }
    interaction_unlock();
}

void xiaotai_interaction_invalidate(void)
{
    if (!interaction_lock()) return;
    invalidate(0);
    interaction_unlock();
}

void xiaotai_interaction_enable(bool enabled)
{
    if (!interaction_lock()) return;
    if (!enabled && xt_input.enabled) invalidate(0);
    xt_input.enabled = enabled;
    interaction_unlock();
}

static void interaction_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event || !interaction_lock()) return;
    if (strcmp(event->name, XIAOTAI_EVENT_FOREGROUND) == 0) {
        if (strcmp(xt_input.application, event->args.text) != 0) {
            copy_word(xt_input.application, sizeof(xt_input.application), event->args.text);
            invalidate(0);
        }
    } else {
        invalidate(XIAOTAI_INTERACTION_VOID_ON_VIEW);
    }
    interaction_unlock();
}

xiaotai_err_t xiaotai_engine_interaction_init(void)
{
    if (xt_input.registry) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xiaotai_mutex_init(&xt_input.lock);
    if (err != XIAOTAI_OK) return err;
    xt_input.registry = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*xt_input.registry));
    if (!xt_input.registry) {
        xiaotai_mutex_deinit(&xt_input.lock);
        return XIAOTAI_ERR_NO_MEM;
    }
    xt_input.enabled = true;
    if ((err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_FOREGROUND, interaction_event, NULL)) == XIAOTAI_OK)
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_VIEW_CHANGED, interaction_event, NULL);
    if (err == XIAOTAI_OK) err = interaction_commands_init();
    if (err != XIAOTAI_OK) xiaotai_interaction_reset();
    return err;
}

void xiaotai_engine_interaction_start(void)
{
    if (!interaction_lock()) return;
    if (xt_input.started) {
        interaction_unlock();
        return;
    }
    xt_input.started = true;
    if (xt_input.registry->profiles_used && xiaotai_hal_input(0)) {
        size_t capacity = 0;
        while (capacity < XIAOTAI_INTERACTION_INPUTS_MAX && xiaotai_hal_input(capacity)) ++capacity;
        xt_input.inputs = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, capacity, sizeof(*xt_input.inputs));
        if (xt_input.inputs)
            xt_input.rejections = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, capacity, sizeof(*xt_input.rejections));
        if (!xt_input.rejections) {
            xiaotai_mem_free(xt_input.inputs);
            xt_input.inputs = NULL;
            xt_input.start_error = XIAOTAI_ERR_NO_MEM;
        } else {
            const xiaotai_input_t *part;
            for (size_t i = 0; (part = xiaotai_hal_input(i)); ++i) {
                if (i >= capacity) {
                    ++xt_input.skipped;
                } else if (part->open(part->self) != XIAOTAI_OK) {
                    ++xt_input.open_failures;
                } else {
                    xt_input.inputs[xt_input.input_count++].part = part;
                }
            }
            if (xt_input.input_count) {
                xt_input.start_error = interaction_poll_start();
            } else {
                xiaotai_mem_free(xt_input.rejections);
                xt_input.rejections = NULL;
            }
        }
    }
    interaction_unlock();
}

void xiaotai_interaction_stats(xiaotai_interaction_stats_t *out)
{
    if (!out) return;
    *out = (xiaotai_interaction_stats_t){0};
    if (!interaction_lock()) return;
    *out = xt_input.stats;
    interaction_unlock();
}

xiaotai_err_t xiaotai_interaction_input(size_t index, xiaotai_interaction_input_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if (!interaction_lock()) return XIAOTAI_ERR_NOT_FOUND;
    if (index >= xt_input.input_count) {
        interaction_unlock();
        return XIAOTAI_ERR_NOT_FOUND;
    }
    const input_state_t *state = &xt_input.inputs[index];
    const xiaotai_input_desc_t *desc = &state->part->desc;
    const uint32_t edges = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED;
    *out = (xiaotai_interaction_input_t){.edge_reporting = (desc->events & edges) == edges,
        .down = state->down, .injected = state->script.active, .consumed = state->consumed,
        .clicks = state->clicks, .level = state->level};
    copy_word(out->role, sizeof(out->role), desc->role);
    copy_word(out->profile, sizeof(out->profile), interaction_timing(desc->role).profile);
    interaction_unlock();
    return XIAOTAI_OK;
}

void xiaotai_interaction_reset(void)
{
    if (!xt_input.registry) return;
    interaction_poll_stop();
    xiaotai_mem_free(xt_input.rejections);
    xiaotai_mem_free(xt_input.inputs);
    xiaotai_mem_free(xt_input.registry);
    xiaotai_mutex_deinit(&xt_input.lock);
    memset(&xt_input, 0, sizeof(xt_input));
}
