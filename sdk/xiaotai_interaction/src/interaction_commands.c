// SPDX-License-Identifier: MIT
#include "interaction_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include "xiaotai/at_command.h"

static void inputs(const xiaotai_reply_t *out, const xiaotai_interaction_stats_t *stats)
{
    xiaotai_interaction_input_t input;
    for (size_t i = 0; xiaotai_interaction_input(i, &input) == XIAOTAI_OK; ++i) {
        char suffix[32] = "";
        if (interaction_lock()) {
            if (i < xt_input.input_count && xt_input.inputs[i].part->desc.kind == XIAOTAI_INPUT_SLIDER)
                (void)snprintf(suffix, sizeof(suffix), " level=%" PRId32, input.level);
            interaction_unlock();
        }
        xiaotai_at_printf(out, "+KEY: role=%s max_gap_ms=%" PRIu32 " late_polls=%" PRIu32
            " raw=%u stable=%u clicks=%u profile=%s consumed=%u injected=%u%s",
            input.role, stats->max_gap_ms, stats->late_polls, (unsigned)input.down, (unsigned)input.down,
            (unsigned)input.clicks, input.profile[0] ? input.profile : "-", (unsigned)input.consumed,
            (unsigned)input.injected, suffix);
    }
}

static xiaotai_err_t at_show(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    inputs(out, &stats);
    return XIAOTAI_OK;
}

static xiaotai_err_t at_click(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    return xiaotai_interaction_inject(argc ? argv[0] : "main", 1, 0);
}

static xiaotai_err_t at_double(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    return xiaotai_interaction_inject(argc ? argv[0] : "main", 2, 0);
}

static xiaotai_err_t at_hold(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    return interaction_inject_hold(argc ? argv[0] : "main");
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_interaction_stats_t s;
    xiaotai_interaction_stats(&s);
    inputs(out, &s);
    uint32_t skipped = 0, open_failures = 0, began_lost = 0;
    xiaotai_err_t start_error = XIAOTAI_OK;
    if (interaction_lock()) {
        skipped = xt_input.skipped;
        open_failures = xt_input.open_failures;
        began_lost = xt_input.began_lost;
        start_error = xt_input.start_error;
        interaction_unlock();
    }
    xiaotai_at_printf(out, "+KEY-STATS: polls=%" PRIu32 " gestures=%" PRIu32 " posted=%" PRIu32
        " unmatched=%" PRIu32 " rejected=%" PRIu32 " invalidated=%" PRIu32 " orphan_releases=%" PRIu32
        " data_errors=%" PRIu32 " injected=%" PRIu32 " skipped=%" PRIu32 " open_failures=%" PRIu32
        " began_lost=%" PRIu32 " start_error=%d",
        s.polls, s.gestures, s.posted, s.unmatched, s.rejected, s.invalidated, s.orphan_releases,
        s.data_errors, s.injected, skipped, open_failures, began_lost, (int)start_error);
}

xiaotai_err_t interaction_commands_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_show},
        {.word = "CLICK", .max_args = 1, .usage = "[<role>]", .run = at_click},
        {.word = "DOUBLE", .max_args = 1, .usage = "[<role>]", .run = at_double},
        {.word = "HOLD", .max_args = 1, .usage = "[<role>]", .run = at_hold},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+KEY", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("interaction", diagnose) : err;
}
