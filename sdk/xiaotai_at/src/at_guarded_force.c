// SPDX-License-Identifier: MIT
#include "internal.h"

#if XIAOTAI_AT_RESTART_FORCE
xiaotai_err_t xiaotai_at_guarded_force_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return xiaotai_at_guarded_restart_schedule(reply);
}
#endif
