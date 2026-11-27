// SPDX-License-Identifier: MIT
/* The door's own sounds (table: scripts/generate_door_sounds.py). A product
 * replaces one by registering a later pack with the same name. */
#include "door_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/media.h"
#include "xiaotai/sounds.h"

#include "door_sounds_data.inc"

xiaotai_err_t door_init_sounds(void)
{
    static const xiaotai_sound_t sounds[] = {
        {.name = XIAOTAI_APP_DOOR_SOUND_OPENED, .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
         .rate_hz = 8000, .channels = 1, .data = door_opened, .size = sizeof(door_opened)},
        {.name = XIAOTAI_APP_DOOR_SOUND_KEY, .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
         .rate_hz = 8000, .channels = 1, .data = door_key, .size = sizeof(door_key)},
    };
    static const xiaotai_sound_pack_t pack = {.name = "door", .sounds = sounds, .count = sizeof(sounds) / sizeof(sounds[0])};
    return xiaotai_sounds_register(&pack);
}

void door_sound_key(void) { (void)xiaotai_media_play(XIAOTAI_APP_DOOR_SOUND_KEY, XIAOTAI_PLAY_CUE); }

/* Over the call that runs: it goes on. */
void door_sound_opened(void)
{
    xiaotai_media_play_t play = {.clips = {{.name = XIAOTAI_APP_DOOR_SOUND_OPENED}}, .clip_count = 1,
                                 .priority = XIAOTAI_PLAY_URGENT, .flags = XIAOTAI_PLAY_OVER_SESSION};
    xiaotai_err_t err = xiaotai_media_play_clips(&play, NULL);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) {     /* a product without a mixer is heard outside a call only */
        play.flags = 0;
        err = xiaotai_media_play_clips(&play, NULL);
    }
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("door", "sound.opened", 0, err);
}
