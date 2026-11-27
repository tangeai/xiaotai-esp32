// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/feedback.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/os.h"

#define FB_PARTS_MAX 4U
#define FB_STATE_BITS_BYTES ((XIAOTAI_FEEDBACK_STATES_MAX + 7U) / 8U)

enum {
    FB_ROUTE_NONE,
    FB_ROUTE_LIGHT,
    FB_ROUTE_VIBRATION,
    FB_ROUTE_EXTRA,
};

typedef struct {
    uint8_t routes[FB_PARTS_MAX];
} fb_state_t;

typedef struct {
    const xiaotai_output_t *part;
    const xiaotai_feedback_effect_t *effect;
    uint32_t effect_start_ms;
    uint8_t effect_route;
    bool was_active;
    uint8_t rgb[XIAOTAI_FEEDBACK_PIXELS_MAX * 3U];
    uint8_t *expanded;
} fb_output_t;

typedef struct {
    bool ready, started, task_ready, timer_kick, scheduled, have_frame;
    xiaotai_mutex_t lock;
    xiaotai_signal_t task_done;
    xiaotai_task_t task;
    uint32_t timer_id, next_frame_ms, last_frame_ms;
    const xiaotai_feedback_table_t **tables;
    fb_state_t *states;
    size_t table_count, state_count, output_count;
    uint8_t active[FB_STATE_BITS_BYTES];
    /* Anchor for a finite or repeating vibration, not for light patterns. */
    uint32_t *vibration_since;
    const xiaotai_output_t *first_light, *first_vibrator;
    const char *shown;
    fb_output_t outputs[FB_PARTS_MAX];
    xiaotai_feedback_stats_t stats;
} fb_engine_t;

extern fb_engine_t g_feedback;

uint32_t fb_now(void);
void fb_lock(void);
void fb_unlock(void);
bool fb_state_active(size_t i);
const xiaotai_feedback_state_t *fb_state_row(size_t i);
const char *fb_effect_name(const char *name);

void fb_pattern_rgb(const xiaotai_light_pattern_t *pattern, uint32_t now, uint8_t *rgb, size_t pixels);
void fb_output_resolve(void); /* owner, before enabling the producer */
void fb_output_start(void);   /* owner */
void fb_output_stop(void);    /* reset after framework/client quiescence */
/* The caller holds the engine lock before applying its update. In the owner
 * form queue a kick before mutating, or return failure without mutation.
 * Owner calls run fb_output_kick after releasing the lock. */
xiaotai_err_t fb_output_request_locked(void);
void fb_output_kick(void *ctx);
/* Shared by both producers; computes one frame at most and returns the next
 * absolute due time, or false if the producer can sleep indefinitely. */
bool fb_output_step(uint32_t now, uint32_t *due_ms);
void fb_effect_output_locked(const xiaotai_feedback_effect_t *effect, uint32_t now);

typedef struct {
    const xiaotai_feedback_effect_t *effect;
    const char *names[XIAOTAI_FEEDBACK_SEQUENCE_MAX];
    xiaotai_feedback_done_fn_t done;
    void *ctx;
    uint32_t requested_ms, valid_ms, generation, id;
    uint8_t count;
} fb_prompt_t;

typedef struct {
    const char *loop, *playing, *waiting;
    uint8_t groups;
} fb_sound_info_t;

void fb_sound_init(void);
void fb_sound_install_default(const xiaotai_feedback_player_t *player);
void fb_sound_reset(void);
void fb_sound_state_changed(void);
xiaotai_err_t fb_sound_fire(const fb_prompt_t *prompt, bool force, bool queued);
void fb_sound_info(fb_sound_info_t *out);
xiaotai_err_t fb_commands_register(void);
xiaotai_err_t fb_default_player_init(void);
void fb_default_player_reset(void);
bool fb_default_player_stale(const xiaotai_feedback_player_t *player, uint32_t generation);
