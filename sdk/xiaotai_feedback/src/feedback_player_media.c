// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

#include "xiaotai/framework.h"
#include "xiaotai/media.h"

_Static_assert(XIAOTAI_FEEDBACK_SEQUENCE_MAX <= XIAOTAI_MEDIA_PLAY_CLIPS_MAX, "clip capacity");
_Static_assert(XIAOTAI_FEEDBACK_FOREVER == XIAOTAI_PLAY_FOREVER, "loop sentinel");

static uint32_t playing_id, playing_token;
static bool was_busy;

static void media_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    if (id == playing_id) playing_id = playing_token = 0;
    xiaotai_feedback_player_done((uint32_t)(uintptr_t)ctx, result);
}

static xiaotai_err_t media_play(void *ctx, const xiaotai_feedback_play_t *request)
{
    (void)ctx;
    xiaotai_media_play_t play = {
        .clip_count = (uint8_t)request->count,
        .priority = XIAOTAI_PLAY_NORMAL,
        .generation = request->generation,
        .repeat = request->repeat,
        .gap_ms = request->gap_ms,
        .done = media_done,
        .ctx = (void *)(uintptr_t)request->token,
    };
    for (size_t i = 0; i < request->count; ++i) play.clips[i].name = request->names[i];
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_play_clips(&play, &id);
    was_busy = err == XIAOTAI_ERR_BUSY;
    if (err == XIAOTAI_OK) {
        playing_id = id;
        playing_token = request->token;
    }
    return err;
}

static void media_stop(void *ctx, uint32_t token)
{
    (void)ctx;
    if (token != playing_token || !playing_id) return;
    const uint32_t id = playing_id;
    playing_id = playing_token = 0;
    xiaotai_media_stop(id);
}

static bool media_busy(void *ctx)
{
    (void)ctx;
    return was_busy;
}

static const xiaotai_feedback_player_t player = {.play = media_play, .stop = media_stop, .busy = media_busy};

static void output_free(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    was_busy = false;
    xiaotai_feedback_player_idle();
}

xiaotai_err_t fb_default_player_init(void)
{
    const xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_MEDIA_OUTPUT_FREE, output_free, NULL);
    if (err == XIAOTAI_OK) fb_sound_install_default(&player);
    return err;
}

void fb_default_player_reset(void)
{
    playing_id = playing_token = 0;
    was_busy = false;
}

bool fb_default_player_stale(const xiaotai_feedback_player_t *candidate, uint32_t generation)
{
    if (candidate != &player) return false;
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    return status.generation != generation;
}
