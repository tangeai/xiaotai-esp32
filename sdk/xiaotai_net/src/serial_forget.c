// SPDX-License-Identifier: MIT
#include "internal.h"

#if XIAOTAI_NET_CONSOLE_FORGET
#include "xiaotai/at_command.h"

static xiaotai_err_t xiaotai_net_console_forget_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    if (xiaotai_activity_current(NULL)) return xiaotai_at_fail(reply, XIAOTAI_ERR_BUSY, "ACTIVITY");
    return net_serial_write_result(reply, net_store_forget_restart(), false);
}

xiaotai_err_t xiaotai_net_console_forget_register(void)
{
    static const xiaotai_at_verb_t verb = {.word = "FORGET", .run = xiaotai_net_console_forget_run};
    return xiaotai_at_verbs_register("AT+NET", &verb, 1);
}
#endif
