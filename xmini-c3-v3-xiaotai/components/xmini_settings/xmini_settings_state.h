// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define XMINI_SETTINGS_DEBOUNCE_MS 1000

typedef struct {
    int volume, gain_db;
    bool guidance_enabled;
    unsigned generation;
} xmini_settings_write_t;

typedef struct {
    int volume, gain_db;
    int durable_volume, durable_gain_db;
    int writing_volume, writing_gain_db;
    bool guidance_enabled, durable_guidance_enabled, writing_guidance_enabled;
    bool loaded, durable_known, writing;
    int error;
    unsigned revision, generation, attempted_generation, writing_generation;
    int64_t deadline_ms;
} xmini_settings_state_t;

bool xmini_settings_values_valid(int volume, int gain_db);
bool xmini_settings_state_init(xmini_settings_state_t *state, int volume, int gain_db);
bool xmini_settings_state_schedule(xmini_settings_state_t *state, int volume, int gain_db, int64_t now_ms);
bool xmini_settings_state_schedule_guidance(xmini_settings_state_t *state, bool enabled, int64_t now_ms);
bool xmini_settings_state_pending(const xmini_settings_state_t *state);
bool xmini_settings_state_take(xmini_settings_state_t *state, int64_t now_ms, xmini_settings_write_t *write);
bool xmini_settings_state_finish(xmini_settings_state_t *state, unsigned generation, int error);
