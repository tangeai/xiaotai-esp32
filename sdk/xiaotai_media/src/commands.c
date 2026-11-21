// SPDX-License-Identifier: MIT
#include "diag.h"
#include "trace.h"

static xiaotai_err_t audio_query(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    media_diag_audio(reply);
    return XIAOTAI_OK;
}

xiaotai_err_t media_commands_register(void)
{
    static const xiaotai_at_verb_t query[] = {{.run = audio_query}};
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+AUDIO", query, sizeof(query) / sizeof(*query));
#if XIAOTAI_MEDIA_TEST_COMMANDS
    if (err == XIAOTAI_OK) err = media_at_test_register();
#endif
#if XIAOTAI_MEDIA_SOUND_COMMANDS
    if (err == XIAOTAI_OK) err = media_at_sound_register();
#endif
#if XIAOTAI_MEDIA_TRACE
    if (err == XIAOTAI_OK) err = media_trace_register();
#endif
    return err;
}
