// SPDX-License-Identifier: MIT
/* aec.single replaces a canceller that removes no echo, and leaves one alone
   once it has shown that it works. The stand-in canceller here either passes
   the microphone through (deaf, like the one the ESP32-S31 created at start)
   or removes 40 dB; both lag their input by one block, as ESP-SR does. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/media.h"
#include "xiaotai_mem_host.h"

extern const xiaotai_audio_processor_t media_aec_single;

enum { CHUNK = 512, CHANNELS = 4 };

struct aec_handle {
    bool deaf;
    int16_t last[CHUNK];
};

static int deaf_creations;   /* how many of the first creations come up deaf */
static int fail_creation;    /* 1-based creation that returns NULL; 0: none */
static int created, destroyed;

aec_handle_t *aec_create_from_config(aec_config_t *config)
{
    assert(config->mic_num == 1 && config->ref_num == 1 && config->sample_rate == 16000);
    assert(config->mode == AEC_MODE_FD_LOW_COST && config->filter_length == 4 && config->nlp_level == AEC_NLP_LEVEL_AGGR);
    ++created;
    if (created == fail_creation) return NULL;
    aec_handle_t *handle = calloc(1, sizeof(*handle));
    assert(handle);
    handle->deaf = created <= deaf_creations;
    return handle;
}

int aec_get_chunksize(const aec_handle_t *handle)
{
    (void)handle;
    return CHUNK;
}

void aec_process(const aec_handle_t *handle, int16_t *indata, int16_t *refdata, int16_t *outdata)
{
    (void)refdata;
    aec_handle_t *self = (aec_handle_t *)handle;
    for (int i = 0; i < CHUNK; ++i) outdata[i] = self->deaf ? self->last[i] : (int16_t)(self->last[i] / 100);
    memcpy(self->last, indata, sizeof(self->last));
}

void aec_destroy(aec_handle_t *handle)
{
    ++destroyed;
    free(handle);
}

static const xiaotai_audio_input_t input = {.desc = {.rate_hz = 16000, .channels = CHANNELS,
    .encoding = XIAOTAI_CODEC_PCM16,
    .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_UNUSED, XIAOTAI_CHANNEL_UNUSED}}};

/* One block: the speaker plays `far` (peak), the microphone hears `near`
   (its own sound) plus a quarter of the speaker. */
static void block(void *state, int far, int near)
{
    static int16_t pcm[CHUNK * CHANNELS];
    for (int i = 0; i < CHUNK; ++i) {
        const int sign = i & 1 ? 1 : -1;
        pcm[i * CHANNELS + 0] = (int16_t)(sign * (far / 4 + near));
        pcm[i * CHANNELS + 1] = (int16_t)(sign * far);
        pcm[i * CHANNELS + 2] = pcm[i * CHANNELS + 3] = 0;
    }
    assert(media_aec_single.feed(state, pcm, sizeof(pcm), 0, 0) == XIAOTAI_OK);
    xiaotai_processor_frame_t frame;
    assert(media_aec_single.fetch(state, &frame, 0) == XIAOTAI_OK && frame.samples == CHUNK);
}

static void *start(int deaf, int fail)
{
    xiaotai_media_reset();
    deaf_creations = deaf;
    fail_creation = fail;
    created = destroyed = 0;
    xiaotai_processor_info_t info = {0};
    void *state = NULL;
    assert(media_aec_single.match(&input.desc, NULL) == 2);
    assert(media_aec_single.open(&input, NULL, &info, &state) == XIAOTAI_OK && state);
    assert(info.feed_frames == CHUNK && (info.flags & XIAOTAI_PROCESSOR_CANCELS_ECHO));
    return state;
}

static uint32_t restarts(void *state)
{
    xiaotai_processor_stats_t stats;
    media_aec_single.stats(state, &stats);
    return stats.restarts;
}

static void finish(void *state)
{
    media_aec_single.close(state);
    assert(created == destroyed + (fail_creation ? 1 : 0));
    assert(xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    /* Deaf at start, a working one afterwards: one restart after 32 blocks of
       far-end sound, then the new canceller proves itself and is kept. */
    void *s = start(1, 0);
    for (int i = 0; i < 32; ++i) block(s, 4000, 0); /* the first block has no previous one to compare with */
    assert(restarts(s) == 0 && created == 1);
    block(s, 4000, 0);
    assert(restarts(s) == 1 && created == 2 && destroyed == 1);
    for (int i = 0; i < 400; ++i) block(s, 4000, 0);
    assert(restarts(s) == 1 && created == 2);
    /* Proven: long double talk (the near end louder than the echo) later on is not mistaken for deafness. */
    for (int i = 0; i < 400; ++i) block(s, 4000, 8000);
    assert(restarts(s) == 1 && created == 2);
    finish(s);

    /* A working canceller from the start is never replaced. */
    s = start(0, 0);
    for (int i = 0; i < 600; ++i) block(s, 4000, i % 3 ? 0 : 8000);
    assert(restarts(s) == 0 && created == 1);
    finish(s);

    /* Silence, the near end alone, or the speaker playing into a muted
       microphone path are not evidence either way. */
    s = start(1, 0);
    for (int i = 0; i < 300; ++i) block(s, 0, 0);
    for (int i = 0; i < 300; ++i) block(s, 0, 3000);
    for (int i = 0; i < 300; ++i) block(s, 200, 0); /* echo at RMS 50, under the threshold */
    assert(restarts(s) == 0 && created == 1);
    finish(s);

    /* Always deaf: at most three restarts, then it stops trying. */
    s = start(1000, 0);
    for (int i = 0; i < 32 * 6; ++i) block(s, 4000, 0);
    assert(restarts(s) == 3 && created == 4 && destroyed == 3);
    finish(s);

    /* A restart that cannot create a new canceller keeps the old one and stops checking. */
    s = start(1, 2);
    for (int i = 0; i < 32 * 3; ++i) block(s, 4000, 0);
    assert(restarts(s) == 0 && created == 2 && destroyed == 0);
    finish(s);

    puts("aec single: a deaf canceller is replaced after 1 s of far-end sound, at most 3 times; a working one is kept");
    return 0;
}
