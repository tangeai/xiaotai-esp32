// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <inttypes.h>
#include <stdio.h>

#include "xiaotai/at.h"

static const char *diagnostic_word(char *value)
{
    if (!value[0]) return "-";
    for (char *c = value; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) *c = '_';
    }
    return value;
}

static void diagnostics(const xiaotai_reply_t *reply)
{
    xiaotai_app_companion_status_t status;
    xiaotai_app_companion_status(&status);
    char line[192];
    const int n = snprintf(line, sizeof(line),
        "foreground=%u setup=%u error_held=%u style=%s page=%s context=%s pages=%" PRIu32
        " refused=%" PRIu32 " stale=%" PRIu32, (unsigned)status.foreground, (unsigned)status.setup,
        (unsigned)status.error_held, diagnostic_word(status.style), diagnostic_word(status.page),
        status.foreground ? diagnostic_word(status.context) : "-", status.pages, status.refused, status.stale);
    if (n > 0 && (size_t)n < sizeof(line)) reply->write(reply->ctx, line);
}

static xiaotai_err_t dismiss_at(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return companion_session_dismiss_result();
}

xiaotai_err_t companion_at_init(void)
{
    static const xiaotai_at_verb_t dismiss = {.word = "DISMISS", .run = dismiss_at};
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+CALL", &dismiss, 1);
    return err == XIAOTAI_OK ? xiaotai_diag_register("app.companion", diagnostics) : err;
}
