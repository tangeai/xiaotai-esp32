// SPDX-License-Identifier: MIT
#include "recognition_playback.h"

#define RC(name) XIAOTAI_RECOGNITION_##name
#define RJ(name) XIAOTAI_RECOGNITION_JOURNAL_##name

bool rc_speech_skippable(const rc_item_t *item)
{
    return item->state == RC(ITEM_FAILED) || item->state == RC(ITEM_CANCELLED) ||
        (item->state == RC(ITEM_SUCCEEDED) && (item->output == RC(OUTPUT_TEXT) ||
         item->speech == RC(SPEECH_FAILED) || item->speech == RC(SPEECH_SKIPPED)));
}

bool rc_speech_ready(const rc_item_t *item)
{
    return item && item->state == RC(ITEM_SUCCEEDED) && item->output != RC(OUTPUT_TEXT) &&
        item->speech == RC(SPEECH_READY);
}

rc_order_t rc_choose_speech(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                            char id[XIAOTAI_RECOGNITION_ID_MAX])
{
    for (size_t i = 0; i < task->info.items; ++i) {
        const rc_item_t *item = &task->items[i];
        if (rc_task_string(task, item->id, id, XIAOTAI_RECOGNITION_ID_MAX, NULL) != XIAOTAI_OK)
            return RC_ORDER_BLOCKED;
        const xiaotai_recognition_journal_item_state_t consumed = rc_item_consumption(mirror, id);
        if (consumed == RJ(PLAYED) || consumed == RJ(DISMISSED)) continue;
        if (consumed == RJ(PLAY_STARTED)) return RC_ORDER_BLOCKED;
        if (rc_speech_skippable(item)) continue;
        return rc_speech_ready(item) ? RC_ORDER_READY : RC_ORDER_WAIT;
    }
    id[0] = '\0';
    return RC_ORDER_NONE;
}

xiaotai_err_t rc_playback_wav(const uint8_t *data, size_t size, xiaotai_sound_t *sound, uint32_t *duration_ms)
{
    *sound = (xiaotai_sound_t){.encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_WAV,
        .data = data, .size = size};
    xiaotai_sound_info_t info;
    const xiaotai_err_t error = xiaotai_sounds_inspect(sound, &info);
    if (error == XIAOTAI_OK) *duration_ms = info.duration_ms;
    return error;
}
