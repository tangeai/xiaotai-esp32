// SPDX-License-Identifier: MIT
/* theme_character_director: what the events of the device do to the
 * character: each raises or clears the condition, or queues the reaction, it
 * should. Events are written here as the character knows them, by their
 * text; that the texts are the ones the emitting units publish is checked
 * where those units may be seen (tests/sdk/integration/character_events_test.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chr.h"
#include "chr_director.h"
#include "chr_pack_host.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static uint32_t clock_ms(void) { return (uint32_t)(xiaotai_time_us() / 1000); }
/* What waits for her, as she takes it some time after the last event. */
static uint32_t waiting(uint32_t later_ms) { return chr_director_take_cues(NULL, clock_ms() + later_ms); }
static uint32_t bit(chr_cue_t cue) { return 1U << cue; }
static bool queued(chr_cue_t cue) { return (waiting(0) & bit(cue)) != 0; }

/* She is not on the screen all the time. Of what happens meanwhile the last thing waits for her, and not
 * for long; the greeting waits until she is there. */
static void test_waiting(void)
{
    chr_director_set(~0U, false);
    (void)waiting(0);
    /* Whatever else happens before the first page she is on, she greets. */
    chr_director_handle("system.started", 0, "");
    chr_director_handle("input.rejected", 0, "talk.start");
    CHECK(waiting(600000) == bit(CHR_CUE_GREET) && waiting(0) == 0);
    chr_director_handle("system.started", 0, "");
    chr_director_handle("input.rejected", 0, "talk.start");
    CHECK(waiting(0) == bit(CHR_CUE_GREET) && waiting(0) == 0);
    /* The last of several. */
    chr_director_handle("input.rejected", 0, "talk.start");
    chr_director_handle("alarm.ringing", 0, "");
    CHECK(waiting(0) == bit(CHR_CUE_NOD) && waiting(0) == 0);
    /* For some seconds; then it is over. */
    chr_director_cue(CHR_CUE_STRETCH, 0);
    CHECK(waiting(4000) == bit(CHR_CUE_STRETCH));
    chr_director_cue(CHR_CUE_STRETCH, 0);
    CHECK(waiting(6000) == 0 && waiting(0) == 0);
    chr_director_cue(CHR_CUE_STRETCH, 0);
    CHECK(waiting(24 * 3600 * 1000) == 0);
    chr_director_cue(CHR_CUE_STRETCH, 0);
    CHECK(waiting(30U * 24U * 3600U * 1000U) == 0);
    /* A step that read its time just before the event takes it all the same. */
    chr_director_cue(CHR_CUE_STRETCH, 0);
    CHECK(waiting((uint32_t)-20) == bit(CHR_CUE_STRETCH));
}

static void no_line(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
}

/* On a device with a service: the director as the framework brings it up. */
static void test_device(void)
{
    static const xiaotai_module_t platform = {.name = "platform", .init = chr_director_init};
    static const xiaotai_module_t *const modules[] = {&platform};
    static const xiaotai_application_t app = {.name = "app"};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1, .applications = apps,
                                               .application_count = 1, .home = "app"};
    const xiaotai_reply_t reply = {.write = no_line};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    /* The first time the service is reached is part of the start, which she greets. */
    CHECK(xiaotai_command_run("AT+CHARACTER", &reply, 1000) == XIAOTAI_OK);
    CHECK(chr_director_conditions() == CHR_CONNECTING && waiting(0) == bit(CHR_CUE_GREET));
    chr_director_handle("net.up", 0, "");
    CHECK(chr_director_conditions() == CHR_CONNECTING);
    chr_director_handle("platform.online", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);
    /* Lost before her eyes, and found again. */
    chr_director_handle("net.down", 0, "");
    CHECK(chr_director_conditions() == CHR_OFFLINE && waiting(0) == bit(CHR_CUE_SHIVER));
    chr_director_handle("net.up", 0, "");
    CHECK(chr_director_conditions() == CHR_CONNECTING && waiting(0) == 0);
    chr_director_handle("platform.online", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == bit(CHR_CUE_CHEER));
    /* Lost behind another page, which went with the loss: the return does not begin with a frown. */
    chr_director_handle("net.down", 0, "");
    chr_director_handle("net.up", 0, "");
    CHECK(waiting(0) == 0);
    chr_director_handle("platform.online", 0, "");
    CHECK(waiting(0) == bit(CHR_CUE_CHEER));
    /* In a talk, on whatever page it is, neither is hers to show, then or afterwards. */
    chr_director_handle("ai_talk.state", 4, "");
    chr_director_handle("net.down", 0, "");
    chr_director_handle("net.up", 0, "");
    chr_director_handle("platform.online", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);
    chr_director_handle("platform.offline", 0, "");
    chr_director_handle("ai_talk.state", CHR_DIRECTOR_TALK_ENDING, "");
    chr_director_handle("platform.online", 0, "");
    CHECK(waiting(0) == 0);
    chr_director_handle("ai_talk.state", 0, "");
    chr_director_handle("net.down", 0, "");
    CHECK(waiting(0) == bit(CHR_CUE_SHIVER));
    chr_director_handle("net.up", 0, "");
    chr_director_handle("platform.online", 0, "");
    CHECK(waiting(0) == bit(CHR_CUE_CHEER) && chr_director_conditions() == 0);
    /* What the bench looks at is over when its time is, for good: not again when the clock's difference
     * has turned round. */
    uint32_t packed = 0;
    CHECK(xiaotai_command_run("AT+CHARACTER SHOW happy 0 0 1", &reply, 1000) == XIAOTAI_OK);
    const uint32_t now = clock_ms();
    CHECK(chr_director_shown(now, &packed) && (packed & 0xFFU) == chr_mood_find("happy"));
    CHECK(!chr_director_shown(now + 1500U, &packed));
    CHECK(!chr_director_shown(now + 5000U + 0x80000000U, &packed) && !chr_director_shown(now, &packed));
    xiaotai_framework_reset();
}

