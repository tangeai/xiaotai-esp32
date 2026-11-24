// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

#include <inttypes.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"

static const char *word(const char *value) { return value ? value : "none"; }

static void report(const xiaotai_reply_t *reply)
{
    fb_sound_info_t info;
    fb_sound_info(&info);
    xiaotai_at_printf(reply, "+FEEDBACK: shown=%s loop=%s playing=%s waiting=%s groups=%02x",
                     word(xiaotai_feedback_shown()), word(info.loop), word(info.playing), word(info.waiting),
                     (unsigned)info.groups);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    xiaotai_at_printf(reply, "+FEEDBACK-STATS: fired=%" PRIu32 " played=%" PRIu32 " dropped_busy=%" PRIu32
        " dropped_group=%" PRIu32 " dropped_expired=%" PRIu32 " dropped_rank=%" PRIu32 " dropped_stale=%" PRIu32,
        stats.fired, stats.played, stats.dropped_busy, stats.dropped_group, stats.dropped_expired,
        stats.dropped_rank, stats.dropped_stale);
    xiaotai_at_printf(reply, "+FEEDBACK-STATS: replaced=%" PRIu32 " cancelled=%" PRIu32 " stale_tokens=%" PRIu32
        " loops=%" PRIu32 " player_errors=%" PRIu32 " light_errors=%" PRIu32 " late_frames=%" PRIu32
        " skipped_outputs=%" PRIu32,
        stats.replaced, stats.cancelled, stats.stale_tokens, stats.loops, stats.player_errors,
        stats.light_errors, stats.late_frames, stats.skipped_outputs);
}

static xiaotai_err_t status(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    report(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t states(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < g_feedback.state_count; ++i) {
        const xiaotai_feedback_state_t *row = fb_state_row(i);
        xiaotai_at_printf(reply, "+FEEDBACK-STATE: name=%s priority=%u active=%d", row->name,
                         (unsigned)row->priority, (int)xiaotai_feedback_is_set(row->name));
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t set(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)reply;
    bool active;
    return xiaotai_at_parse_flag(argv[1], &active) ? xiaotai_feedback_set(argv[0], active) : XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t fire(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    if (argc == 2 && strcmp(argv[1], "FORCE")) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_feedback_options_t opts = {.force = argc == 2};
    return xiaotai_feedback_fire(argv[0], &opts);
}

static xiaotai_err_t cancel(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_feedback_cancel(NULL, true);
}

static xiaotai_err_t group(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)reply;
    uint32_t index;
    bool enabled;
    if (!xiaotai_at_parse_u32(argv[0], 1, XIAOTAI_FEEDBACK_GROUPS_MAX, &index) ||
        !xiaotai_at_parse_flag(argv[1], &enabled)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_feedback_group_enable((uint8_t)index, enabled);
}

xiaotai_err_t fb_commands_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = status},
        {.word = "STATES", .run = states},
        {.word = "SET", .min_args = 2, .max_args = 2, .usage = "<state> 0|1", .run = set},
        {.word = "FIRE", .min_args = 1, .max_args = 2, .usage = "<effect> [FORCE]", .run = fire},
        {.word = "CANCEL", .run = cancel},
        {.word = "GROUP", .min_args = 2, .max_args = 2, .usage = "<n> 0|1", .run = group},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+FEEDBACK", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("feedback", report) : err;
}
