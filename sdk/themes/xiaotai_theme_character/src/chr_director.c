// SPDX-License-Identifier: MIT
/* What the device tells the character. Events arrive on the owner task; the
 * character lives on the UI task. Between them sits a mailbox of plain atomic
 * words: the conditions that hold, the reaction that waits to play (and the
 * greeting), and what the test bench asked to see.
 *
 * Events are subscribed by name. The units that emit them (network, platform,
 * calls, an alarm, settings, gestures) are not dependencies of the character:
 * a product without one of them never emits its events, and the character
 * simply never reacts to them. tests/sdk/integration/character_events_test.c holds
 * the names against the headers that publish them. */
#include "chr_director.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static atomic_uint conditions, cues, cue_ms, shown, shown_until;
static atomic_bool alone;      /* the bench asked for painting on one core */
static atomic_int volume;
static int last_volume = -1;
static bool been_online;       /* since the start: the first time is part of the greeting */
static bool talking;           /* from where an AI talk is connected on, to its last word */
/* The sentences told of and not yet taken: one writer (the owner task), one reader (the UI task). Each is the mood in
 * the top byte and where it begins, plus 3, below. An answer's sentences come a few at a time at most. Those before
 * `sentence_over` are of a talk that is over (the writer moves it; the reader skips to it). */
enum { SENTENCES = 8 };
static atomic_uint sentences[SENTENCES], sentence_in, sentence_out, sentence_over;
static unsigned sentence_next(void)
{
    const unsigned out = atomic_load(&sentence_out), over = atomic_load(&sentence_over);
    return (int)(over - out) > 0 ? over : out;
}

static void hold(uint32_t bits) { atomic_fetch_or(&conditions, bits); }
static void drop(uint32_t bits) { atomic_fetch_and(&conditions, ~bits); }
_Static_assert(CHR_CUE_COUNT <= 32, "the waiting reactions are one bit each of a 32-bit word");
/* One reaction waits for the character, the last, and not for long: what she was not on the screen for is over
 * when she comes back. The greeting alone waits, until the first page she is on. */
enum { CUE_STALE_MS = 5000 };
static uint32_t clock_ms(void) { return (uint32_t)(xiaotai_time_us() / 1000); }
static void play(chr_cue_t cue)
{
    atomic_store(&cue_ms, clock_ms());
    atomic_fetch_and(&cues, 1U << CHR_CUE_GREET);
    atomic_fetch_or(&cues, 1U << cue);
}

/* What the network does in a talk is not shown then, nor afterwards, on whatever page the talk is. */
static void play_network(chr_cue_t cue)
{
    if (!talking) play(cue);
}

