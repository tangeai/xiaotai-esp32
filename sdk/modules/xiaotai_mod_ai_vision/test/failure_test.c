// SPDX-License-Identifier: MIT
/* mod_ai_vision_failure: a camera or encoder failure costs the pictures of
 * that conversation only (V6):
 *   - the engine reports the uplink failed: state FAILED with the engine's
 *     error, counted and recorded, the conversation (activity, generation)
 *     untouched, no second attempt in the same conversation, not even when the
 *     switch is turned off and on; the session closes when the conversation
 *     lets go of it;
 *   - the open itself is refused, or the link refuses the permission: FAILED
 *     at once, the permission taken back, nothing to close later;
 *   - the next conversation tries again. */
#include <string.h>
#include "harness.h"
#include "xiaotai/eventlog.h"

static int test_uplink_failure(void)
{
    const uint32_t id = owner_ai_begin();
    owner_ai_generation(id, 11);
    owner_uplink(11, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    trace_clear();
    owner_uplink(11, XIAOTAI_VIDEO_ERROR, XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(trace, "state:3 ") == 0);
    xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_FAILED && status.error == XIAOTAI_ERR_TIMEOUT);
    CHECK(status.failures == 1U && status.sessions == 1U && status.link == 11U);
    CHECK(last_state_payload.state == XIAOTAI_AI_VISION_FAILED && last_state_payload.error == XIAOTAI_ERR_TIMEOUT);

    uint32_t generation = 0;
    int32_t value = 0;
    uint8_t level = 0;
    CHECK(eventlog_last("failed", &generation, &value, &level));
    CHECK(generation == 11U && value == XIAOTAI_ERR_TIMEOUT && level == XIAOTAI_EVENTLOG_ERROR);

    /* The conversation goes on with audio alone. */
    CHECK(owner_current_activity() == id && owner_activity_generation(id) == 11U);
    CHECK(trace_count("revoked") == 0);

    /* Whatever the engine reports next for that session changes nothing. */
    owner_uplink(11, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_FAILED && fake_video.opens == 1);

    /* The tail: the failed session is closed like any other. */
    trace_clear();
    owner_ai_generation(id, 0);
    CHECK(strcmp(trace, "close:11 permit:11:1:0 state:0 ") == 0);
    status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_OFF && status.error == XIAOTAI_OK && status.failures == 1U);
    owner_ai_end(id);
    return 0;
}

static int test_no_retry_by_switch(void)
{
    const uint32_t id = owner_ai_begin();
    owner_ai_generation(id, 12);
    CHECK(fake_video.opens == 2);   /* a new conversation tries again */
    owner_uplink(12, XIAOTAI_VIDEO_ERROR, XIAOTAI_ERR_NOT_FOUND);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_FAILED);

    trace_clear();
    CHECK(owner_set_enabled(false) == XIAOTAI_OK);
    CHECK(strcmp(trace, "close:12 permit:12:1:0 state:0 ") == 0);
    trace_clear();
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(strcmp(trace, "state:3 ") == 0);
    const xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_FAILED && status.error == XIAOTAI_ERR_NOT_FOUND);
    CHECK(status.link == 0U && status.failures == 2U && fake_video.opens == 2);

    trace_clear();
    owner_ai_end(id);
    CHECK(strcmp(trace, "state:0 ") == 0);

    /* An error report without a code still fails the session. */
    const uint32_t next = owner_ai_begin();
    owner_ai_generation(next, 13);
    owner_uplink(13, XIAOTAI_VIDEO_ERROR, XIAOTAI_OK);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_FAILED && owner_status().error == XIAOTAI_FAIL);
    owner_ai_end(next);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_OFF);
    return 0;
}

static int test_open_refused(void)
{
    fake_video.open_result = XIAOTAI_ERR_NOT_SUPPORTED;
    const uint32_t id = owner_ai_begin();
    trace_clear();
    owner_ai_generation(id, 21);
    CHECK(strcmp(trace, "permit:21:1:1 open:21 permit:21:1:0 state:3 ") == 0);
    xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_FAILED && status.error == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(status.link == 0U && status.failures == 4U);
    CHECK(owner_current_activity() == id);

    /* No second attempt in this conversation; nothing to close at its end. */
    fake_video.open_result = XIAOTAI_OK;
    CHECK(owner_set_enabled(false) == XIAOTAI_OK);
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(fake_video.opens == 4);
    trace_clear();
    owner_ai_end(id);
    CHECK(strcmp(trace, "state:0 ") == 0);
    return 0;
}

static int test_permit_refused(void)
{
    fake_transport.grant_result = XIAOTAI_ERR_INVALID_STATE;
    const uint32_t id = owner_ai_begin();
    trace_clear();
    owner_ai_generation(id, 31);
    CHECK(strcmp(trace, "permit:31:1:1 state:3 ") == 0);
    const xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_FAILED && status.error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(status.link == 0U && status.failures == 5U && status.sessions == 5U);
    owner_ai_end(id);
    fake_transport.grant_result = XIAOTAI_OK;

    /* The next conversation works again. */
    const uint32_t next = owner_ai_begin();
    trace_clear();
    owner_ai_generation(next, 32);
    CHECK(strcmp(trace, "permit:32:1:1 open:32 state:1 ") == 0);
    owner_ai_end(next);
    CHECK(eventlog_count("failed") == 5);
    CHECK(strstr(owner_diag(), " sessions=6 failures=5 ") != NULL);
    return 0;
}

int main(void)
{
    fakes_reset();
    if (harness_start(NULL, true) != 0) return 1;
    int failed = test_uplink_failure();
    if (!failed) failed = test_no_retry_by_switch();
    if (!failed) failed = test_open_refused();
    if (!failed) failed = test_permit_refused();
    harness_stop();
    if (failed) return 1;
    puts("mod_ai_vision_failure: ok");
    return 0;
}
