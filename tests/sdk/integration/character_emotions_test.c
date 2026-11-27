// SPDX-License-Identifier: MIT
/* theme_character_emotions: from the AI service's ten emotion words, by way of
 * the companion's names for them, to the character: each word is a mood of its
 * own, and none is lost on the way. Every expression of the companion's list
 * is a mood too. */
#include <stdio.h>
#include <string.h>

#include "chr.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/view.h"

int main(void)
{
    static const struct { const char *word, *mood; } words[] = {
        {"中性", "neutral"}, {"开心", "happy"}, {"兴奋", "excited"}, {"温和", "gentle"}, {"安慰", "comfort"},
        {"思考", "thinking"}, {"惊讶", "surprised"}, {"严肃", "serious"}, {"困倦", "sleepy"}, {"困惑", "confused"},
    };
    bool seen[CHR_MOOD_COUNT] = {0};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        const size_t mood = chr_mood_find(xiaotai_app_companion_emotion_face(words[i].word));
        if (strcmp(chr_mood_name(mood), words[i].mood) || seen[mood]) {
            fprintf(stderr, "%s is shown as %s\n", words[i].word, chr_mood_name(mood));
            return 1;
        }
        seen[mood] = true;
    }
    /* The states that pages name; she says she is offline in a way of her own (CHR_OFFLINE). */
    static const struct { const char *state, *mood; } states[] = {
        {XIAOTAI_EMOTION_RINGING, "surprised"}, {XIAOTAI_EMOTION_DIALING, "listening"}, {XIAOTAI_EMOTION_OFFLINE, "neutral"},
        {XIAOTAI_EMOTION_ERROR, "sad"},
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
        if (strcmp(chr_mood_name(chr_mood_find(states[i].state)), states[i].mood)) {
            fprintf(stderr, "the state %s is shown as %s\n", states[i].state, chr_mood_name(chr_mood_find(states[i].state)));
            return 1;
        }
    for (size_t i = 0; i < XIAOTAI_APP_COMPANION_EMOTIONS; ++i) {
        const char *name = xiaotai_app_companion_emotion(i);
        if (strcmp(chr_mood_name(chr_mood_find(name)), name)) {
            fprintf(stderr, "expression %s is shown as %s\n", name, chr_mood_name(chr_mood_find(name)));
            return 1;
        }
    }
    if (chr_mood_find(xiaotai_app_companion_emotion_face("")) != CHR_MOOD_NEUTRAL ||
        chr_mood_find(xiaotai_app_companion_emotion_face("未知")) != CHR_MOOD_NEUTRAL) {
        fprintf(stderr, "no word, or an unknown one, is not neutral\n");
        return 1;
    }
    printf("PASS: theme_character_emotions\n");
    return 0;
}
