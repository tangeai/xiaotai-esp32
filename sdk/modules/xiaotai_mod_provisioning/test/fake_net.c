// SPDX-License-Identifier: MIT
/* Fakes of what the provisioning module calls below the framework: the
 * network engine's functions it uses, the preferences service's pending flag
 * and the AT engine's verb tables with a dispatcher that follows the rules of
 * xiaotai_at_dispatch (xiaotai/at_command.h). The platform gate is in
 * fake_platform.c, linked only where the platform core is part of the build. */
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "xiaotai/framework.h"
#include "xiaotai/preferences.h"

fake_net_t fake_net;
atomic_bool fake_settings_pending;

static void call(const char *text)
{
    strncat(fake_net.calls, text, sizeof(fake_net.calls) - strlen(fake_net.calls) - 1);
    strncat(fake_net.calls, " ", sizeof(fake_net.calls) - strlen(fake_net.calls) - 1);
}

/* Defined by fake_platform.c where the platform core is in the build. */
__attribute__((weak)) bool fake_gate_is_closed_and_drained(void) { return false; }

void fake_net_reset(void)
{
    memset(&fake_net, 0, sizeof(fake_net));
    fake_net.link.state = XIAOTAI_NET_UP;
    fake_net.link.kind = XIAOTAI_NETWORK_WIFI;
    fake_net.link.revision = 1;
    fake_net.provision.revision = 1;
    atomic_store(&fake_settings_pending, false);
}

/* ------------------------------------------------- the engine's functions */

void xiaotai_net_status(xiaotai_net_status_t *status) { *status = fake_net.link; }
void xiaotai_net_provision_status(xiaotai_net_provision_status_t *status) { *status = fake_net.provision; }
bool xiaotai_net_provisioning_boot(void) { return fake_net.provisioning_boot; }
bool xiaotai_net_provisioning_exclusive(void) { return fake_net.exclusive_mode; }

xiaotai_err_t xiaotai_net_reprovision(bool forget)
{
    char text[32];
    snprintf(text, sizeof(text), "reprovision(%d)", forget ? 1 : 0);
    call(text);
    ++fake_net.reprovisions;
    fake_net.last_forget = forget;
    fake_net.quiesced_at_reprovision = xiaotai_quiesced();
    fake_net.gate_closed_at_reprovision = fake_gate_is_closed_and_drained();
    fake_net.settings_pending_at_reprovision = atomic_load(&fake_settings_pending);
    if (fake_net.reprovision_result != XIAOTAI_OK) return fake_net.reprovision_result;
    /* Accepted requests are pending from the call on (the engine refuses a
     * second one synchronously). */
    fake_net.link.control_pending = true;
    ++fake_net.link.revision;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_net_provision_close(void)
{
    call("close");
    ++fake_net.closes;
    return fake_net.close_result;
}

xiaotai_err_t xiaotai_net_provision_store_retry(void)
{
    call("store_retry");
    ++fake_net.store_retries;
    return fake_net.store_retry_result;
}

xiaotai_err_t xiaotai_net_retry(void)
{
    call("retry");
    ++fake_net.retries;
    return fake_net.retry_result;
}

xiaotai_err_t xiaotai_net_scan(void)
{
    call("scan");
    ++fake_net.scans;
    return fake_net.scan_result;
}

size_t xiaotai_net_networks(xiaotai_net_network_t *networks, size_t capacity, xiaotai_net_scan_status_t *status)
{
    ++fake_net.network_reads;
    if (status) *status = (xiaotai_net_scan_status_t){.state = XIAOTAI_NET_SCAN_READY, .revision = 3};
    if (capacity > 0 && networks) {
        memset(&networks[0], 0, sizeof(networks[0]));
        strcpy(networks[0].ssid, "home");
        networks[0].available = true;
        networks[0].saved = true;
    }
    return 2;
}

/* ------------------------------------------------------- driving the fake */

void fake_net_provision(xiaotai_net_provision_state_t state, xiaotai_err_t error)
{
    fake_net.provision.state = state;
    fake_net.provision.error = error;
    ++fake_net.provision.revision;
    const xiaotai_args_t args = {.value = (int32_t)state};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_NET_PROVISION, &args, &fake_net.provision, sizeof(fake_net.provision));
}

