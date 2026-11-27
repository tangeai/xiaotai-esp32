// SPDX-License-Identifier: MIT
/* Console verbs this module adds to the network engine's AT+NET, and the
 * diagnostics section "provisioning".
 *
 *   AT+NET PROVISION [FORCE]   xiaotai_provisioning_begin(), FORCE giving
 *                              XIAOTAI_PROVISIONING_PREEMPT
 *   AT+NET CANCEL              xiaotai_provisioning_cancel()
 * The word FORGET belongs to a second table (provisioning_at_forget.c) that
 * exists only with XIAOTAI_NET_CONSOLE_FORGET; without it the word is
 * refused here with XIAOTAI_ERR_INVALID_ARG and nothing happens. */
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/net_config.h"

static xiaotai_err_t at_provision(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    uint32_t flags = 0;
    if (argc == 1) {
        if (strcmp(argv[0], "FORCE") == 0) {
            flags = XIAOTAI_PROVISIONING_PREEMPT;
        } else if (strcmp(argv[0], "FORGET") == 0) {
#if XIAOTAI_NET_CONSOLE_FORGET
            return XIAOTAI_AT_PASS;   /* the FORGET table owns it */
#else
            return XIAOTAI_ERR_INVALID_ARG;
#endif
        } else {
            return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    return prov_begin(flags, false);
}

static xiaotai_err_t at_cancel(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_provisioning_cancel();
}

static const xiaotai_at_verb_t s_verbs[] = {
    {.word = "PROVISION", .min_args = 0, .max_args = 1, .run = at_provision, .usage = "[FORCE]"},
    {.word = "CANCEL", .min_args = 0, .max_args = 0, .run = at_cancel, .usage = NULL},
};

xiaotai_err_t prov_at_register(void)
{
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+NET", s_verbs, sizeof(s_verbs) / sizeof(s_verbs[0]));
#if XIAOTAI_NET_CONSOLE_FORGET
    if (err == XIAOTAI_OK) err = xiaotai_provisioning_at_forget_register();
#endif
    return err;
}

static const char *phase_word(xiaotai_provisioning_phase_t phase)
{
    static const char *const words[] = {
        "none", "starting", "waiting", "verifying", "rejected", "storing", "store_failed", "done", "restarting",
    };
    return (size_t)phase < sizeof(words) / sizeof(words[0]) ? words[phase] : "-";
}

/* One line; the console prefixes it with +PROVISIONING:. Keys are added over
 * time, never renamed. */
void prov_diag(const xiaotai_reply_t *out)
{
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_stats_t stats;
    xiaotai_provisioning_status(&status);
    xiaotai_provisioning_stats(&stats);
    char line[400];
    snprintf(line, sizeof(line),
             "phase=%s exclusive=%d restart_pending=%d holding=%d error=%d begin=%d begins=%lu "
             "begins_refused=%lu begins_failed=%lu cancels=%lu retries=%lu stored=%lu rejected=%lu "
             "store_failures=%lu flush_timeouts=%lu flush_failures=%lu restart_holds=%lu",
             phase_word(status.phase), status.exclusive ? 1 : 0, status.restart_pending ? 1 : 0,
             status.holding_restart ? 1 : 0, (int)status.error, (int)prov.begin, (unsigned long)stats.begins,
             (unsigned long)stats.begins_refused, (unsigned long)stats.begins_failed, (unsigned long)stats.cancels,
             (unsigned long)stats.retries, (unsigned long)stats.stored, (unsigned long)stats.rejected,
             (unsigned long)stats.store_failures, (unsigned long)stats.flush_timeouts,
             (unsigned long)stats.flush_failures, (unsigned long)stats.restart_holds);
    out->write(out->ctx, line);
}
