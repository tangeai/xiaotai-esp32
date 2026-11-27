// SPDX-License-Identifier: MIT
/* The Opus decoder's concealment with a real libopus: a lost packet is
 * replaced by sound, better with the packet that follows it, and decoding goes
 * on as if nothing was missing. */
#include "xiaotai/media_codec.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern const xiaotai_audio_decoder_t media_opus_decoder;
typedef struct OpusEncoder OpusEncoder;
extern OpusEncoder *opus_encoder_create(int32_t rate, int channels, int application, int *error);
extern int32_t opus_encode(OpusEncoder *encoder, const int16_t *pcm, int frame, unsigned char *data, int32_t max);
extern int opus_encoder_ctl(OpusEncoder *encoder, int request, ...);
extern void opus_encoder_destroy(OpusEncoder *encoder);

enum { RATE = 16000, FRAME = 320, FRAMES = 200, LOST_EVERY = 5, VOIP = 2048,
       SET_BITRATE = 4002, SET_FEC = 4012, SET_LOSS = 4014 };
static int16_t source[FRAMES][FRAME];
static uint8_t packets[FRAMES][400];
static int32_t sizes[FRAMES];

static void encode_all(void)
{
    int error = -1;
    OpusEncoder *encoder = opus_encoder_create(RATE, 1, VOIP, &error);
    assert(encoder && !error);
    assert(!opus_encoder_ctl(encoder, SET_BITRATE, 24000) && !opus_encoder_ctl(encoder, SET_FEC, 1) &&
           !opus_encoder_ctl(encoder, SET_LOSS, 20));
    for (int i = 0; i < FRAMES; ++i) {
        for (int n = 0; n < FRAME; ++n) {
            const double t = (double)(i * FRAME + n) / RATE;
            const double voice = sin(2 * M_PI * 180 * t) + 0.6 * sin(2 * M_PI * 720 * t) + 0.3 * sin(2 * M_PI * 1500 * t);
            source[i][n] = (int16_t)(5000 * voice * (0.6 + 0.4 * sin(2 * M_PI * 3 * t)));
        }
        sizes[i] = opus_encode(encoder, source[i], FRAME, packets[i], sizeof(packets[i]));
        assert(sizes[i] > 0);
    }
    opus_encoder_destroy(encoder);
}

/* Energy of what was played where packets were lost, against the same place
 * decoded without loss: 0 would be silence. */
static double run(bool conceal, bool with_next)
{
    const xiaotai_media_format_t format = {XIAOTAI_CODEC_OPUS, RATE, 1, 20};
    void *clean = NULL, *lossy = NULL;
    assert(media_opus_decoder.open(&format, &clean) == XIAOTAI_OK &&
           media_opus_decoder.open(&format, &lossy) == XIAOTAI_OK);
    double error = 0, energy = 0;
    int16_t want[FRAME], got[FRAME];
    for (int i = 0; i < FRAMES; ++i) {
        size_t n = 0;
        assert(media_opus_decoder.decode(clean, packets[i], (size_t)sizes[i], want, FRAME, &n) == XIAOTAI_OK && n == FRAME);
        memset(got, 0, sizeof(got));
        if (i % LOST_EVERY == LOST_EVERY - 1 && i + 1 < FRAMES) {
            if (conceal) {
                assert(media_opus_decoder.conceal(lossy, with_next ? packets[i + 1] : NULL,
                       with_next ? (size_t)sizes[i + 1] : 0, got, FRAME) == XIAOTAI_OK);
            }
            for (int k = 0; k < FRAME; ++k) {
                error += ((double)want[k] - got[k]) * ((double)want[k] - got[k]);
                energy += (double)want[k] * want[k];
            }
        } else {
            assert(media_opus_decoder.decode(lossy, packets[i], (size_t)sizes[i], got, FRAME, &n) == XIAOTAI_OK && n == FRAME);
        }
    }
    media_opus_decoder.close(clean);
    media_opus_decoder.close(lossy);
    return error / energy;
}

int main(void)
{
    encode_all();
    const double silence = run(false, false), plc = run(true, false), fec = run(true, true);
    printf("relative error where packets were lost: silence %.3f, PLC %.3f, FEC %.3f\n", silence, plc, fec);
    assert(silence > 0.99 && plc < silence && fec < plc);

    const xiaotai_media_format_t format = {XIAOTAI_CODEC_OPUS, RATE, 1, 20};
    void *state = NULL;
    int16_t pcm[1920];
    assert(media_opus_decoder.open(&format, &state) == XIAOTAI_OK);
    /* Only whole steps of 2.5 ms, at most one longest packet. */
    assert(media_opus_decoder.conceal(state, NULL, 0, pcm, 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(media_opus_decoder.conceal(state, NULL, 0, pcm, 0) == XIAOTAI_ERR_INVALID_SIZE);
    assert(media_opus_decoder.conceal(state, NULL, 0, pcm, 1960) == XIAOTAI_ERR_INVALID_SIZE);
    /* A packet that is not Opus is not handed on as the one that follows. */
    const uint8_t bad[] = {3};
    assert(media_opus_decoder.conceal(state, bad, sizeof(bad), pcm, FRAME) == XIAOTAI_OK);
    assert(media_opus_decoder.reset(state) == XIAOTAI_OK);
    media_opus_decoder.close(state);
    return 0;
}
