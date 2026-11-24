// SPDX-License-Identifier: MIT
#include "feedback_fakes.h"
#include "audio_fakes.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/hal.h"
#include "xiaotai/media.h"

companion_feedback_fixture_t feedback_fixture;

static void holds_line(void *ctx, const char *line)
{
    const char *holds = strstr(line, " holds=");
    if (holds) *(unsigned *)ctx = (unsigned)strtoul(holds + 7, NULL, 10);
}

unsigned companion_fixture_restart_holds(void)
{
    unsigned holds = UINT32_MAX;
    xiaotai_diag_report("framework", &(xiaotai_reply_t){.write = holds_line, .ctx = &holds});
    return holds;
}

#ifndef COMPANION_TEST_REAL_UI
const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t output = {0};
    return !index && feedback_fixture.audio_present ? &output : NULL;
}
#endif

void xiaotai_media_status(xiaotai_media_status_t *status)
{
    *status = audio_fixture.status;
    status->ready = status->ready || feedback_fixture.audio_ready;
    status->startup_complete = status->ready || feedback_fixture.audio_start_error != XIAOTAI_OK;
    status->startup_error = feedback_fixture.audio_start_error;
}

static size_t effect_index(const char *name)
{
    assert(fixture.feedback);
    for (size_t i = 0; i < fixture.feedback->effect_count; ++i)
        if (!strcmp(fixture.feedback->effects[i].name, name)) return i;
    assert(false);
    return 0;
}

unsigned companion_fixture_fire_count(const char *name)
{
    return feedback_fixture.by_effect[effect_index(name)];
}

xiaotai_err_t xiaotai_feedback_fire(const char *name, const xiaotai_feedback_options_t *options)
{
    if (!feedback_fixture.enabled) return XIAOTAI_OK;
    assert(xiaotai_on_owner());
    const size_t index = effect_index(name);
    assert(index < sizeof(feedback_fixture.by_effect) / sizeof(feedback_fixture.by_effect[0]));
    ++feedback_fixture.by_effect[index];
    ++feedback_fixture.fired;
    feedback_fixture.last_effect = name;
    feedback_fixture.last_generation = options ? options->generation : 0;
    feedback_fixture.last_valid_ms = options ? options->valid_ms : 0;
    if (!strcmp(name, XIAOTAI_APP_COMPANION_EFFECT_STARTUP) && feedback_fixture.startup_error)
        return feedback_fixture.startup_error;
    if (!strcmp(name, XIAOTAI_APP_COMPANION_EFFECT_CONNECTED)) {
        feedback_fixture.holds_at_connected = companion_fixture_restart_holds();
        if (feedback_fixture.connected_error) return feedback_fixture.connected_error;
    }
    if (feedback_fixture.busy && (fixture.feedback->effects[index].sound.flags & XIAOTAI_FEEDBACK_WAIT))
        feedback_fixture.waiting = name;
    if (options && options->done) {
        assert(!feedback_fixture.done);
        feedback_fixture.done = options->done;
        feedback_fixture.done_ctx = options->ctx;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_feedback_set(const char *name, bool active)
{
    if (!strcmp(name, XIAOTAI_APP_COMPANION_STATE_INCOMING)) feedback_fixture.rings[0] = active;
    if (!strcmp(name, XIAOTAI_APP_COMPANION_STATE_OUTGOING)) feedback_fixture.rings[1] = active;
    if (!strcmp(name, XIAOTAI_APP_COMPANION_STATE_ERROR)) feedback_fixture.error = active;
    return XIAOTAI_OK;
}

void companion_fixture_prompt_end(xiaotai_err_t result)
{
    xiaotai_feedback_done_fn_t done = feedback_fixture.done;
    void *ctx = feedback_fixture.done_ctx;
    feedback_fixture.done = NULL;
    feedback_fixture.done_ctx = NULL;
    if (done) {
        ++feedback_fixture.released;
        done(result, ctx);
    }
}

xiaotai_err_t xiaotai_feedback_cancel(const char *name, bool stop_playing)
{
    if (stop_playing) {
        assert(!strcmp(name, XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE) ||
               !strcmp(name, XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE));
        ++feedback_fixture.cancel_playing;
    }
    if (!feedback_fixture.enabled) return XIAOTAI_OK;
    ++feedback_fixture.cancelled;
    feedback_fixture.last_cancelled = name;
    if (feedback_fixture.waiting && !strcmp(feedback_fixture.waiting, name)) {
        feedback_fixture.waiting = NULL;
        companion_fixture_prompt_end(XIAOTAI_ERR_INVALID_STATE);
    }
    return XIAOTAI_OK;
}

void companion_fixture_audio_idle(void)
{
    feedback_fixture.busy = false;
    if (feedback_fixture.waiting) {
        feedback_fixture.heard_effect = feedback_fixture.waiting;
        feedback_fixture.waiting = NULL;
        ++feedback_fixture.heard;
    }
}

xiaotai_err_t xiaotai_feedback_group_enable(uint8_t group, bool enabled)
{
    assert(group == XIAOTAI_APP_COMPANION_GROUP_GUIDANCE);
    feedback_fixture.guidance = enabled;
    return XIAOTAI_OK;
}

bool xiaotai_feedback_sound_pending(void) { return feedback_fixture.busy; }

xiaotai_err_t xiaotai_binding_announce(void)
{
    assert(xiaotai_on_owner());
    ++feedback_fixture.announced;
    if (feedback_fixture.announce_result) return feedback_fixture.announce_result;
    assert(fixture.binding.phase == XIAOTAI_BINDING_WAITING && !fixture.binding.announcing);
    fixture.binding.announcing = true;
    ++fixture.binding.revision;
    return XIAOTAI_OK;
}

void companion_fixture_announcement_end(void)
{
    assert(xiaotai_on_owner() && fixture.binding.announcing);
    fixture.binding.announcing = false;
    ++fixture.binding.revision;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_BINDING_ANNOUNCED, NULL, NULL, 0) == XIAOTAI_OK);
}

void xiaotai_binding_announce_stop(void)
{
    ++feedback_fixture.stopped;
    companion_fixture_announcement_end();
}
