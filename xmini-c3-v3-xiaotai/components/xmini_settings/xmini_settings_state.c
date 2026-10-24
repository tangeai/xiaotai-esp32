// SPDX-License-Identifier: MIT
#include "xmini_settings_state.h"
#include <string.h>

bool xmini_settings_values_valid(int volume, int gain_db)
{
    return volume >= 0 && volume <= 100 && gain_db >= 0 && gain_db <= 36;
}

bool xmini_settings_state_init(xmini_settings_state_t *state, int volume, int gain_db)
{
    if (!state || !xmini_settings_values_valid(volume, gain_db)) return false;
    *state = (xmini_settings_state_t){.volume = volume, .gain_db = gain_db,
        .durable_volume = volume, .durable_gain_db = gain_db, .loaded = true,
        .guidance_enabled = true, .durable_guidance_enabled = true,
        .durable_known = true, .revision = state->revision + 1, .generation = 1};
    return true;
}

static bool dirty(const xmini_settings_state_t *state)
{
    return !state->durable_known || state->volume != state->durable_volume || state->gain_db != state->durable_gain_db ||
           state->guidance_enabled != state->durable_guidance_enabled;
}

bool xmini_settings_state_pending(const xmini_settings_state_t *state)
{
    return state && state->loaded && (state->writing || dirty(state));
}

static bool schedule(xmini_settings_state_t *state, int volume, int gain_db, bool guidance_enabled, int64_t now_ms)
{
    if (!state || !state->loaded || !xmini_settings_values_valid(volume, gain_db)) return false;
    bool changed = state->volume != volume || state->gain_db != gain_db || state->guidance_enabled != guidance_enabled;
    bool retry_failed = state->error && !state->writing;
    if (!changed && !retry_failed) return true;
    state->volume = volume;
    state->gain_db = gain_db;
    state->guidance_enabled = guidance_enabled;
    state->deadline_ms = now_ms + XMINI_SETTINGS_DEBOUNCE_MS;
    ++state->generation;
    ++state->revision;
    return true;
}

bool xmini_settings_state_schedule(xmini_settings_state_t *state, int volume, int gain_db, int64_t now_ms)
{
    return state && schedule(state, volume, gain_db, state->guidance_enabled, now_ms);
}

bool xmini_settings_state_schedule_guidance(xmini_settings_state_t *state, bool enabled, int64_t now_ms)
{
    return state && schedule(state, state->volume, state->gain_db, enabled, now_ms);
}

bool xmini_settings_state_take(xmini_settings_state_t *state, int64_t now_ms, xmini_settings_write_t *write)
{
    if (!state || !write || !state->loaded || state->writing || !dirty(state) ||
        state->attempted_generation == state->generation || now_ms < state->deadline_ms)
        return false;
    state->writing = true;
    state->writing_generation = state->generation;
    state->attempted_generation = state->generation;
    state->writing_volume = state->volume;
    state->writing_gain_db = state->gain_db;
    state->writing_guidance_enabled = state->guidance_enabled;
    *write = (xmini_settings_write_t){.volume = state->volume, .gain_db = state->gain_db,
        .guidance_enabled = state->guidance_enabled, .generation = state->generation};
    ++state->revision;
    return true;
}

bool xmini_settings_state_finish(xmini_settings_state_t *state, unsigned generation, int error)
{
    if (!state || !state->writing || state->writing_generation != generation) return false;
    state->writing = false;
    state->error = error;
    state->durable_known = error == 0;
    if (!error) {
        state->durable_volume = state->writing_volume;
        state->durable_gain_db = state->writing_gain_db;
        state->durable_guidance_enabled = state->writing_guidance_enabled;
    }
    ++state->revision;
    return true;
}
