// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include <inttypes.h>
#include <stdio.h>

xiaotai_app_voice_terminal_config_t terminal_config;
terminal_state_t terminal;

xiaotai_err_t xiaotai_app_voice_terminal_configure(const xiaotai_app_voice_terminal_config_t *config)
{
    if (terminal.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (config && config->announce_repeat_s > 3600U) return XIAOTAI_ERR_INVALID_ARG;
    terminal_config = config ? *config : (xiaotai_app_voice_terminal_config_t){0};
    return XIAOTAI_OK;
}

void xiaotai_app_voice_terminal_status(xiaotai_app_voice_terminal_status_t *status)
{
    if (!status) return;
    *status = (xiaotai_app_voice_terminal_status_t){
        .ring_silenced = terminal.ring_silenced, .reset_pending = terminal.reset_pending,
        .gesture_session = terminal.gesture_session, .accepted = terminal.accepted,
        .refused = terminal.refused, .stale = terminal.stale, .blocked = terminal.blocked,
        .superseded = terminal.superseded,
    };
    if (terminal.light) (void)snprintf(status->shown, sizeof(status->shown), "%s", terminal.light);
}

static void report(const xiaotai_reply_t *out)
{
    char line[216];
    (void)snprintf(line, sizeof(line),
        "shown=%s ring_silenced=%u reset_pending=%u gesture_session=%" PRIu32
        " accepted=%" PRIu32 " refused=%" PRIu32 " stale=%" PRIu32 " blocked=%" PRIu32 " superseded=%" PRIu32,
        terminal.light ? terminal.light : "-", (unsigned)terminal.ring_silenced,
        (unsigned)terminal.reset_pending, terminal.gesture_session, terminal.accepted,
        terminal.refused, terminal.stale, terminal.blocked, terminal.superseded);
    out->write(out->ctx, line);
}

xiaotai_err_t xiaotai_app_voice_terminal_init(void)
{
    if (terminal.initialized) return XIAOTAI_ERR_INVALID_STATE;
    terminal.initialized = true;
    xiaotai_err_t err = terminal_tables_init();
    if (err == XIAOTAI_OK) err = terminal_key_init();
    if (err == XIAOTAI_OK) err = terminal_state_init();
    if (err == XIAOTAI_OK) err = terminal_reset_init();
    if (err == XIAOTAI_OK) err = terminal_binding_init();
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("app.voice_terminal", report);
    return err;
}

void xiaotai_app_voice_terminal_start(void)
{
    xiaotai_wechat_status_t call;
    xiaotai_wechat_status(&call);
    terminal_show(&call);
}

void xiaotai_app_voice_terminal_reset(void)
{
    terminal_reset_teardown();
    terminal_binding_teardown();
    terminal_config = (xiaotai_app_voice_terminal_config_t){0};
    terminal = (terminal_state_t){0};
}
