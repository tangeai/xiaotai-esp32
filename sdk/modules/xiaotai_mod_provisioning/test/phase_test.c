// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_runtime_wifi.c::runtime_wifi
/* mod_provisioning_phase: the derivation table of the specification (2.2),
 * row by row and first match first, the snapshot (both engine structs taken
 * at one moment, revision moving on every change), the changed event with its
 * payload, the opened event with reason and access point name, the module's
 * own begin as STARTING, and the status before the module started. */
#include <string.h>
#include "harness.h"

typedef struct {
    xiaotai_net_provision_state_t state;
    xiaotai_err_t error;
} set_t;

static void set_state(void *ctx)
{
    const set_t *set = ctx;
    fake_net_provision(set->state, set->error);
}

static void emit_restarting(void *ctx)
{
    (void)ctx;
    const xiaotai_args_t args = {.value = 20, .text = "network"};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_RESTARTING, &args, NULL, 0);
}

static void foreign_request(void *ctx)
{
    /* Somebody else's request (a retry from the console) is pending. */
    (void)ctx;
    fake_net.link.control_pending = true;
    fake_net_link_changed();
}

static void foreign_request_done(void *ctx)
{
    (void)ctx;
    fake_net.link.control_pending = false;
    fake_net_link_changed();
}

static void finish(void *ctx)
{
    fake_net_finish(*(xiaotai_err_t *)ctx);
}

int main(void)
{
    /* Before the module started: phase NONE, everything zero. */
    xiaotai_provisioning_status_t status;
    memset(&status, 0x5A, sizeof(status));
    xiaotai_provisioning_status(&status);
    CHECK(status.phase == XIAOTAI_PROVISIONING_NONE && status.revision == 0 && !status.restart_pending);
    CHECK(status.provision.state == XIAOTAI_NET_PROVISION_CLOSED && status.link.state == XIAOTAI_NET_STOPPED);
    CHECK(!xiaotai_provisioning_active());

    /* Methods already open at start (no credentials at boot): OPENED at start. */
    fake_net_reset();
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    fake_net.provision.reason = XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED;
    fake_net.provision.methods = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL;
    strcpy(fake_net.provision.ap_ssid, "XiaoTai-00FF");
    CHECK(harness_start(20) == 0);
    CHECK(strstr(trace, "opened:1:XiaoTai-00FF ") != NULL);
    CHECK(phase() == XIAOTAI_PROVISIONING_WAITING && xiaotai_provisioning_active());
    harness_stop();

    fake_net_reset();
    CHECK(harness_start(20) == 0);
    CHECK(trace_count("opened:") == 0);
    CHECK(phase() == XIAOTAI_PROVISIONING_NONE);
    xiaotai_provisioning_status(&status);
    const uint32_t first_revision = status.revision;

    /* Rows 3 and 4: the engine's provisioning state, in turn. */
    static const struct {
        xiaotai_net_provision_state_t engine;
        xiaotai_provisioning_phase_t phase;
    } rows[] = {
        {XIAOTAI_NET_PROVISION_OPENING, XIAOTAI_PROVISIONING_STARTING},
        {XIAOTAI_NET_PROVISION_OPEN, XIAOTAI_PROVISIONING_WAITING},
        {XIAOTAI_NET_PROVISION_VERIFYING, XIAOTAI_PROVISIONING_VERIFYING},
        {XIAOTAI_NET_PROVISION_REJECTED, XIAOTAI_PROVISIONING_REJECTED},
        {XIAOTAI_NET_PROVISION_STORING, XIAOTAI_PROVISIONING_STORING},
        {XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_PROVISIONING_STORE_FAILED},
        {XIAOTAI_NET_PROVISION_DONE, XIAOTAI_PROVISIONING_DONE},
        {XIAOTAI_NET_PROVISION_CLOSING, XIAOTAI_PROVISIONING_NONE},
        {XIAOTAI_NET_PROVISION_CLOSED, XIAOTAI_PROVISIONING_NONE},
    };
    uint32_t revision = first_revision;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        trace_clear();
        set_t set = {.state = rows[i].engine};
        on_owner(set_state, &set);
        xiaotai_provisioning_status(&status);
        CHECK(status.phase == rows[i].phase);
        /* Both copies are the engine's at that moment. */
        CHECK(status.provision.state == rows[i].engine && status.provision.revision == fake_net.provision.revision);
        CHECK(status.link.revision == fake_net.link.revision);
        CHECK(status.revision > revision);
        revision = status.revision;
        char expected[32];
        snprintf(expected, sizeof(expected), "changed:%d ", (int)rows[i].phase);
        CHECK(strstr(trace, expected) != NULL);
        CHECK(strstr(trace, "!payload") == NULL);
        CHECK(xiaotai_provisioning_active() == (rows[i].phase != XIAOTAI_PROVISIONING_NONE));
    }
    /* OPEN after OPENING announced the methods once; REJECTED is a failure
     * of "verify" with the engine's error. */
    trace_clear();
    set_t open = {.state = XIAOTAI_NET_PROVISION_OPEN};
    on_owner(set_state, &open);
    CHECK(trace_count("opened:") == 1);
    trace_clear();
    set_t rejected = {.state = XIAOTAI_NET_PROVISION_REJECTED, .error = XIAOTAI_ERR_NOT_FOUND};
    on_owner(set_state, &rejected);
    CHECK(trace_count("opened:") == 0);
    CHECK(strstr(trace, "failed:verify:261 ") != NULL);   /* XIAOTAI_ERR_NOT_FOUND */
    xiaotai_provisioning_stats_t stats;
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.rejected == 2);   /* the table's row and this one */
    set_t closed = {.state = XIAOTAI_NET_PROVISION_CLOSED};
    on_owner(set_state, &closed);

    /* A request of somebody else does not make the phase STARTING. */
    on_owner(foreign_request, NULL);
    CHECK(phase() == XIAOTAI_PROVISIONING_NONE);
    on_owner(foreign_request_done, NULL);

    /* Row 2: this module's begin before it reached the engine (settings
     * still pending), then the engine working on it (control_pending). */
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(phase() == XIAOTAI_PROVISIONING_STARTING && fake_net.reprovisions == 0);
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_reprovisions(1, 1000));
    drain();
    CHECK(fake_net.link.control_pending && phase() == XIAOTAI_PROVISIONING_STARTING);
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(phase() == XIAOTAI_PROVISIONING_WAITING);

    /* Row 1 beats every other: once the restart is announced. */
    on_owner(emit_restarting, NULL);
    CHECK(phase() == XIAOTAI_PROVISIONING_RESTARTING);
    on_owner(set_state, &closed);
    CHECK(phase() == XIAOTAI_PROVISIONING_RESTARTING);
    harness_stop();
    puts("mod_provisioning_phase: passed");
    return 0;
}
