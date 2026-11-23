// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_camera_pacing.py
// AI declares a 1 Hz still profile with a 750 ms age limit and immediate refetch.
/* mod_ai_vision_session: pictures flow exactly while the activity "ai" carries
 * a non-zero generation and the switch is on.
 *   - nothing before the service accepted the session (generation 0), the
 *     still profile declared once at init (V1, V3);
 *   - non-zero generation: permission on the conversation's link, then a video
 *     session with the still profile on that generation, uplink only (V2, V3);
 *   - generation back to 0 (tail playout): the session closes at once and the
 *     permission is withdrawn (V4);
 *   - revocation: the attachment's end closes the session before the host
 *     hears of it (V5);
 *   - the switch turned off releases the camera, turned on opens it again; on
 *     while no session was accepted opens nothing (V7);
 *   - only the activity "ai" carries vision;
 *   - nothing the module holds outlives a conversation (host memory counters).
 * The admission half of the AI image uplink of the former camera adapter
 * regression test is covered here (requested only for the AI session, only
 * with a generation, ready without a peer subscription). */
#include <string.h>
#include "harness.h"
#include "xiaotai_mem_host.h"

static int check_still_profile(const xiaotai_video_profile_t *p)
{
    CHECK(p->codec == XIAOTAI_VIDEO_CODEC_JPEG);
    CHECK(p->width == 320 && p->height == 240 && p->max_pixels == 0U);
    CHECK(p->fallback_width == 240 && p->fallback_height == 176);
    CHECK(!p->upright);
    CHECK(p->fps == 1);
    CHECK(p->quality == 45 && p->min_quality == 30);
    CHECK(p->max_frame_bytes == 64U * 1024U);
    CHECK(p->backlog_limit_bytes == 32U * 1024U);
    CHECK(p->max_age_ms == 750);
    CHECK(p->pressure_limit == 3);
    CHECK(p->refetch_stale);
    CHECK(p->retry_ms == 0);
    CHECK(p->bitrate_bps == 0U && p->min_qp == 0 && p->max_qp == 0 && p->key_interval_s == 0);
    return 0;
}

static int test_init(void)
{
    /* Declared once, at init, before any session: the engine reserves for it. */
    CHECK(strcmp(trace, "require ") == 0);
    CHECK(fake_video.requires == 1 && fake_video.resolves == 1 && fake_video.resolved_camera == 0);
    CHECK(check_still_profile(&fake_video.required) == 0);
    CHECK(check_still_profile(&fake_video.resolved) == 0);
    const xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.enabled && status.state == XIAOTAI_AI_VISION_OFF && status.link == 0U);
    CHECK(status.error == XIAOTAI_OK && status.sessions == 0U && status.failures == 0U);
    CHECK(fake_video.opens == 0 && fake_transport.permits == 0);
    return 0;
}

static int test_conversation(void)
{
    trace_clear();
    const uint32_t id = owner_ai_begin();
    CHECK(id != 0U);
    /* Connecting and handshaking: the link may exist, but no picture yet. */
    CHECK(trace[0] == '\0' && fake_video.opens == 0);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_OFF);

    owner_ai_generation(id, 101);
    CHECK(strcmp(trace, "permit:101:1:1 open:101 state:1 ") == 0);
    CHECK(fake_video.session.generation == 101U);
    CHECK(fake_video.session.uplink != NULL && check_still_profile(&fake_video.opened) == 0);
    CHECK(fake_video.session.downlink == NULL && !fake_video.opened_downlink);
    CHECK(fake_video.session.link == &fake_transport_link);
    CHECK(fake_video.session.camera == 0 && fake_video.session.placement == NULL);
    xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_STARTING && status.link == 101U && status.sessions == 1U);
    CHECK(state_event_count == 1 && state_events[0] == XIAOTAI_AI_VISION_STARTING);
    CHECK(last_state_payload.link == 101U && last_state_payload.enabled);

    owner_uplink(101, XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    CHECK(state_event_count == 1);
    owner_uplink(101, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_RUNNING);
    CHECK(state_event_count == 2 && state_events[1] == XIAOTAI_AI_VISION_RUNNING);

    /* Another module's session (or an older one) does not touch this one. */
    owner_uplink(77, XIAOTAI_VIDEO_ERROR, XIAOTAI_ERR_TIMEOUT);
    status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_RUNNING && status.failures == 0U);
    CHECK(strstr(owner_diag(), " ignored=1 ") != NULL);

    /* The tail starts playing: the camera stops at once, the link stays for
     * the audio, so the permission goes too. */
    trace_clear();
    owner_ai_generation(id, 0);
    CHECK(strcmp(trace, "close:101 permit:101:1:0 state:0 ") == 0);
    status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_OFF && status.link == 0U && status.error == XIAOTAI_OK);
    CHECK(owner_activity_generation(id) == 0U);

    /* The engine winding the closed session down is not this module's concern. */
    owner_uplink(101, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
    CHECK(state_event_count == 3 && state_events[2] == XIAOTAI_AI_VISION_OFF);

    trace_clear();
    owner_ai_end(id);
    CHECK(trace[0] == '\0' && fake_video.closes == 1);
    CHECK(eventlog_count("opened") == 1 && eventlog_count("closed") == 1);
    return 0;
}