void chr_director_handle(const char *name, int32_t value, const char *text)
{
    const uint32_t now = atomic_load(&conditions);
    if (!strcmp(name, "system.started")) play(CHR_CUE_GREET);
    else if (!strcmp(name, "net.down")) {
        if (!(now & CHR_OFFLINE)) play_network(CHR_CUE_SHIVER);
        drop(CHR_CONNECTING);
        hold(CHR_OFFLINE);
    } else if (!strcmp(name, "net.up")) {
        drop(CHR_OFFLINE);
        /* A shiver at the loss that nobody saw is not played at the return. */
        atomic_fetch_and(&cues, ~(1U << CHR_CUE_SHIVER));
        if (xiaotai_module_present("platform")) hold(CHR_CONNECTING);
        else if (now & (CHR_OFFLINE | CHR_CONNECTING)) { drop(CHR_CONNECTING); play_network(CHR_CUE_CHEER); }
    } else if (!strcmp(name, "platform.online")) {
        if (been_online && (now & (CHR_OFFLINE | CHR_CONNECTING))) play_network(CHR_CUE_CHEER);
        been_online = true;
        drop(CHR_OFFLINE | CHR_CONNECTING);
    } else if (!strcmp(name, "platform.offline")) {
        if (!(now & CHR_OFFLINE)) hold(CHR_CONNECTING);
    } else if (!strcmp(name, "call.incoming")) hold(CHR_RINGING);
    else if (!strcmp(name, "call.connected")) { drop(CHR_RINGING); hold(CHR_IN_CALL); }
    else if (!strcmp(name, "call.ended")) drop(CHR_RINGING | CHR_IN_CALL);
    else if (!strcmp(name, "alarm.ringing")) {
        if (value) hold(CHR_ALARM);
        else { drop(CHR_ALARM); play(CHR_CUE_NOD); }
    } else if (!strcmp(name, "ai_talk.state")) {
        /* A talk is being connected; or it is over for the service, and its last sentence is still being
         * received or played. */
        talking = value != 0;
        if (value == CHR_DIRECTOR_TALK_CONNECTING) hold(CHR_STARTING);
        else drop(CHR_STARTING);
        if (value == CHR_DIRECTOR_TALK_ENDING) hold(CHR_PARTING);
        else drop(CHR_PARTING);
        if (!value) atomic_store(&sentence_over, atomic_load(&sentence_in));     /* what was told of it is over too */
    } else if (!strcmp(name, "companion.sentence")) {
        const unsigned in = atomic_load(&sentence_in);
        if (in - sentence_next() >= SENTENCES) return;   /* nobody takes them: she is not on the screen */
        const int32_t begin = value < CHR_SENTENCE_OWN ? -1 : value > 0xFFFFFC ? 0xFFFFFC : value;
        atomic_store(&sentences[in % SENTENCES], (uint32_t)chr_mood_find(text) << 24 | (uint32_t)(begin - CHR_SENTENCE_OWN));
        atomic_store(&sentence_in, in + 1U);
    } else if (!strcmp(name, "settings.changed")) {
        if (!strcmp(text, "audio.volume")) {
            /* Every setting is announced once at start: only a change is a reaction. */
            if (last_volume >= 0 && last_volume != value) {
                atomic_store(&volume, value);
                play(CHR_CUE_VOLUME);
            }
            last_volume = value;
        } else if (!strcmp(text, "audio.mic_muted")) {
            if (value) hold(CHR_MUTED);
            else drop(CHR_MUTED);
        }
    } else play(CHR_CUE_ERROR);   /* settings.refused, input.rejected, media.output_error */
}

static void on_event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    chr_director_handle(event->name, event->args.value, event->args.text);
}

static const char *const events[] = {
    "system.started", "net.down", "net.up", "platform.online", "platform.offline", "call.incoming", "call.connected",
    "call.ended", "alarm.ringing", "ai_talk.state", "settings.changed", "settings.refused", "input.rejected", "media.output_error",
    "companion.sentence",
};

const char *chr_director_event(size_t index)
{
    return index < sizeof(events) / sizeof(events[0]) ? events[index] : NULL;
}

uint32_t chr_director_conditions(void) { return atomic_load_explicit(&conditions, memory_order_relaxed); }

bool chr_director_take_sentence(int32_t *begin_ms, size_t *mood)
{
    const unsigned out = sentence_next();
    if (out == atomic_load(&sentence_in)) {
        atomic_store(&sentence_out, out);
        return false;
    }
    const uint32_t packed = atomic_load(&sentences[out % SENTENCES]);
    atomic_store(&sentence_out, out + 1U);
    *begin_ms = (int32_t)(packed & 0xFFFFFFU) + CHR_SENTENCE_OWN;
    *mood = packed >> 24;
    return true;
}

uint32_t chr_director_take_cues(int32_t *value, uint32_t now_ms)
{
    uint32_t taken = atomic_exchange(&cues, 0U);
    if (value) *value = atomic_load(&volume);
    /* (The step's time was read before it began: an event of the last moment is a little ahead of it.) */
    const int32_t age = (int32_t)(now_ms - atomic_load(&cue_ms));
    const bool stale = age > CUE_STALE_MS || age < -CUE_STALE_MS;
    if (stale || taken & 1U << CHR_CUE_GREET) taken &= 1U << CHR_CUE_GREET;
    return taken;
}

