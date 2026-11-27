// SPDX-License-Identifier: MIT
/* theme_character_events: the on-screen character subscribes to the device's
 * events by their text, because the units that emit them are not its
 * dependencies. Here, where every unit may be seen, each name it subscribes
 * to is held against the header that publishes it, and the two setting names
 * it reads against the settings module's. A renamed event fails this test
 * instead of silently never reaching her. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chr.h"
#include "chr_director.h"
#include "xiaotai/apps/alarm.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/framework.h"
#include "xiaotai/interaction.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"

/* A sentence's value that is no sentence (an expression begins, it is over) means the same to the application that
 * sends it and to the character. */
_Static_assert(XIAOTAI_APP_COMPANION_SENTENCE_NOW == CHR_SENTENCE_NOW, "an expression begins");
_Static_assert(XIAOTAI_APP_COMPANION_SENTENCE_OWN == CHR_SENTENCE_OWN, "an expression is over");

int main(void)
{
    /* In the order the character subscribes (chr_director.c). */
    static const char *const published[] = {
        XIAOTAI_EVENT_STARTED, XIAOTAI_EVENT_NET_DOWN, XIAOTAI_EVENT_NET_UP, XIAOTAI_EVENT_PLATFORM_ONLINE,
        XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_CONNECTED,
        XIAOTAI_EVENT_CALL_ENDED, XIAOTAI_EVENT_APP_ALARM_RINGING, XIAOTAI_EVENT_AI_TALK_STATE, XIAOTAI_EVENT_SETTINGS_CHANGED,
        XIAOTAI_EVENT_SETTINGS_REFUSED, XIAOTAI_EVENT_INPUT_REJECTED, XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR,
        XIAOTAI_EVENT_APP_COMPANION_SENTENCE,
    };
    const size_t count = sizeof(published) / sizeof(published[0]);
    int failures = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *subscribed = chr_director_event(i);
        if (!subscribed || strcmp(subscribed, published[i]) != 0) {
            fprintf(stderr, "event %zu: the character subscribes to \"%s\", the unit publishes \"%s\"\n", i,
                    subscribed ? subscribed : "(nothing)", published[i]);
            ++failures;
        }
    }
    if (chr_director_event(count) != NULL) {
        fprintf(stderr, "the character subscribes to \"%s\", which this test does not know\n", chr_director_event(count));
        ++failures;
    }
    if (strcmp(XIAOTAI_SETTING_AUDIO_VOLUME, "audio.volume") != 0 || strcmp(XIAOTAI_SETTING_AUDIO_MIC_MUTED, "audio.mic_muted") != 0) {
        fprintf(stderr, "the setting names the character reads have changed\n");
        ++failures;
    }
    if ((int)CHR_DIRECTOR_TALK_ENDING != (int)XIAOTAI_AI_TALK_ENDING ||
        (int)CHR_DIRECTOR_TALK_CONNECTING != (int)XIAOTAI_AI_TALK_CONNECTING) {
        fprintf(stderr, "a phase of a talk that the character reads has another number\n");
        ++failures;
    }
    if (failures) {
        fprintf(stderr, "FAIL: theme_character_events: %d names differ\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_character_events\n");
    return EXIT_SUCCESS;
}