/* The end of a talk from the device to the mouth: the service is done with it (its page shows none any
 * more), but the last sentence is still being received and played. */
static void test_last_words(void)
{
    size_t size = 0;
    const void *pack = chr_host_pack(&size);
    chr_t *c = malloc(chr_size());
    chr_rect_t damage[CHR_DAMAGE_MAX];
    chr_info_t info;
    CHECK(c && chr_init(c, 1024, 600, 3, pack, size));
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_director_handle("ai_talk.state", 4, "");
    for (int i = 0; i < 60; ++i) {
        chr_set_conditions(c, chr_director_conditions());
        chr_set_envelope(c, 0, 9000);
        (void)chr_step(c, 33, damage);
    }
    /* ENDING: the page goes back to none, the voice goes on, with a gap longer than a pause in it. */
    chr_director_handle("ai_talk.state", CHR_DIRECTOR_TALK_ENDING, "");
    chr_set_phase(c, CHR_PHASE_IDLE);
    unsigned open = 0, shut = 0;
    bool waved = false;
    for (int i = 0; i < 120; ++i) {
        const bool voice = i < 40 || i >= 80;
        chr_set_conditions(c, chr_director_conditions());
        chr_set_envelope(c, 0, voice ? 9000 : 0);
        (void)chr_step(c, 33, damage);
        chr_inspect(c, &info);
        if (voice && (i < 40 || i > 82)) open += info.mouth_open > 0;
        if (i > 50 && i < 80) shut += info.mouth_open == 0;
        waved |= info.cue == CHR_CUE_BYE;
    }
    CHECK(open == 40U + 37U && shut == 29U && !waved);
    /* Over: she is still when the voice is, and then says goodbye. */
    chr_director_handle("ai_talk.state", 0, "");
    for (int i = 0; i < 30; ++i) {
        chr_set_conditions(c, chr_director_conditions());
        chr_set_envelope(c, 0, 0);
        (void)chr_step(c, 33, damage);
        chr_inspect(c, &info);
        waved |= info.cue == CHR_CUE_BYE;
    }
    CHECK(waved && info.mouth_open == 0 && !info.parting);
    free(c);
}

/* The sentences an answer tells of wait for her in their order, with where each begins and what it brings, the
 * beginning and the end of an expression the AI asked for too; those of a talk that is over do not. */
static void test_sentences(void)
{
    int32_t begin = 0;
    size_t mood = 0;
    while (chr_director_take_sentence(&begin, &mood)) {}
    static const int32_t begins[] = {0, 1500, -1, CHR_SENTENCE_NOW, CHR_SENTENCE_OWN, 600000};
    static const char *const faces[] = {"comfort", "serious", "neutral", "happy", "gentle", "excited"};
    for (size_t i = 0; i < 6; ++i) chr_director_handle("companion.sentence", begins[i], faces[i]);
    for (size_t i = 0; i < 6; ++i) {
        CHECK(chr_director_take_sentence(&begin, &mood));
        CHECK(begin == begins[i] && mood == chr_mood_find(faces[i]));
    }
    CHECK(!chr_director_take_sentence(&begin, &mood));
    chr_director_handle("companion.sentence", -7, "happy");
    CHECK(chr_director_take_sentence(&begin, &mood) && begin == -1);
    /* Told while she was not on the screen, then the talk is over: none is left for her when she is back. The next
     * talk's are hers. */
    chr_director_handle("ai_talk.state", 4, "");
    chr_director_handle("companion.sentence", 0, "comfort");
    chr_director_handle("companion.sentence", 1500, "serious");
    chr_director_handle("ai_talk.state", CHR_DIRECTOR_TALK_ENDING, "");
    chr_director_handle("ai_talk.state", 0, "");
    CHECK(!chr_director_take_sentence(&begin, &mood));
    for (int i = 0; i < 10; ++i) chr_director_handle("companion.sentence", i, "happy");
    for (int i = 0; i < 8; ++i) CHECK(chr_director_take_sentence(&begin, &mood) && begin == i);
    CHECK(!chr_director_take_sentence(&begin, &mood));
    chr_director_handle("ai_talk.state", 0, "");
    chr_director_handle("companion.sentence", 0, "serious");
    CHECK(chr_director_take_sentence(&begin, &mood) && mood == chr_mood_find("serious"));
    CHECK(!chr_director_take_sentence(&begin, &mood));
}

