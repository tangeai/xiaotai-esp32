// SPDX-License-Identifier: MIT
#include "binding_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/modules/binding_config.h"

/* The protocol uses an unquoted, whitespace-delimited record. Do not turn a
 * non-identifier received from the platform into extra fields or AT lines. */
static const char *token(const char *text, size_t max)
{
    for (size_t i = 0; i <= max; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (!c) return i ? text : "-";
        if (c < 0x21 || c > 0x7e || c == '=') break;
    }
    return "-";
}

static xiaotai_err_t at_status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)argv;
    xiaotai_binding_status_t st;
    xiaotai_binding_status(&st);
    xiaotai_at_printf(out,
        "+BIND: phase=%d signed=%u error=%d code=%ld verification=%s left=%lu announcing=%u device=%s client=%s gen=%lu",
        (int)st.phase, (unsigned)st.signed_rebinding, (int)st.error, (long)st.code,
        token(st.verification, XIAOTAI_BINDING_CODE_MAX), (unsigned long)st.seconds_left,
        (unsigned)st.announcing, token(st.device_id, XIAOTAI_IDENTITY_DEVICE_ID_MAX),
        token(st.client_id, XIAOTAI_IDENTITY_CLIENT_ID_MAX), (unsigned long)st.generation);
    return XIAOTAI_OK;
}

static xiaotai_err_t at_retry(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)argv; (void)out;
    return xiaotai_binding_retry();
}

static xiaotai_err_t at_replay(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)argv; (void)out;
    return xiaotai_binding_announce();
}

static void diagnostics(const xiaotai_reply_t *out)
{
    xiaotai_binding_status_t st;
    xiaotai_binding_stats_t n;
    xiaotai_binding_status(&st);
    xiaotai_binding_stats(&n);
    xiaotai_at_printf(out, "phase=%d signed=%u announcing=%u error=%d needed_first=%lu needed_rebinding=%lu "
        "attempts=%lu signed_attempts=%lu refused=%lu stale=%lu connect_timeouts=%lu grants=%lu "
        "stores=%lu store_failures=%lu confirms=%lu confirm_timeouts=%lu bound=%lu failed=%lu expired=%lu cancelled=%lu",
        (int)st.phase, (unsigned)st.signed_rebinding, (unsigned)st.announcing, (int)st.error,
        (unsigned long)n.needed_first, (unsigned long)n.needed_rebinding, (unsigned long)n.attempts,
        (unsigned long)n.attempts_signed, (unsigned long)n.reports_refused, (unsigned long)n.stale_code_retries,
        (unsigned long)n.connect_timeouts, (unsigned long)n.grants, (unsigned long)n.stores,
        (unsigned long)n.store_failures, (unsigned long)n.confirmations, (unsigned long)n.confirm_timeouts,
        (unsigned long)n.bound, (unsigned long)n.failed, (unsigned long)n.expired, (unsigned long)n.cancelled);
    xiaotai_at_printf(out, "announcements=%lu server=%lu announce_failed=%lu announce_busy=%lu forgets=%lu forget_failures=%lu",
        (unsigned long)n.announcements, (unsigned long)n.announcements_server, (unsigned long)n.announcements_failed,
        (unsigned long)n.announcements_busy, (unsigned long)n.forgets, (unsigned long)n.forget_failures);
    xiaotai_at_printf(out, "grants_ignored=%lu grants_malformed=%lu grants_mismatched=%lu guard_refusals=%lu "
        "auto_resumed=%lu prepare_failures=%lu prepare_timeouts=%lu prepare_late=%lu",
        (unsigned long)n.grants_ignored, (unsigned long)n.grants_malformed, (unsigned long)n.grants_mismatched,
        (unsigned long)n.guard_refusals, (unsigned long)n.auto_resumed, (unsigned long)n.prepare_failures,
        (unsigned long)n.prepare_timeouts, (unsigned long)n.prepare_late);
}

xiaotai_err_t binding_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_status},
        {.word = "RETRY", .run = at_retry},
        {.word = "REPLAY", .run = at_replay},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+BIND", verbs, sizeof(verbs) / sizeof(verbs[0]));
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(BINDING_LOG_SOURCE, diagnostics);
#if XIAOTAI_BINDING_DEV_COMMANDS
    if (err == XIAOTAI_OK) err = xiaotai_binding_at_dev_register();
#endif
    return err;
}