static int test_revoked(void)
{
    const uint32_t id = owner_ai_begin();
    CHECK(id != 0U);
    owner_ai_generation(id, 202);
    owner_uplink(202, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    CHECK(owner_status().state == XIAOTAI_AI_VISION_RUNNING);

    /* A call preempts the conversation: the camera is let go before the AI
     * talk module hears of the revocation, which then releases its link (and
     * with it the permission). */
    trace_clear();
    const uint32_t call = owner_call_begin();
    CHECK(call != 0U);
    CHECK(strcmp(trace, "close:202 revoked state:0 ") == 0);
    const xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_OFF && status.link == 0U && status.sessions == 2U);

    /* Only the activity "ai" carries pictures: a call's session opens nothing. */
    trace_clear();
    owner_ai_generation(call, 606);
    CHECK(owner_activity_generation(call) == 606U);
    CHECK(trace[0] == '\0');
    owner_call_end(call);
    CHECK(trace[0] == '\0');
    return 0;
}

static int test_switch(void)
{
    const uint32_t id = owner_ai_begin();
    owner_ai_generation(id, 303);
    CHECK(owner_status().link == 303U);

    trace_clear();
    CHECK(owner_set_enabled(false) == XIAOTAI_OK);
    CHECK(strcmp(trace, "close:303 permit:303:1:0 state:0 ") == 0);
    xiaotai_ai_vision_status_t status = owner_status();
    CHECK(!status.enabled && status.state == XIAOTAI_AI_VISION_OFF && status.link == 0U);
    CHECK(!last_state_payload.enabled);

    trace_clear();
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(strcmp(trace, "permit:303:1:1 open:303 state:1 ") == 0);
    status = owner_status();
    CHECK(status.enabled && status.state == XIAOTAI_AI_VISION_STARTING && status.link == 303U);
    /* The same conversation: counted once. */
    CHECK(status.sessions == 3U);

    /* Turning it on again changes nothing. */
    trace_clear();
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(trace[0] == '\0');

    /* The conversation ends while pictures flow: closed, and the host's own
     * link release clears the permission. */
    trace_clear();
    owner_ai_end(id);
    CHECK(strcmp(trace, "close:303 state:0 ") == 0);

    /* On while the session is not accepted yet: nothing opens until it is. */
    CHECK(owner_set_enabled(false) == XIAOTAI_OK);
    const uint32_t second = owner_ai_begin();
    trace_clear();
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(trace[0] == '\0' && owner_status().state == XIAOTAI_AI_VISION_OFF);
    owner_ai_generation(second, 404);
    CHECK(strcmp(trace, "permit:404:1:1 open:404 state:1 ") == 0);
    owner_ai_end(second);

    /* Off when the session is accepted, on later: opened with the generation
     * the framework holds then. */
    const uint32_t third = owner_ai_begin();
    CHECK(owner_set_enabled(false) == XIAOTAI_OK);
    trace_clear();
    owner_ai_generation(third, 505);
    CHECK(trace[0] == '\0');
    CHECK(owner_set_enabled(true) == XIAOTAI_OK);
    CHECK(strcmp(trace, "permit:505:1:1 open:505 state:1 ") == 0);
    owner_ai_end(third);
    return 0;
}

static int test_known_at_begin(void)
{
    /* A host that knows its generation when the activity begins. */
    trace_clear();
    const uint32_t id = owner_ai_begin_at(707);
    CHECK(id != 0U);
    CHECK(strcmp(trace, "permit:707:1:1 open:707 state:1 ") == 0);
    owner_ai_end(id);
    return 0;
}

static int test_nothing_held(void)
{
    /* Whatever a conversation allocates (event copies) is gone after it. */
    drain();
    xiaotai_mem_host_clear();
    const uint32_t id = owner_ai_begin();
    owner_ai_generation(id, 808);
    owner_uplink(808, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    owner_ai_generation(id, 0);
    owner_ai_end(id);
    drain();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    CHECK(owner_status().link == 0U);
    return 0;
}

int main(void)
{
    fakes_reset();
    if (harness_start(NULL, true) != 0) return 1;
    int failed = test_init();
    if (!failed) failed = test_conversation();
    if (!failed) failed = test_revoked();
    if (!failed) failed = test_switch();
    if (!failed) failed = test_known_at_begin();
    if (!failed) failed = test_nothing_held();
    harness_stop();
    if (failed) return 1;
    puts("mod_ai_vision_session: ok");
    return 0;
}
