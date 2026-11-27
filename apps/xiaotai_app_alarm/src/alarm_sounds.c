// SPDX-License-Identifier: MIT
#include "alarm_sounds.h"

#include <stdint.h>
#include "xiaotai/apps/alarm.h"
#include "xiaotai/sounds.h"

#include "alarm_sounds_data.inc"

xiaotai_err_t alarm_sounds_init(void)
{
    static const xiaotai_sound_t sounds[] = {
        {.name = XIAOTAI_APP_ALARM_SOUND_ALARM, .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
         .rate_hz = 8000, .channels = 1, .data = alarm_bell, .size = sizeof(alarm_bell)},
        {.name = XIAOTAI_APP_ALARM_SOUND_TIMER, .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
         .rate_hz = 8000, .channels = 1, .data = alarm_timer, .size = sizeof(alarm_timer)},
    };
    static const xiaotai_sound_pack_t pack = {.name = "alarm", .sounds = sounds,
                                              .count = sizeof(sounds) / sizeof(sounds[0])};
    return xiaotai_sounds_register(&pack);
}
