// SPDX-License-Identifier: MIT
#include "audio_fakes.h"

#include <assert.h>

#include "xiaotai/framework.h"

companion_audio_fixture_t audio_fixture;

void xiaotai_media_info(xiaotai_media_info_t *info) { *info = audio_fixture.info; }

xiaotai_err_t xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id)
{
    assert(xiaotai_on_owner() && !audio_fixture.record.done && !audio_fixture.play.done);
    ++audio_fixture.records;
    if (audio_fixture.record_error) return audio_fixture.record_error;
    audio_fixture.record = *request;
    audio_fixture.status.recording = *id = ++audio_fixture.serial;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    assert(xiaotai_on_owner() && !audio_fixture.record.done && !audio_fixture.play.done);
    ++audio_fixture.plays;
    if (audio_fixture.play_error) return audio_fixture.play_error;
    assert(request->clip_count == 1 && request->clips[0].sound);
    audio_fixture.play = *request;
    audio_fixture.status.playing = *id = ++audio_fixture.serial;
    return XIAOTAI_OK;
}

void xiaotai_media_record_taken(uint32_t id, uint32_t samples)
{
    (void)id;
    (void)samples;
}

void xiaotai_media_record_stop(uint32_t id)
{
    assert(xiaotai_on_owner() && id && id == audio_fixture.status.recording);
    ++audio_fixture.record_stops;
}

void xiaotai_media_stop(uint32_t id)
{
    assert(xiaotai_on_owner() && id && id == audio_fixture.status.playing);
    ++audio_fixture.play_stops;
}

void companion_fixture_record_done(xiaotai_err_t result, uint32_t samples)
{
    assert(xiaotai_on_owner() && samples <= audio_fixture.record.samples);
    const xiaotai_media_record_t record = audio_fixture.record;
    const uint32_t id = audio_fixture.status.recording;
    audio_fixture.record = (xiaotai_media_record_t){0};
    audio_fixture.status.recording = 0;
    assert(id && record.done);
    record.done(id, result, samples, record.ctx);
}

void companion_fixture_play_done(xiaotai_err_t result)
{
    assert(xiaotai_on_owner());
    const xiaotai_media_play_t play = audio_fixture.play;
    const uint32_t id = audio_fixture.status.playing;
    audio_fixture.play = (xiaotai_media_play_t){0};
    audio_fixture.status.playing = 0;
    assert(id && play.done);
    play.done(id, result, play.ctx);
}
