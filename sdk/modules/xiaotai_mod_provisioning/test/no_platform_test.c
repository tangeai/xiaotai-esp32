// SPDX-License-Identifier: MIT
/* mod_provisioning_no_platform: the module built with src/platform_none.c
 * and linked without any platform symbol (no fake_platform.c here). Where a
 * restart follows, the begin closes no gate: it waits for settings only and
 * hands the request over as soon as they are written; the restart hand-over
 * works as with the platform core. */
#include <string.h>
#include "harness.h"

static void finish(void *ctx) { fake_net_finish(*(xiaotai_err_t *)ctx); }

int main(void)
{
    fake_net_reset();
    fake_net.exclusive_mode = true;
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(XIAOTAI_PROVISIONING_FORGET) == XIAOTAI_OK);
    xiaotai_delay_ms(250);
    drain();
    CHECK(fake_net.reprovisions == 0 && owner_quiesced());
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(fake_net.last_forget && !fake_net.gate_closed_at_reprovision && fake_net.quiesced_at_reprovision);
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(xiaotai_restart_pending() && owner_quiesced());
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 2000));
    for (int i = 0; i < 100 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1 && strstr(trace, "restarting:provisioning ") != NULL);
    CHECK(trace_count("quiesce:0") == 0);
    harness_stop();
    puts("mod_provisioning_no_platform: passed");
    return 0;
}
