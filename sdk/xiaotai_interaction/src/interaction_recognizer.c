// SPDX-License-Identifier: MIT
#include "interaction_internal.h"

static bool state_input(const input_state_t *state)
{
    const xiaotai_input_kind_t kind = state->part->desc.kind;
    return kind == XIAOTAI_INPUT_CONTACT || kind == XIAOTAI_INPUT_SLIDER;
}

void interaction_void(input_state_t *state)
{
    if (state_input(state)) return;
    if (state->clicks || (state->down && !state->consumed) ||
        (state->script.active && !state->script.asserted)) ++xt_input.stats.invalidated;
    state->clicks = 0;
    state->consumed = state->down;
    state->press_matched = false;
    if (state->script.active) {
        if (state->script.asserted) state->script.remaining = 1;
        else state->script = (input_script_t){0};
    }
}

static void settle_clicks(input_state_t *state)
{
    if (state->clicks) (void)interaction_match(state, XIAOTAI_GESTURE_CLICK, state->clicks, NULL);
    state->clicks = 0;
}

static void long_if_due(input_state_t *state, uint32_t now)
{
    if (!state->down || state->consumed || now - state->began < state->timing.long_ms) return;
    (void)interaction_match(state, XIAOTAI_GESTURE_LONG_PRESS, 0, NULL);
    state->clicks = 0;
    state->consumed = true;
}

static void edge(input_state_t *state, uint32_t now, bool down)
{
    if (down) {
        if (state->down) return;
        if (state->clicks && now - state->released >= state->timing.click_ms) settle_clicks(state);
        state->down = true;
        state->began = now;
        state->consumed = !xt_input.enabled;
        if (state->consumed) return;
        if (!state->clicks) {
            state->timing = interaction_timing(state->part->desc.role);
            /* The receiver cannot guard a new gesture if its initial snapshot was never queued. */
            if (!interaction_began(state)) {
                state->consumed = true;
                return;
            }
        }
        state->press_matched = interaction_match(state, XIAOTAI_GESTURE_PRESS, 0, NULL);
    } else {
        if (!state->down) {
            ++xt_input.stats.orphan_releases;
            return;
        }
        long_if_due(state, now);
        state->down = false;
        if (!state->consumed) {
            state->released = now;
            ++state->clicks;
            if (state->clicks >= state->timing.max_clicks) settle_clicks(state);
        }
        if (state->press_matched) (void)interaction_match(state, XIAOTAI_GESTURE_RELEASE, 0, NULL);
        state->consumed = false;
        state->press_matched = false;
    }
}

static uint32_t scripted(input_script_t *script, uint32_t now)
{
    if (!script->active) return 0;
    if (!script->started || (!script->asserted && now - script->at >= 80U)) {
        script->started = script->asserted = true;
        script->at = now;
        return XIAOTAI_INPUT_PRESSED;
    }
    if (!script->asserted || now - script->at < script->hold_ms) return 0;
    script->asserted = false;
    script->at = now;
    if (!--script->remaining) script->active = false;
    return XIAOTAI_INPUT_RELEASED;
}

static void set_contact(input_state_t *state, bool down)
{
    state->down = state->contact_active = down;
    state->pending_contact = true;
}

static void do_contact(input_state_t *state, uint32_t now, uint32_t events)
{
    const uint32_t edges = events & (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED);
    uint32_t changes;
    if (edges) {
        /* A physical contact supersedes a synthetic pulse as soon as it moves. */
        state->script = (input_script_t){0};
        changes = edges;
        state->real_down = (edges & XIAOTAI_INPUT_RELEASED) == 0;
    } else {
        changes = scripted(&state->script, now);
    }
    if (changes & XIAOTAI_INPUT_PRESSED) set_contact(state, true);
    if (changes & XIAOTAI_INPUT_RELEASED) {
        if (xt_input.enabled && (changes & XIAOTAI_INPUT_PRESSED)) {
            state->number = xiaotai_generation_next();
            (void)interaction_match(state, XIAOTAI_GESTURE_ACTIVE, 0, NULL);
        }
        set_contact(state, false);
    }
    if (xt_input.enabled && state->pending_contact) {
        state->number = xiaotai_generation_next();
        (void)interaction_match(state, state->contact_active ? XIAOTAI_GESTURE_ACTIVE : XIAOTAI_GESTURE_INACTIVE, 0, NULL);
        state->pending_contact = false;
    }
}

static void do_level(input_state_t *state, uint32_t now, uint32_t events, int32_t level)
{
    if (events & XIAOTAI_INPUT_LEVEL) {
        state->level = level;
        state->pending_level = true;
    }
    if (xt_input.enabled && state->pending_level &&
        (!state->sent_level || now - state->level_at >= XIAOTAI_INTERACTION_LEVEL_MS)) {
        state->number = xiaotai_generation_next();
        (void)interaction_match(state, XIAOTAI_GESTURE_LEVEL, 0, NULL);
        state->pending_level = false;
        state->sent_level = true;
        state->level_at = now;
    }
}

void interaction_recognize(input_state_t *state, uint32_t now, uint32_t events, const char *text, int32_t level)
{
    if (state->part->desc.kind == XIAOTAI_INPUT_CONTACT) {
        do_contact(state, now, events);
        return;
    }
    if (state->part->desc.kind == XIAOTAI_INPUT_SLIDER) {
        do_level(state, now, events, level);
        return;
    }

    const uint32_t real = events & (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED);
    if (real) {
        if (state->script.active) {
            interaction_void(state);
            state->script = (input_script_t){0};
            if (!state->real_down) state->down = false;
        }
        if (real & XIAOTAI_INPUT_PRESSED) state->real_down = true;
        if (real & XIAOTAI_INPUT_RELEASED) state->real_down = false;
    }
    const uint32_t edges = real ? real : scripted(&state->script, now);
    if (edges & XIAOTAI_INPUT_PRESSED) edge(state, now, true);
    if (edges & XIAOTAI_INPUT_RELEASED) edge(state, now, false);
    long_if_due(state, now);
    if (!state->down && state->clicks && now - state->released >= state->timing.click_ms) settle_clicks(state);

    static const struct { uint32_t event; xiaotai_gesture_t gesture; uint8_t count; } ready[] = {
        {XIAOTAI_INPUT_CLICK, XIAOTAI_GESTURE_CLICK, 1}, {XIAOTAI_INPUT_DOUBLE_CLICK, XIAOTAI_GESTURE_CLICK, 2},
        {XIAOTAI_INPUT_LONG_PRESS, XIAOTAI_GESTURE_LONG_PRESS, 0}, {XIAOTAI_INPUT_TURN_UP, XIAOTAI_GESTURE_TURN_UP, 0},
        {XIAOTAI_INPUT_TURN_DOWN, XIAOTAI_GESTURE_TURN_DOWN, 0}, {XIAOTAI_INPUT_DATA, XIAOTAI_GESTURE_DATA, 0},
    };
    for (size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); ++i) {
        if (!(events & ready[i].event) || !xt_input.enabled) continue;
        state->number = xiaotai_generation_next();
        (void)interaction_match(state, ready[i].gesture, ready[i].count, text && ready[i].event == XIAOTAI_INPUT_DATA ? text : NULL);
    }
}
