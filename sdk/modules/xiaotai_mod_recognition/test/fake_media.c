// SPDX-License-Identifier: MIT
#include "operation_fixture.h"
#include "recognition_journal.h"
#include "xiaotai/framework.h"
#include <string.h>

bool fake_muted, fake_speaker_muted, fake_stop_holds;
bool fake_capture = true;
bool fake_playback = true, fake_play_stopped;
bool fake_foreign_recording;
unsigned fake_plays;
uint32_t fake_recorded;
unsigned fake_stops;
xiaotai_media_record_t fake_capture_request;
static uint32_t current_id, next_id;
static xiaotai_media_play_t current_play;
static uint32_t current_play_id;

void xiaotai_media_info(xiaotai_media_info_t *info)
{
    *info = (xiaotai_media_info_t){.capture = true, .capture_pcm = fake_capture, .capture_rate_hz = 16000,
                                  .playback = fake_playback};
}

void xiaotai_media_status(xiaotai_media_status_t *status)
{
    *status = (xiaotai_media_status_t){.ready = true, .mic_muted = fake_muted, .speaker_muted = fake_speaker_muted,
                                    .recording = fake_foreign_recording ? UINT32_MAX : current_id,
                                    .recorded_samples = fake_recorded};
}

xiaotai_err_t xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id)
{
    CHECK(xiaotai_on_owner() && !fake_capture_request.done);
    CHECK(request->pcm && request->samples >= 16000 && request->done);
    memset(request->pcm, 0x55, request->samples * sizeof(*request->pcm));
    fake_capture_request = *request;
    current_id = *id = ++next_id;
    fake_recorded = 0;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    CHECK(xiaotai_on_owner());
    if (request->priority != XIAOTAI_PLAY_URGENT) {
        return current_play_id && !fake_play_stopped ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
    }
    CHECK(!current_play_id && request->clip_count == 1U && request->done && !request->generation);
    CHECK(request->clips[0].sound && request->clips[0].sound->container == XIAOTAI_SOUND_WAV);
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    CHECK(mirror->state == XIAOTAI_RECOGNITION_JOURNAL_PRESENT && mirror->item_count);
    bool marked = false;
    for (size_t i = 0; i < mirror->item_count; ++i) marked |= mirror->items[i].state == XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED;
    CHECK(marked);
    current_play = *request;
    fake_play_stopped = false;
    current_play_id = *id = ++fake_plays;
    return XIAOTAI_OK;
}

void xiaotai_media_stop(uint32_t id)
{
    CHECK(xiaotai_on_owner() && id && id == current_play_id);
    fake_play_stopped = true;
}

void fake_play_done(xiaotai_err_t error)
{
    CHECK(xiaotai_on_owner() && current_play_id && current_play.done);
    const xiaotai_sound_t *const sound = current_play.clips[0].sound;
    CHECK(sound && sound->size >= 44U && !memcmp(sound->data, "RIFF", 4U));
    const uint32_t id = current_play_id;
    current_play_id = 0;
    const xiaotai_media_play_t play = current_play;
    current_play = (xiaotai_media_play_t){0};
    play.done(id, error, play.ctx);
}

void fake_record_done(xiaotai_err_t error, uint32_t samples)
{
    CHECK(xiaotai_on_owner() && fake_capture_request.done && samples <= fake_capture_request.samples);
    /* Touch the lent buffer at the last possible moment; ASan would detect a
     * release at cancel instead of at done, even for a zero-length capture. */
    fake_capture_request.pcm[samples ? samples - 1 : 0] = 0x3322;
    const xiaotai_media_record_t request = fake_capture_request;
    fake_capture_request = (xiaotai_media_record_t){0};
    const uint32_t id = current_id;
    current_id = 0;
    request.done(id, error, samples, request.ctx);
}

uint32_t fake_taken;
void xiaotai_media_record_taken(uint32_t id, uint32_t samples)
{
    (void)id;
    if (samples > fake_taken) fake_taken = samples;
}

void xiaotai_media_record_stop(uint32_t id)
{
    CHECK(xiaotai_on_owner() && id == current_id && id);
    ++fake_stops;
    if (!fake_stop_holds) fake_record_done(XIAOTAI_ERR_INVALID_STATE, fake_recorded);
}
