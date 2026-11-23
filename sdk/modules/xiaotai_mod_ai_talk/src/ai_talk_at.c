// SPDX-License-Identifier: MIT
#include "ai_talk_internal.h"

#include <inttypes.h>
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"

static const char *choice(const char *const *values, size_t count, unsigned value)
{
    return value < count ? values[value] : "unknown";
}

static void show(const xiaotai_reply_t *out, const xiaotai_ai_talk_status_t *status, const char *prefix)
{
    static const char *const phases[] = {"idle", "connecting", "listening", "thinking", "speaking", "ending"};
    static const char *const origins[] = {"user", "wake", "other"};
    static const char *const ends[] = {"none", "local", "remote", "preempted", "system", "timeout",
                                      "refused", "unsupported", "network", "no_memory", "audio", "protocol"};
    static const char *const speakers[] = {"none", "user", "ai"};
    xiaotai_ai_talk_caption_t caption = {0};
    if (status->session && xiaotai_ai_talk_caption_read(&caption) && caption.session != status->session)
        caption = (xiaotai_ai_talk_caption_t){0};
    xiaotai_at_printf(out, "%sphase=%s gen=%" PRIu32 " link=%" PRIu32 " origin=%s end=%s"
                     " caption=%s revision=%" PRIu32 " paragraph=%" PRIu32 " final=%u truncated=%u",
                     prefix, choice(phases, sizeof(phases) / sizeof(phases[0]), (unsigned)status->phase),
                     status->session, status->link,
                     choice(origins, sizeof(origins) / sizeof(origins[0]), (unsigned)status->origin),
                     choice(ends, sizeof(ends) / sizeof(ends[0]), (unsigned)status->end),
                     choice(speakers, sizeof(speakers) / sizeof(speakers[0]), (unsigned)caption.speaker),
                     caption.revision, caption.paragraph, (unsigned)caption.final, (unsigned)caption.truncated);
}

static xiaotai_err_t at_show(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    show(out, &status, "+AI: ");
    return XIAOTAI_OK;
}

static xiaotai_err_t at_start(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    return xiaotai_ai_talk_start(NULL, NULL);
}

#define CONTROL(name) \
    static xiaotai_err_t at_##name(int argc, const char *const *argv, const xiaotai_reply_t *out) \
    { \
        (void)argc; \
        (void)argv; \
        (void)out; \
        xiaotai_ai_talk_status_t status; \
        xiaotai_ai_talk_status(&status); \
        return xiaotai_ai_talk_##name(status.session); \
    }
CONTROL(stop)
CONTROL(interrupt)
CONTROL(submit)
#undef CONTROL

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_ai_talk_status_t status;
    ai_talk_diag_t diag;
    xiaotai_ai_talk_status(&status);
    ai_talk_diag(&diag);
    show(out, &status, "");
    xiaotai_at_printf(out, "+AI-STATS: error=%d started=%" PRIu32 " completed=%" PRIu32 " failed=%" PRIu32
                     " stale_calls=%" PRIu32 " stale_commands=%" PRIu32 " tool_calls=%" PRIu32
                     " tool_unhandled=%" PRIu32 " video_extra=%" PRIu32 " commands_dropped=%" PRIu32
                     " events_lost=%" PRIu32 " token_capacity=%" PRIu32 " tool_reply_failures=%" PRIu32,
                     (int)status.error, status.started, status.completed, status.failed, status.stale_calls,
                     status.stale_commands, status.tool_calls, status.tool_unhandled, diag.video_extra,
                     diag.commands_dropped, diag.events_lost, diag.token_capacity, diag.tool_reply_failures);
}

xiaotai_err_t ai_talk_at_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_show},
        {.word = "START", .run = at_start},
        {.word = "STOP", .run = at_stop},
        {.word = "INTERRUPT", .run = at_interrupt},
        {.word = "SUBMIT", .run = at_submit},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+AI", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("ai_talk", diagnose) : err;
}
