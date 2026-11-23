// SPDX-License-Identifier: MIT
#include <assert.h>
#include "session.h"

int main(void)
{
    const xiaotai_media_requirement_t need = {
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 1000,
    };
    xiaotai_media_session_t request = {
        .generation = 7, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .flags = XIAOTAI_MEDIA_HOLD, .preroll_token = 19, .downlink_burst_ms = 1000,
    };
    media_session_control_t control = {0};
    assert(media_session_control_open(&control, &request, true, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_media_require(&need) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    assert(media_session_control_open(&control, &request, false, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_session_control_open(&control, &request, true, true) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_session_control_open(&control, &request, true, false) == XIAOTAI_OK);
    request.preroll_token = 0;
    assert(control.config.preroll_token == 19 && control.phase == MEDIA_SESSION_ARMED);
    assert(!control.start_pending && !control.uplink_open && !control.preroll_bound);
    assert(media_session_control_open(&control, &request, true, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_session_control_activate(&control, 7, false, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(control.phase == MEDIA_SESSION_ARMED && control.stale_calls == 0);
    assert(media_session_control_drain(&control, 7, 50, 1000) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_session_control_activate(&control, 6, true, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(!media_session_control_close(&control, 8));
    assert(control.stale_calls == 2);
    assert(media_session_control_activate(&control, 7, true, false) == XIAOTAI_OK);
    assert(control.phase == MEDIA_SESSION_ACTIVE && control.preroll_bound && control.start_pending);
    assert(media_session_control_activate(&control, 7, true, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_session_control_drain(&control, 7, 50, 1000) == XIAOTAI_OK);
    assert(media_session_control_drain(&control, 7, 50, 1000) == XIAOTAI_ERR_INVALID_STATE);

    xiaotai_err_t result = XIAOTAI_FAIL;
    assert(!media_session_control_start_result(&control, 6, XIAOTAI_OK));
    assert(!media_session_control_drain_result(&control, 7, true, 1000, &result));
    assert(media_session_control_start_result(&control, 7, XIAOTAI_OK));
    assert(!media_session_control_start_result(&control, 7, XIAOTAI_OK));
    assert(!media_session_control_drain_result(&control, 7, false, 50999, &result));
    assert(media_session_control_drain_result(&control, 7, false, 51000, &result));
    assert(control.phase == MEDIA_SESSION_DRAINED && result == XIAOTAI_ERR_TIMEOUT);
    assert(!media_session_control_drain_result(&control, 7, true, 51001, &result));
    assert(media_session_control_close(&control, 7));
    assert(control.stale_calls == 2 && control.previous == 7 && !control.preroll_bound);
    assert(media_session_control_open(&control, &request, false, false) == XIAOTAI_ERR_INVALID_STATE);

    request.generation = 9;
    request.flags = 0;
    assert(media_session_control_open(&control, &request, false, false) == XIAOTAI_OK);
    assert(control.phase == MEDIA_SESSION_ACTIVE && control.start_pending);
    assert(media_session_control_start_result(&control, 9, XIAOTAI_FAIL));
    assert(control.phase == MEDIA_SESSION_FAILED && !control.uplink_open);
    assert(media_session_control_close(&control, 9));
    request.generation = 10;
    assert(media_session_control_open(&control, &request, false, false) == XIAOTAI_OK);
    assert(media_session_control_start_result(&control, 10, XIAOTAI_OK));
    assert(media_session_control_drain(&control, 10, 100, 100000) == XIAOTAI_OK);
    assert(media_session_control_drain_result(&control, 10, true, 100000, &result) && result == XIAOTAI_OK);
    assert(media_session_control_close(&control, 10));

    request.generation = 11;
    const int64_t before_wrap_us = ((int64_t)UINT32_MAX - 100) * 1000;
    assert(media_session_control_open(&control, &request, false, false) == XIAOTAI_OK);
    assert(media_session_control_start_result(&control, 11, XIAOTAI_OK));
    assert(XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS == 7000U);
    assert(media_session_control_drain(&control, 11, XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS, before_wrap_us) == XIAOTAI_OK);
    assert(!media_session_control_drain_result(&control, 11, false, before_wrap_us + 6999000, &result));
    assert(media_session_control_drain_result(&control, 11, false, before_wrap_us + 7000000, &result));
    assert(result == XIAOTAI_ERR_TIMEOUT && media_session_control_close(&control, 11));
    xiaotai_media_reset();
    return 0;
}
