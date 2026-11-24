// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/interaction.h"
#include "xiaotai/port/os.h"
#include "xiaotai/view.h"

typedef struct {
    uint16_t click_ms, long_ms;
    uint8_t max_clicks;
    uint32_t flags;
    const char *profile;
} input_timing_t;

typedef struct {
    uint32_t at, hold_ms;
    uint8_t remaining;
    bool active, started, asserted;
} input_script_t;

typedef struct {
    const xiaotai_input_t *part;
    input_timing_t timing;
    input_script_t script;
    uint32_t began, released, number, level_at;
    int32_t level;
    uint8_t clicks;
    bool down, real_down, consumed, press_matched, pending_contact, contact_active;
    bool pending_level, sent_level;
} input_state_t;

typedef struct {
    const xiaotai_interaction_profile_t *profile;
    uint32_t id;
} input_layer_t;

typedef struct {
    const xiaotai_interaction_profile_t *profiles[XIAOTAI_INTERACTION_PROFILES_MAX];
    input_layer_t layers[XIAOTAI_INTERACTION_LAYERS_MAX];
    size_t profiles_used, layers_used;
    uint32_t next_layer;
} input_registry_t;

extern struct interaction_context {
    xiaotai_mutex_t lock;
    input_registry_t *registry;
    input_state_t *inputs;
    size_t input_count;
    xiaotai_args_t *rejections;
    size_t rejection_head, rejection_count;
    char application[32], context[XIAOTAI_INTERACTION_CONTEXT_MAX];
    bool started, enabled;
    xiaotai_interaction_stats_t stats;
    uint32_t last_poll, skipped, open_failures, began_lost, rejections_lost;
    xiaotai_err_t start_error;
} xt_input;

bool interaction_lock(void);
void interaction_unlock(void);
input_state_t *interaction_find(const char *role);
input_timing_t interaction_timing(const char *role);
void interaction_void(input_state_t *state);
bool interaction_began(input_state_t *state);
bool interaction_match(input_state_t *state, xiaotai_gesture_t gesture, uint8_t count, const char *text);
void interaction_flush_rejections(void); /* poll, lock held */
void interaction_recognize(input_state_t *state, uint32_t now, uint32_t events, const char *text, int32_t level);
void interaction_poll(uint32_t now);
uint32_t interaction_next_due(uint32_t due, uint32_t now);
xiaotai_err_t interaction_poll_start(void);
void interaction_poll_stop(void);
xiaotai_err_t interaction_commands_init(void);
xiaotai_err_t interaction_inject_hold(const char *role);
