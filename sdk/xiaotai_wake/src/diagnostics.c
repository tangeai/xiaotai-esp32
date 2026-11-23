// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include "internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"

static void report(const xiaotai_reply_t *reply, const char *name)
{
    wake_diagnostics_t sample;
    wake_engine_diagnostics(&sample);
    const xiaotai_wake_status_t *s = &sample.status;
    const int64_t at_us = s->last_score_us > 0 ? s->last_score_us : 0;
    const int64_t age_us = at_us > 0 && sample.now_us >= at_us ? sample.now_us - at_us : 0;

    xiaotai_at_printf(reply, "+%s: ready=%d listening=%d init_error=%d threshold=%u frontend=%s"
        " fed=%" PRIu32 " dropped=%" PRIu32 " resets=%" PRIu32 " stale=%" PRIu32 " inference=%" PRIu32
        " raw_hits=%" PRIu32 " accepted=%" PRIu32 " wanted=%d wrong_rate=%" PRIu32
        " reject_stream=%" PRIu32 " reject_age=%" PRIu32 " reject_error=%" PRIu32
        " skipped_unarmed=%" PRIu32 " skipped_cooldown=%" PRIu32 " lost=%" PRIu32,
        name, (int)s->ready, (int)s->listening, (int)s->init_error, (unsigned)s->threshold_milli,
        sample.raw ? "aec-clean" : "aec-clean-agc", s->fed, s->dropped, s->resets, s->stale,
        s->inferences, s->raw_hits, s->detections, (int)s->wanted, s->wrong_rate, s->reject_stream,
        s->reject_age, s->reject_error, s->skipped_unarmed, s->skipped_cooldown, s->detections_lost);
    xiaotai_at_printf(reply, "+%s-SCORE: last=%u at_ms=%" PRId64 " age_ms=%" PRId64
        " avg_us=%" PRIu32 " max_us=%" PRIu32 " over_hop=%" PRIu32 " window_age_max_ms=%" PRIu32
        " features_max_us=%" PRIu32 " inference_max_us=%" PRIu32 " head_max_us=%" PRIu32
        " reused_frames=%" PRIu32 " computed_frames=%" PRIu32,
        name, (unsigned)s->last_score_milli, at_us / 1000, age_us / 1000, s->detect_avg_us, s->detect_max_us,
        s->detect_over_budget, s->window_age_max_ms, s->features_max_us, s->inference_max_us, s->head_max_us,
        s->reused_frames, s->computed_frames);
}

static void diagnose(const xiaotai_reply_t *reply)
{
    report(reply, "WAKE_ENGINE");
}

static xiaotai_err_t status_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    report(reply, "WAKE");
    return XIAOTAI_OK;
}

static xiaotai_err_t threshold_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)reply;
    uint32_t threshold;
    if (!xiaotai_at_parse_u32(argv[0], XIAOTAI_WAKE_THRESHOLD_MIN_MILLI, XIAOTAI_WAKE_THRESHOLD_MAX_MILLI,
                              &threshold)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_wake_set_threshold((uint16_t)threshold);
}

typedef struct {
    uint32_t remaining;
    bool started;
} wake_watch_t;

/* The wait belongs to the console stream, not the framework owner. */
static bool watch_next(void *ctx, const xiaotai_reply_t *reply)
{
    wake_watch_t *watch = ctx;
    if (watch->started) {
        xiaotai_delay_ms(1000);
    } else {
        xiaotai_firmware_t firmware = {0};
        xiaotai_firmware(&firmware);
        xiaotai_at_printf(reply, "+BUILD: elf_sha256=%s", firmware.elf_sha256[0] ? firmware.elf_sha256 : "-");
        if (xiaotai_at_stream_result(reply) != XIAOTAI_OK) return false;
        watch->started = true;
    }
    report(reply, "WAKE");
    return --watch->remaining > 0;
}

static xiaotai_err_t watch_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    uint32_t seconds = 20;
    if (argc && !xiaotai_at_parse_u32(argv[0], 5, 30, &seconds)) return XIAOTAI_ERR_INVALID_ARG;
    wake_watch_t *watch = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*watch));
    if (!watch) return XIAOTAI_ERR_NO_MEM;
    *watch = (wake_watch_t){.remaining = seconds + 1U};
    const xiaotai_at_stream_t stream = {.next = watch_next, .release = xiaotai_mem_free, .ctx = watch};
    const xiaotai_err_t error = xiaotai_at_stream(reply, &stream);
    if (error != XIAOTAI_OK) xiaotai_mem_free(watch);
    return error;
}

xiaotai_err_t wake_diagnostics_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = status_verb},
        {.word = "THRESHOLD", .min_args = 1, .max_args = 1, .usage = "<50..990>", .run = threshold_verb},
        {.word = "WATCH", .max_args = 1, .usage = "[<5..30>]", .run = watch_verb},
    };
    const xiaotai_err_t error = xiaotai_diag_register("wake_engine", diagnose);
    return error == XIAOTAI_OK ? xiaotai_at_verbs_register("AT+WAKE", verbs, sizeof(verbs) / sizeof(*verbs)) : error;
}