bool chr_director_shown(uint32_t now_ms, uint32_t *packed)
{
    unsigned until = atomic_load_explicit(&shown_until, memory_order_relaxed);
    if (!until) return false;
    if ((int32_t)(until - now_ms) <= 0) {
        /* Over. Left as it is, the difference would turn round in a month. */
        (void)atomic_compare_exchange_strong(&shown_until, &until, 0U);
        return false;
    }
    *packed = atomic_load_explicit(&shown, memory_order_relaxed);
    return true;
}

void chr_director_cue(chr_cue_t cue, int32_t value)
{
    if (cue <= CHR_CUE_NONE || cue >= CHR_CUE_COUNT) return;
    if (cue == CHR_CUE_VOLUME) atomic_store(&volume, value);
    play(cue);
}

void chr_director_set(uint32_t bits, bool on)
{
    if (on) hold(bits);
    else drop(bits);
}

/* -------------------------------------------------------------- AT+CHARACTER */

static xiaotai_err_t state_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(reply, "+CHARACTER: conditions=%u pending=%u", (unsigned)atomic_load(&conditions),
                      (unsigned)atomic_load(&cues));
    return XIAOTAI_OK;
}

static xiaotai_err_t cue_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    const chr_cue_t cue = chr_cue_find(argv[0]);
    uint32_t value = 0;
    if (cue == CHR_CUE_NONE || (argc > 1 && !xiaotai_at_parse_u32(argv[1], 0, 100, &value))) return XIAOTAI_ERR_INVALID_ARG;
    chr_director_cue(cue, (int32_t)value);
    return XIAOTAI_OK;
}

/* SHOW <mood> [<phase 0..3> [<conditions> [<seconds>]]]: what the bench wants to look at. */
static xiaotai_err_t show_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    uint32_t phase = 0, bits = 0, seconds = 8;
    if (!chr_mood_name(0) || strcmp(chr_mood_name(chr_mood_find(argv[0])), argv[0]) ||
        (argc > 1 && !xiaotai_at_parse_u32(argv[1], 0, 3, &phase)) ||
        (argc > 2 && !xiaotai_at_parse_u32(argv[2], 0, 255, &bits)) ||
        (argc > 3 && !xiaotai_at_parse_u32(argv[3], 0, 600, &seconds))) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&shown, (uint32_t)chr_mood_find(argv[0]) | phase << 8 | bits << 16);
    const uint32_t until = (uint32_t)(xiaotai_time_us() / 1000) + seconds * 1000U;
    atomic_store(&shown_until, seconds ? (until ? until : 1U) : 0U);
    return XIAOTAI_OK;
}

bool chr_director_share(void) { return !atomic_load_explicit(&alone, memory_order_relaxed); }

/* SHARE <0|1>: painting on one core or shared with the second, to measure what sharing gives. */
static xiaotai_err_t share_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)reply;
    uint32_t on = 0;
    if (!xiaotai_at_parse_u32(argv[0], 0, 1, &on)) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&alone, !on);
    return XIAOTAI_OK;
}

xiaotai_err_t chr_director_init(void)
{
    atomic_store(&conditions, xiaotai_module_present("platform") ? CHR_CONNECTING : 0U);
    atomic_store(&cues, 0U);
    atomic_store(&shown_until, 0U);
    atomic_store(&alone, false);
    atomic_store(&sentence_in, 0U);
    atomic_store(&sentence_out, 0U);
    last_volume = -1;
    been_online = talking = false;
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe(events[i], on_event, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    static const xiaotai_at_verb_t verbs[] = {
        {.run = state_verb},
        {.word = "CUE", .min_args = 1, .max_args = 2, .run = cue_verb, .usage = "<name> [<value>]"},
        {.word = "SHOW", .min_args = 1, .max_args = 4, .run = show_verb,
         .usage = "<mood> [<phase> [<conditions> [<seconds>]]]"},
        {.word = "SHARE", .min_args = 1, .max_args = 1, .run = share_verb, .usage = "<0|1>"},
    };
    return xiaotai_at_verbs_register("AT+CHARACTER", verbs, sizeof(verbs) / sizeof(verbs[0]));
}
