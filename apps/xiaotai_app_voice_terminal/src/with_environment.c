// SPDX-License-Identifier: MIT
#include "terminal_internal.h"
#include "xiaotai/modules/environment.h"

static void result(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_environment_status_t *status = event->size == sizeof(*status) ? event->data : NULL;
    terminal_reset_environment_result(event->args.value == XIAOTAI_ENVIRONMENT_CANCELLED,
                                       status ? status->error : XIAOTAI_ERR_INVALID_STATE);
}

static xiaotai_err_t init(void)
{
    return XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ENVIRONMENT_RESULT, result, NULL);
}

static xiaotai_err_t prepare(bool *waiting)
{
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    if (status.phase == XIAOTAI_ENVIRONMENT_IDLE) return XIAOTAI_OK;
    if (status.phase != XIAOTAI_ENVIRONMENT_ACQUIRING && status.phase != XIAOTAI_ENVIRONMENT_WAITING)
        return XIAOTAI_ERR_BUSY;
    const xiaotai_err_t err = xiaotai_environment_cancel();
    *waiting = err == XIAOTAI_OK;
    return err;
}

const terminal_environment_t terminal_with_environment = {.init = init, .prepare = prepare};
