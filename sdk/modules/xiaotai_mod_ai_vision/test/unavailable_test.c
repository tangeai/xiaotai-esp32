// SPDX-License-Identifier: MIT
/* mod_ai_vision_unavailable: a device that cannot produce the pictures starts
 * normally in state UNAVAILABLE and contributes nothing:
 *   - no camera, or a camera without a JPEG encoder: nothing is declared to
 *     the video engine;
 *   - a camera that cannot produce the still picture size, or an engine that
 *     refuses the profile;
 * in every case conversations open nothing, the switch still changes and is
 * answered with XIAOTAI_ERR_NOT_SUPPORTED, and the reason is in the event log
 * and the diagnostics. A device that can (the control case) is OFF. */
#include <string.h>
#include "harness.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"

static int check_unavailable(xiaotai_err_t reason, unsigned requires)
{
    xiaotai_ai_vision_status_t status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_UNAVAILABLE && status.enabled && status.link == 0U);
    CHECK(fake_video.requires == requires);
    CHECK(owner_contributions("ai", XIAOTAI_LINK_SLOT_VIDEO, "ai", NULL, 0) == 0U);

    uint32_t generation = 1;
    int32_t value = 0;
    uint8_t level = 0;
    CHECK(eventlog_last("unavailable", &generation, &value, &level));
    CHECK(generation == 0U && value == (int32_t)reason && level == XIAOTAI_EVENTLOG_INFO);
    CHECK(strstr(owner_diag(), " state=4 link=0 ") != NULL);
    char expected[48];
    snprintf(expected, sizeof(expected), " unavailable=%d ", (int)reason);
    CHECK(strstr(owner_diag(), expected) != NULL);

    /* A conversation runs as if vision were not there. */
    trace_clear();
    const uint32_t id = owner_ai_begin();
    CHECK(id != 0U);
    owner_ai_generation(id, 31);
    owner_uplink(31, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    CHECK(fake_video.opens == 0 && fake_transport.permits == 0 && fake_video.closes == 0);

    /* The switch changes all the same; it just has nothing to switch. */
    CHECK(owner_set_enabled(false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(!owner_status().enabled);
    CHECK(owner_set_enabled(true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(owner_status().enabled);
    CHECK(post_vision_intent(-1) == XIAOTAI_OK);
    CHECK(!owner_status().enabled);
    owner_ai_generation(id, 0);
    owner_ai_end(id);
    CHECK(trace[0] == '\0');

    /* The state never changed, so no state event was sent. */
    status = owner_status();
    CHECK(status.state == XIAOTAI_AI_VISION_UNAVAILABLE && status.sessions == 0U && status.failures == 0U);
    CHECK(state_event_count == 0);
    return 0;
}

static int run(const char *name, xiaotai_err_t reason, unsigned requires)
{
    if (harness_start(NULL, true) != 0) return 1;
    const int failed = check_unavailable(reason, requires);
    harness_stop();
    if (failed) fprintf(stderr, "case: %s\n", name);
    return failed;
}

static int stored_switch_while_unavailable(void)
{
    /* With a lasting switch the setting is registered and written even here. */
    fakes_reset();
    fake_video.capabilities.uplink_codecs = 0;
    const xiaotai_ai_vision_config_t config = {.user_switch = true};
    if (harness_start(&config, true) != 0) return 1;
    int32_t value = -1;
    CHECK(xiaotai_setting_get_int("ai_vision.enabled", &value) == XIAOTAI_OK && value == 1);
    CHECK(owner_set_enabled(false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_setting_get_int("ai_vision.enabled", &value) == XIAOTAI_OK && value == 0);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    int32_t stored = -1;
    size_t size = sizeof(stored);
    CHECK(xiaotai_kv_get("xt_settings", "ai_vision", &stored, &size, 1000) == XIAOTAI_OK);
    CHECK(size == sizeof(stored) && stored == 0);
    harness_stop();
    return 0;
}

int main(void)
{
    int failed = 0;

    /* No camera at all. */
    fakes_reset();
    fake_video.capabilities.uplink_codecs = 0;
    fake_video.capabilities.camera_width = 0;
    fake_video.capabilities.camera_height = 0;
    failed |= run("no camera", XIAOTAI_ERR_NOT_SUPPORTED, 0);
    if (!failed) failed |= fake_video.resolves != 0;

    /* A camera whose pictures only an H.264 encoder takes. */
    fakes_reset();
    fake_video.capabilities.uplink_codecs = XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264);
    if (!failed) failed |= run("no JPEG encoder", XIAOTAI_ERR_NOT_SUPPORTED, 0);

    /* A camera smaller than the still picture. */
    fakes_reset();
    fake_video.resolve_result = XIAOTAI_ERR_NOT_SUPPORTED;
    if (!failed) failed |= run("picture too large", XIAOTAI_ERR_NOT_SUPPORTED, 0);
    if (!failed) failed |= fake_video.resolves != 1;

    /* The engine's profile table is full. */
    fakes_reset();
    fake_video.require_result = XIAOTAI_ERR_NO_MEM;
    if (!failed) failed |= run("profile refused", XIAOTAI_ERR_NO_MEM, 1);

    if (!failed) failed |= stored_switch_while_unavailable();

    /* Control: a device that can produce the pictures is OFF and contributes. */
    if (!failed) {
        fakes_reset();
        if (harness_start(NULL, true) != 0) return 1;
        const xiaotai_ai_vision_status_t status = owner_status();
        failed |= status.state != XIAOTAI_AI_VISION_OFF;
        failed |= owner_contributions("ai", XIAOTAI_LINK_SLOT_VIDEO, "ai", NULL, 0) != 1U;
        failed |= fake_video.requires != 1;
        harness_stop();
    }
    if (failed) {
        fprintf(stderr, "mod_ai_vision_unavailable failed; trace: %s\n", trace);
        return 1;
    }
    puts("mod_ai_vision_unavailable: ok");
    return 0;
}
