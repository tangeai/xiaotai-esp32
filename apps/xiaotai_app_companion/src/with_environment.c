// SPDX-License-Identifier: MIT
#include "companion_internal.h"
#include "xiaotai/modules/environment.h"

static void changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const int32_t phase = event->args.value;
    /* The framework's quiesce event owns takeover and release. An idle
     * notification can be an AT refusal, not a maintenance session. */
    if (phase > XIAOTAI_ENVIRONMENT_IDLE && phase <= XIAOTAI_ENVIRONMENT_ERROR_HELD && xiaotai_quiesced()) {
        companion_setup_refresh();
    }
}

static xiaotai_err_t init(void)
{
    return XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ENVIRONMENT_CHANGED, changed, NULL);
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_environment = {
    .name = "environment", .init = init,
};