int main(void)
{
    test_sentences();
    test_last_words();
    /* Every subscribed name is told apart from the others, and the list ends. */
    size_t count = 0;
    while (chr_director_event(count)) ++count;
    CHECK(count >= 14);
    for (size_t i = 0; i < count; ++i)
        for (size_t k = i + 1; k < count; ++k) CHECK(strcmp(chr_director_event(i), chr_director_event(k)) != 0);
    CHECK(!strcmp(chr_director_event(0), XIAOTAI_EVENT_STARTED));

    chr_director_set(~0U, false);
    (void)waiting(0);
    chr_director_handle(XIAOTAI_EVENT_STARTED, 0, "");
    CHECK(queued(CHR_CUE_GREET));

    /* Network: down is offline and a shiver, online again cheers, the service going away is connecting again. */
    chr_director_set(CHR_CONNECTING, true);
    chr_director_handle("platform.online", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);
    chr_director_handle("net.down", 0, "");
    CHECK(chr_director_conditions() == CHR_OFFLINE && queued(CHR_CUE_SHIVER));
    chr_director_handle("net.down", 0, "");
    CHECK(!queued(CHR_CUE_SHIVER));
    chr_director_set(CHR_OFFLINE, false);
    chr_director_set(CHR_CONNECTING, true);
    chr_director_handle("platform.online", 0, "");
    CHECK(chr_director_conditions() == 0 && queued(CHR_CUE_CHEER));
    chr_director_handle("platform.online", 0, "");
    CHECK(!queued(CHR_CUE_CHEER));
    chr_director_handle("platform.offline", 0, "");
    CHECK(chr_director_conditions() == CHR_CONNECTING);
    chr_director_set(~0U, false);

    /* A talk is connected and runs; the service is done with it, whose last words are still heard; then it is over. */
    chr_director_handle("ai_talk.state", CHR_DIRECTOR_TALK_CONNECTING, "");
    CHECK(chr_director_conditions() == CHR_STARTING);
    chr_director_handle("ai_talk.state", 4, "");
    CHECK(chr_director_conditions() == 0);
    chr_director_handle("ai_talk.state", CHR_DIRECTOR_TALK_ENDING, "");
    CHECK(chr_director_conditions() == CHR_PARTING);
    chr_director_handle("ai_talk.state", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);

    /* A call rings, connects and ends, and is not waved at (she was not there); an alarm rings and stops. */
    chr_director_handle("call.incoming", 0, "peer");
    CHECK(chr_director_conditions() == CHR_RINGING);
    chr_director_handle("call.connected", 0, "");
    CHECK(chr_director_conditions() == CHR_IN_CALL);
    chr_director_handle("call.ended", 0, "");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);
    chr_director_handle("call.incoming", 0, "peer");
    chr_director_handle("call.ended", 0, "missed");
    CHECK(chr_director_conditions() == 0 && waiting(0) == 0);
    chr_director_handle("alarm.ringing", 1, "wake up");
    CHECK(chr_director_conditions() == CHR_ALARM);
    chr_director_handle("alarm.ringing", 0, "wake up");
    CHECK(chr_director_conditions() == 0 && queued(CHR_CUE_NOD));

    /* The volume announced at start is not a reaction; a change is, with its value. */
    chr_director_handle("settings.changed", 50, "audio.volume");
    CHECK(!queued(CHR_CUE_VOLUME));
    chr_director_handle("settings.changed", 80, "audio.volume");
    int32_t value = 0;
    CHECK(chr_director_take_cues(&value, clock_ms()) == bit(CHR_CUE_VOLUME) && value == 80);
    chr_director_handle("settings.changed", 80, "audio.volume");
    CHECK(!queued(CHR_CUE_VOLUME));
    chr_director_handle("settings.changed", 1, "audio.mic_muted");
    CHECK(chr_director_conditions() == CHR_MUTED);
    chr_director_handle("settings.changed", 0, "audio.mic_muted");
    CHECK(chr_director_conditions() == 0);
    chr_director_handle("settings.changed", 3, "screen.sleep");
    CHECK(waiting(0) == 0);
    chr_director_handle("input.rejected", 0, "talk.start");
    CHECK(queued(CHR_CUE_ERROR));
    chr_director_handle("media.output_error", 0, "volume");
    CHECK(queued(CHR_CUE_ERROR));

    /* The bench: a cue by number, and no mood held unless it asked. */
    chr_director_cue(CHR_CUE_VOLUME, 30);
    CHECK(chr_director_take_cues(&value, clock_ms()) == bit(CHR_CUE_VOLUME) && value == 30);
    chr_director_cue(CHR_CUE_COUNT, 0);
    CHECK(waiting(0) == 0);
    uint32_t packed = 0;
    CHECK(!chr_director_shown(1000, &packed));
    test_waiting();
    test_device();

    if (failures) {
        fprintf(stderr, "FAIL: theme_character_director: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_character_director\n");
    return EXIT_SUCCESS;
}
