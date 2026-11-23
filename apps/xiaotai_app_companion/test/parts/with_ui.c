// SPDX-License-Identifier: MIT
#include "fakes.h"

static void activity(void) { ++fixture.activity_notes; }

static bool awake(void) { return !fixture.screen_asleep; }
static xiaotai_err_t sleep(void)
{
    ++fixture.sleep_requests;
    if (fixture.sleep_fail) return XIAOTAI_FAIL;
    fixture.screen_asleep = true;
    return XIAOTAI_OK;
}
static void timeout(uint32_t ms) { fixture.sleep_ms = ms; }
static bool session(companion_home_session_t *out)
{
    if (fixture.home_session.mode == COMPANION_HOME_IDLE) return false;
    *out = fixture.home_session;
    return true;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui = {.name = "ui", .user_activity = activity,
    .screen_awake = awake, .screen_sleep = sleep, .sleep_timeout = timeout, .home_session = session};
