// SPDX-License-Identifier: MIT
/* mod_provisioning_at (built with CONFIG_XIAOTAI_NET_CONSOLE_FORGET=1 and
 * src/provisioning_at_forget.c) and mod_provisioning_at_off (=0, without that
 * source: the build needs none of its symbols): the verbs this module adds to
 * AT+NET, run through the framework's command path and a dispatcher that
 * follows xiaotai_at_dispatch.
 *   on   PROVISION, PROVISION FORCE, PROVISION FORGET, PROVISION FORGET FORCE
 *        map to the begin flags; FORCE FORGET and other words are INVALID_ARG
 *   off  any FORGET is INVALID_ARG and the engine is never called; FORCE works
 *   both CANCEL; BUSY while an activity is current; the registered verbs and
 *        usages (what AT+HELP prints) equal the manifest's "commands" whose
 *        option is on in this build; the diagnostics line */
#include <string.h>
#include "harness.h"
#include "manifest_commands.h"
#include "xiaotai/net_config.h"

static void finish(void *ctx) { fake_net_finish(*(xiaotai_err_t *)ctx); }

/* Runs line and checks the begin it caused: forget and whether an activity was
 * revoked. Leaves the engine with the methods open again. */
static int provision(const char *line, bool forget, bool preempt)
{
    const unsigned before = fake_net.reprovisions;
    const int revoked = atomic_load(&revocations);
    if (preempt) CHECK(owner_activity_begin() != 0);
    CHECK(command(line) == XIAOTAI_OK);
    CHECK(wait_reprovisions(before + 1, 1000));
    CHECK(fake_net.last_forget == forget);
    CHECK(atomic_load(&revocations) == revoked + (preempt ? 1 : 0));
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(phase() == XIAOTAI_PROVISIONING_WAITING);
    return 0;
}

static bool option_on(const char *option)
{
    if (option[0] == '\0') return true;
    if (strcmp(option, "XIAOTAI_NET_CONSOLE_FORGET") == 0) return XIAOTAI_NET_CONSOLE_FORGET;
    return false;
}

static int help_matches_manifest(void)
{
    size_t registered = 0;
    for (size_t t = 0; t < fake_at_table_count; ++t) {
        for (size_t v = 0; v < fake_at_tables[t].count; ++v) {
            const xiaotai_at_verb_t *verb = &fake_at_tables[t].verbs[v];
            const char *usage = verb->usage ? verb->usage : "";
            bool found = false;
            for (size_t m = 0; m < MANIFEST_COMMAND_COUNT; ++m) {
                const manifest_command_t *entry = &manifest_commands[m];
                found |= strcmp(entry->command, fake_at_tables[t].command) == 0 &&
                         strcmp(entry->verb, verb->word ? verb->word : "") == 0 && strcmp(entry->usage, usage) == 0 &&
                         option_on(entry->option);
            }
            if (!found) fprintf(stderr, "registered %s %s %s is not in the manifest\n", fake_at_tables[t].command,
                                verb->word, usage);
            CHECK(found);
            ++registered;
        }
    }
    size_t expected = 0;
    for (size_t m = 0; m < MANIFEST_COMMAND_COUNT; ++m) expected += option_on(manifest_commands[m].option) ? 1 : 0;
    CHECK(registered == expected);
    return 0;
}

int main(void)
{
    fake_net_reset();
    CHECK(harness_start(20) == 0);
    CHECK(help_matches_manifest() == 0);

    CHECK(provision("AT+NET PROVISION", false, false) == 0);
    CHECK(provision("AT+NET PROVISION FORCE", false, true) == 0);
#if XIAOTAI_NET_CONSOLE_FORGET
    CHECK(provision("AT+NET PROVISION FORGET", true, false) == 0);
    CHECK(provision("AT+NET PROVISION FORGET FORCE", true, true) == 0);
#endif
    /* Refused words: nothing reaches the engine, nothing is quiesced. */
    const unsigned before = fake_net.reprovisions;
    CHECK(command("AT+NET PROVISION FORCE FORGET") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(command("AT+NET PROVISION BOGUS") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(command("AT+NET PROVISION FORGET FORCE FORCE") == XIAOTAI_ERR_INVALID_ARG);
#if !XIAOTAI_NET_CONSOLE_FORGET
    CHECK(command("AT+NET PROVISION FORGET") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(command("AT+NET PROVISION FORGET FORCE") == XIAOTAI_ERR_INVALID_ARG);
#endif
    xiaotai_delay_ms(150);
    drain();
    CHECK(fake_net.reprovisions == before && !owner_quiesced());

    /* BUSY without FORCE while an activity is current. */
    CHECK(owner_activity_begin() != 0);
    CHECK(command("AT+NET PROVISION") == XIAOTAI_ERR_BUSY);
    CHECK(fake_net.reprovisions == before && !owner_quiesced());

    /* CANCEL goes to the engine's close once the begin was handed. */
    CHECK(command("AT+NET CANCEL") == XIAOTAI_OK && fake_net.closes == 1);
    fake_net.close_result = XIAOTAI_ERR_NOT_FOUND;
    CHECK(command("AT+NET CANCEL") == XIAOTAI_ERR_NOT_FOUND && fake_net.closes == 2);
    CHECK(command("AT+NET CANCEL NOW") == XIAOTAI_ERR_INVALID_ARG && fake_net.closes == 2);

    /* Diagnostics: one line of key=value words. */
    xiaotai_provisioning_stats_t stats;
    xiaotai_provisioning_stats(&stats);
    reply_text[0] = '\0';
    char expected[64];
    snprintf(expected, sizeof(expected), "begins=%lu begins_refused=%lu", (unsigned long)stats.begins,
             (unsigned long)stats.begins_refused);
    diag_section("provisioning");
    const char *head = "phase=waiting exclusive=0 restart_pending=0 holding=0 error=261 ";
    CHECK(strncmp(reply_text, head, strlen(head)) == 0);
    CHECK(strstr(reply_text, expected) != NULL);
    CHECK(strchr(reply_text, '\n') == reply_text + strlen(reply_text) - 1);   /* one line */
    harness_stop();
    puts(XIAOTAI_NET_CONSOLE_FORGET ? "mod_provisioning_at: passed" : "mod_provisioning_at_off: passed");
    return 0;
}