void fake_net_link_changed(void)
{
    ++fake_net.link.revision;
    const xiaotai_args_t args = {.value = (int32_t)fake_net.link.state};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_NET_STATE, &args, &fake_net.link, sizeof(fake_net.link));
}

void fake_net_finish(xiaotai_err_t result)
{
    fake_net.link.control_pending = false;
    fake_net.link.control_error = result;
    if (result == XIAOTAI_OK && fake_net.exclusive_mode) {
        fake_net.provision.restart_required = true;
        fake_net_provision(fake_net.provision.state, XIAOTAI_OK);
    } else if (result == XIAOTAI_OK) {
        fake_net.link.state = XIAOTAI_NET_HELD;
        fake_net.link.user_disconnected = true;
        fake_net.provision.reason = XIAOTAI_NET_PROVISION_REASON_REQUESTED;
        fake_net.provision.methods = XIAOTAI_PROVISION_PORTAL;
        strcpy(fake_net.provision.ap_ssid, "XiaoTai-1A2B");
        fake_net_link_changed();
        fake_net_provision(XIAOTAI_NET_PROVISION_OPEN, XIAOTAI_OK);
        return;
    }
    fake_net_link_changed();
}

/* ------------------------------------------------------------ preferences */

bool xiaotai_preferences_pending(void) { return atomic_load(&fake_settings_pending); }

/* ------------------------------------------------------------- AT verbs */

fake_at_table_t fake_at_tables[FAKE_AT_TABLES_MAX];
size_t fake_at_table_count;

/* The rules of xiaotai_at_dispatch over every table of argv[0]. */
static xiaotai_err_t dispatch(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    const char *word = argc > 1 ? argv[1] : NULL;
    bool word_known = false;
    for (size_t t = 0; t < fake_at_table_count && word; ++t) {
        if (strcmp(fake_at_tables[t].command, argv[0]) != 0) continue;
        for (size_t v = 0; v < fake_at_tables[t].count; ++v) {
            if (fake_at_tables[t].verbs[v].word && strcmp(fake_at_tables[t].verbs[v].word, word) == 0) {
                word_known = true;
            }
        }
    }
    if (!word_known) return XIAOTAI_ERR_INVALID_ARG;   /* UNKNOWN_VERB or MISSING_VERB */
    bool ran = false;
    for (size_t t = 0; t < fake_at_table_count; ++t) {
        if (strcmp(fake_at_tables[t].command, argv[0]) != 0) continue;
        for (size_t v = 0; v < fake_at_tables[t].count; ++v) {
            const xiaotai_at_verb_t *verb = &fake_at_tables[t].verbs[v];
            if (!verb->word || strcmp(verb->word, word) != 0) continue;
            const int args = argc - 2;
            if (args < verb->min_args || args > verb->max_args) continue;
            ran = true;
            const xiaotai_err_t result = verb->run(args, argv + 2, reply);
            if (result != XIAOTAI_AT_PASS) return result;
        }
    }
    return ran ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_INVALID_ARG;   /* NO_OWNER : ARGUMENT_COUNT */
}

xiaotai_err_t xiaotai_at_verbs_register(const char *command, const xiaotai_at_verb_t *verbs, size_t count)
{
    if (!command || !verbs || !count) return XIAOTAI_ERR_INVALID_ARG;
    if (fake_at_table_count >= FAKE_AT_TABLES_MAX) return XIAOTAI_ERR_NO_MEM;
    bool known = false;
    for (size_t t = 0; t < fake_at_table_count; ++t) known |= strcmp(fake_at_tables[t].command, command) == 0;
    if (!known) {
        const xiaotai_err_t err = xiaotai_command_register(command, NULL, dispatch);
        if (err != XIAOTAI_OK) return err;
    }
    fake_at_tables[fake_at_table_count++] = (fake_at_table_t){.command = command, .verbs = verbs, .count = count};
    return XIAOTAI_OK;
}
