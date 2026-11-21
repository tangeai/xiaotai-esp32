// SPDX-License-Identifier: MIT
#include "environment_internal.h"
#include "xiaotai/at_command.h"
#include <inttypes.h>
#include <string.h>

static void show(const xiaotai_reply_t *reply, bool diagnostic)
{
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    const xiaotai_platform_environment_t *e = &status.environment;
    const char *saved = e->unconfirmed ? "?" : e->saved[0] ? e->saved : "default";
    const char *next = e->unconfirmed ? "?" : e->next[0] ? e->next : "-";
    const char *lead = diagnostic ? "+ENVIRONMENT-SAVED: saved=" : "+EP-SAVED: saved=";
    static const char middle[] = " next=";
    const bool split = strlen(lead) + strlen(saved) + sizeof(middle) - 1U + strlen(next) > XIAOTAI_AT_OUT_LINE_MAX;
    xiaotai_at_printf(reply, "%sactive=%s phase=%d error=%d boot_error=%d cancel=%d write_attempted=%d",
                      diagnostic ? "" : "+EP: ", e->active[0] ? e->active : "-", (int)status.phase,
                      (int)status.error, (int)e->boot_error,
                      (int)status.cancel_requested, (int)status.write_attempted);
    if (!split) {
        xiaotai_at_printf(reply, "%s%s%s%s", lead, saved, middle, next);
    } else {
        /* Each snapshot URL fits on its own, even with the diagnostic prefix. */
        xiaotai_at_printf(reply, "%s%s", lead, saved);
        xiaotai_at_printf(reply, "%s%s", diagnostic ? "+ENVIRONMENT-NEXT: next=" : "+EP-NEXT: next=", next);
    }
}

static xiaotai_err_t at_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    show(reply, false);
    return XIAOTAI_OK;
}

static xiaotai_err_t at_set(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)reply;
    return xiaotai_environment_set(argv[0]);
}

static xiaotai_err_t at_reset(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_environment_set(NULL);
}

static xiaotai_err_t at_cancel(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_environment_cancel();
}

static void on_result(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event->data || event->size != sizeof(xiaotai_environment_status_t)) return;
    const xiaotai_environment_status_t *status = event->data;
    const char *result;
    switch (event->args.value) {
    case XIAOTAI_ENVIRONMENT_SAVED: result = "saved"; break;
    case XIAOTAI_ENVIRONMENT_FAILED: result = "failed"; break;
    case XIAOTAI_ENVIRONMENT_CANCELLED: result = "cancelled"; break;
    default: return;
    }
    (void)xiaotai_at_notifyf("+EP: result=%s error=%d", result, (int)status->error);
}

static void diag(const xiaotai_reply_t *reply)
{
    show(reply, true);
    xiaotai_environment_stats_t stats;
    xiaotai_environment_stats(&stats);
    xiaotai_at_printf(reply, "+ENVIRONMENT-STATS: requests=%" PRIu32 " refused_guard=%" PRIu32 " refused_busy=%" PRIu32
                      " saved=%" PRIu32 " failed=%" PRIu32 " cancelled=%" PRIu32 " wait_timeouts=%" PRIu32
                      " store_timeouts=%" PRIu32 " release_failures=%" PRIu32,
                      stats.requests, stats.refused_guard, stats.refused_busy, stats.saved, stats.failed,
                      stats.cancelled, stats.wait_timeouts, stats.store_timeouts, stats.release_failures);
}

xiaotai_err_t environment_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_show},
        {.word = "SET", .min_args = 1, .max_args = 1, .run = at_set, .usage = "<base>"},
        {.word = "RESET", .run = at_reset},
        {.word = "CANCEL", .run = at_cancel},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+EP", verbs, sizeof(verbs) / sizeof(verbs[0]));
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(ENVIRONMENT_TAG, diag);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ENVIRONMENT_RESULT, on_result, NULL);
    return err;
}
