// SPDX-License-Identifier: MIT
/* AT+NET PROVISION FORGET [FORCE]: start over with the stored credentials
 * erased. Compiled only with XIAOTAI_NET_CONSOLE_FORGET (development and
 * bench firmware), and every function is named xiaotai_provisioning_at_forget_*
 * so a build with the option off is checked after linking to contain none of
 * them (manifest "commands": symbol). */
#include <string.h>
#include "internal.h"
#include "xiaotai/at_command.h"

static xiaotai_err_t xiaotai_provisioning_at_forget_run(int argc, const char *const *argv,
                                                        const xiaotai_reply_t *reply)
{
    (void)reply;
    if (strcmp(argv[0], "FORGET") != 0) return XIAOTAI_ERR_INVALID_ARG;
    uint32_t flags = XIAOTAI_PROVISIONING_FORGET;
    if (argc == 2) {
        if (strcmp(argv[1], "FORCE") != 0) return XIAOTAI_ERR_INVALID_ARG;
        flags |= XIAOTAI_PROVISIONING_PREEMPT;
    }
    return prov_begin(flags, false);
}

static const xiaotai_at_verb_t xiaotai_provisioning_at_forget_verbs[] = {
    {.word = "PROVISION", .min_args = 1, .max_args = 2, .run = xiaotai_provisioning_at_forget_run,
     .usage = "FORGET [FORCE]"},
};

xiaotai_err_t xiaotai_provisioning_at_forget_register(void)
{
    return xiaotai_at_verbs_register("AT+NET", xiaotai_provisioning_at_forget_verbs,
                                     sizeof(xiaotai_provisioning_at_forget_verbs) /
                                         sizeof(xiaotai_provisioning_at_forget_verbs[0]));
}
