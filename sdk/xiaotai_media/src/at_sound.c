// SPDX-License-Identifier: MIT
#include "diag.h"
#include "xiaotai/media_config.h"

#if XIAOTAI_MEDIA_SOUND_COMMANDS
#include <string.h>

#include "xiaotai/sounds.h"

static uint32_t s_preview;

void media_at_sound_detach(void) { s_preview = 0; }

static void preview_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    (void)result; (void)ctx;
    if (s_preview == id) s_preview = 0;
    media_diag_diagnostic_end(false, id);
}

static xiaotai_err_t xiaotai_media_at_sound_list(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    const size_t count = xiaotai_sounds_count();
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_sound_t *sound = xiaotai_sounds_at(i);
        if (!sound) continue;
        const char *kind = !strncmp(sound->name, "ring.", 5) ? "ring" :
            !strncmp(sound->name, "guide.", 6) || !strncmp(sound->name, "digit.", 6) ? "guide" : "prompt";
        xiaotai_at_printf(reply, "+SOUND: name=%s kind=%s", sound->name, kind);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t xiaotai_media_at_sound_play(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc == 2 && !xiaotai_at_is_digits(argv[1], 6)) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "GUIDE_DIGITS");
    }
    const xiaotai_sound_t *sound = xiaotai_sounds_find(argv[0]);
    if (!sound) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_media_play_t request = {.clip_count = 1, .priority = XIAOTAI_PLAY_URGENT, .done = preview_done};
    request.clips[0].name = sound->name;
    if (argc == 2) {
        for (size_t i = 0; i < 6; ++i) {
            request.clips[request.clip_count++].name = xiaotai_sounds_digit((unsigned)(argv[1][i] - '0'));
        }
    }
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_play_clips(&request, &id);
    if (err == XIAOTAI_OK) {
        s_preview = id;
        media_diag_diagnostic_begin(false, id);
    }
    return err;
}

static xiaotai_err_t xiaotai_media_at_sound_stop(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    if (s_preview) xiaotai_media_stop(s_preview);
    return XIAOTAI_OK;
}

xiaotai_err_t media_at_sound_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = xiaotai_media_at_sound_list},
        {.word = "PLAY", .min_args = 1, .max_args = 2, .run = xiaotai_media_at_sound_play, .usage = "<name> [<6 digits>]"},
        {.word = "STOP", .run = xiaotai_media_at_sound_stop},
    };
    return xiaotai_at_verbs_register("AT+SOUND", verbs, sizeof(verbs) / sizeof(*verbs));
}
#endif
