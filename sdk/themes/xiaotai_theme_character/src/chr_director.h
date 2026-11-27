// SPDX-License-Identifier: MIT
#pragma once
/* The mailbox between the device's events (owner task) and the character (UI
 * task), and the AT+CHARACTER command of the test bench. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chr.h"
#include "xiaotai/port/err.h"

/* [init] Subscribes to the events and registers the command. */
xiaotai_err_t chr_director_init(void);
/* [any] The conditions that hold now (CHR_OFFLINE, ...). */
uint32_t chr_director_conditions(void);
/* [ui] The reaction to what has happened since the last call, as the bit of its chr_cue_t, none when it is
 * some seconds old; the greeting does not grow old. *value is the volume for CHR_CUE_VOLUME. `now_ms` is
 * the clock of the scene (xiaotai_time_us() in milliseconds). */
uint32_t chr_director_take_cues(int32_t *value, uint32_t now_ms);
/* [ui] True while the bench holds a mood: mood | phase << 8 | conditions << 16. */
bool chr_director_shown(uint32_t now_ms, uint32_t *packed);
/* The phase of an AI talk whose service is done (xiaotai_ai_talk_phase_t), as "ai_talk.state" carries it. */
enum { CHR_DIRECTOR_TALK_CONNECTING = 1, CHR_DIRECTOR_TALK_ENDING = 5 };
/* [owner] One event: its name, value and text. */
void chr_director_handle(const char *name, int32_t value, const char *text);
/* [any] What an event does, for callers that have no event (tests, the bench). */
void chr_director_cue(chr_cue_t cue, int32_t value);
void chr_director_set(uint32_t conditions, bool on);
/* The subscribed event names, for the test that holds them against the headers; NULL past the end. */
const char *chr_director_event(size_t index);
/* [ui] Microphone and speaker loudness, 0..32768 (with_media.c). False in a product that has no
 * loudness to follow (media_none.c). */
bool chr_director_loudness(uint16_t *microphone, uint16_t *speaker);
/* [ui] The speaker's loudness as it will be heard ahead_ms from now; false when that is not known (nothing written so
 * far ahead, or no loudness at all), *speaker is 0 then. */
bool chr_director_speaker_ahead(uint32_t ahead_ms, uint16_t *speaker);
/* [ui] The next sentence of an answer the application told of ("companion.sentence"), oldest first: where it
 * begins in the answer's voice (ms after its first sound; -1 unsaid; CHR_SENTENCE_NOW, CHR_SENTENCE_OWN: a face the AI
 * asked for begins, it is over) and the mood it shows. False when none waits; none of a talk that is over does. */
bool chr_director_take_sentence(int32_t *begin_ms, size_t *mood);
/* [any] Whether painting may be shared with a second core (the bench turns it off to measure). */
bool chr_director_share(void);
/* [ui] The sprite pack this firmware carries (esp-idf/ or host/), as the actor draws from it; NULL
 * when there is none. Closed when the actor stops. */
struct chr_pack;
const struct chr_pack *chr_pack_carried(void);
void chr_pack_carried_close(void);
/* Adds what the pack's reader has to say to a diagnostic line. */
void chr_pack_carried_report(char *line, size_t capacity);
