// SPDX-License-Identifier: MIT
/* mod_provisioning_restart: where a restart follows (exclusive provisioning,
 * normal boot) the module hands its quiesce to the framework's restart as
 * soon as the engine reports restart_required, so the quiesce is never
 * released in between and the platform gate stays closed; the restart's
 * reason is "provisioning"; the engine's own later request finds the restart
 * pending; XIAOTAI_EVENT_RESTARTING makes the phase RESTARTING. A begin whose
 * restart was requested cannot be cancelled. */
#include <string.h>
#include "harness.h"

static void engine_request(void *ctx)
{
    *(xiaotai_err_t *)ctx = xiaotai_restart_request("network", 0);
}

static void finish(void *ctx)
{
    fake_net_finish(*(xiaotai_err_t *)ctx);
    /* The engine's worker asks too, through the owner's queue behind its
     * event; by then the restart is pending. */
    (void)xiaotai_run(engine_request, ctx);
}

int main(void)
{
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_gate.drained = true;
    fake_net.exclusive_mode = true;
    CHECK(harness_start(40) == 0);

    CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    drain();
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.phase == XIAOTAI_PROVISIONING_STARTING && status.restart_pending);
    CHECK(owner_quiesced() && fake_gate.closed);

    /* Hold the restart (an application's prompt) to look at the hand-over. */
    xiaotai_provisioning_restart_hold();
    static xiaotai_err_t result = XIAOTAI_OK;
    on_owner(finish, &result);
    CHECK(result == XIAOTAI_OK);   /* the engine's request: already pending */
    CHECK(xiaotai_restart_pending() && owner_quiesced());
    CHECK(framework_holds() == 1);   /* also leaves the framework's lines in reply_text */
    CHECK(strstr(reply_text, "quiesced=1 reason=provisioning restart=1") != NULL);
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE && fake_net.closes == 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(phase() == XIAOTAI_PROVISIONING_STARTING);

    xiaotai_provisioning_restart_release();
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 1000));
    for (int i = 0; i < 100 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1);
    CHECK(atomic_load(&probe_settings_flushes) == 1);
    CHECK(strstr(trace, "restarting:provisioning ") != NULL);
    /* One quiesce from the begin to the restart: never released. */
    CHECK(trace_count("quiesce:1") == 1 && trace_count("quiesce:0") == 0);
    CHECK(fake_gate.closed && fake_gate.opens == 0);
    CHECK(trace_count("failed:") == 0);
    harness_stop();
    puts("mod_provisioning_restart: passed");
    return 0;
}
