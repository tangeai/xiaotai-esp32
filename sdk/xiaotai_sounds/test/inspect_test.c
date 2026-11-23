// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_audio.py
// Replaces legacy: tests/regression/test_wav_reader.py
/* Container checks without playing.
 *
 * WAV: the fixtures of the existing WAV suites, built here byte for byte as
 * Python's wave module writes them (RIFF, 16-byte "fmt ", "data"):
 *   - the parser matrix of the recognition download test (valid PCM at six
 *     rates and two channel counts, metadata chunks with odd lengths, an
 *     extended fmt chunk, and every malformed or unsupported variant), each
 *     at an odd address, checking rate, channels, frames, the exact position
 *     and content of the samples, and an all-zero result on refusal;
 *   - the header side of the WAV reader test (13 rates from 8000 to 48000
 *     including off-by-one rates, 1 and 2 channels, 1..997 frames);
 *   - the file playback test's inspect checks (duration rounded up, NULL
 *     result, a 10-byte file, the 10 MiB limit checked before any read).
 * RAW and PACKETS: whole frames per encoding, packet lengths 1..512 inside
 * the data, durations, and the description errors. No port allocation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/sounds.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* ------------------------------------------------------------ byte buffers */

typedef struct {
    uint8_t *data;
    size_t size;
} buf_t;

static void put(buf_t *b, const void *bytes, size_t n)
{
    uint8_t *grown = realloc(b->data, b->size + n + 1);
    if (!grown) abort();
    b->data = grown;
    if (n) memcpy(b->data + b->size, bytes, n);
    b->size += n;
}

static void put_u16(buf_t *b, unsigned v)
{
    uint8_t le[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, le, 2);
}

static void put_u32(buf_t *b, uint32_t v)
{
    uint8_t le[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, le, 4);
}

static buf_t slice(const buf_t *b, size_t from, size_t to)
{
    buf_t out = {0};
    put(&out, b->data + from, to - from);
    return out;
}

static buf_t cat(const buf_t *a, const buf_t *b)
{
    buf_t out = {0};
    put(&out, a->data, a->size);
    put(&out, b->data, b->size);
    return out;
}

static buf_t lit(const char *bytes, size_t n)
{
    buf_t out = {0};
    put(&out, bytes, n);
    return out;
}

static void drop(buf_t *b)
{
    free(b->data);
    *b = (buf_t){0};
}

/* "RIFF" + length + "WAVE" + chunks, as the Python helper riff() builds it. */
static buf_t riff(const buf_t *chunks)
{
    buf_t out = {0};
    put(&out, "RIFF", 4);
    put_u32(&out, (uint32_t)(4 + chunks->size));
    put(&out, "WAVE", 4);
    put(&out, chunks->data, chunks->size);
    return out;
}

/* What wave.writeframes produces for interleaved 16-bit samples. */
static buf_t wav_file(uint32_t rate, unsigned channels, const int16_t *samples, size_t count)
{
    buf_t out = {0};
    put(&out, "RIFF", 4);
    put_u32(&out, (uint32_t)(36 + count * 2));
    put(&out, "WAVEfmt ", 8);
    put_u32(&out, 16);
    put_u16(&out, 1);
    put_u16(&out, channels);
    put_u32(&out, rate);
    put_u32(&out, rate * channels * 2);
    put_u16(&out, channels * 2);
    put_u16(&out, 16);
    put(&out, "data", 4);
    put_u32(&out, (uint32_t)(count * 2));
    for (size_t i = 0; i < count; ++i) put_u16(&out, (uint16_t)samples[i]);
    return out;
}

/* pcm_wav() of the recognition test: 4096 frames of (i * 137) % 65536 - 32768. */
static int16_t *recognition_samples(unsigned channels, size_t *count)
{
    *count = 4096U * channels;
    int16_t *samples = malloc(*count * sizeof *samples);
    if (!samples) abort();
    for (size_t i = 0; i < *count; ++i) samples[i] = (int16_t)((long)((i * 137U) % 65536U) - 32768);
    return samples;
}

static buf_t pcm_wav(uint32_t rate, unsigned channels)
{
    size_t count;
    int16_t *samples = recognition_samples(channels, &count);
    buf_t out = wav_file(rate, channels, samples, count);
    free(samples);
    return out;
}

static xiaotai_sound_t wav_sound(const uint8_t *data, size_t size)
{
    return (xiaotai_sound_t){.name = "probe", .encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_WAV,
                             .data = data, .size = size};
}

static int info_zero(const xiaotai_sound_info_t *info)
{
    return !info->rate_hz && !info->channels && !info->frames && !info->duration_ms && !info->payload &&
           !info->payload_size;
}

static uint32_t ceil_ms(uint64_t frames, uint32_t rate)
{
    return (uint32_t)((frames * 1000U + rate - 1U) / rate);
}

/* Inspects a copy at an odd address. For a valid file: rate, channels, frame
 * count, duration and that the payload is exactly `pcm` at `offset` in the
 * file. For a refused one: the code and an all-zero result. */
static int check_wav(const char *what, const buf_t *file, xiaotai_err_t expected, uint32_t rate, unsigned channels,
                     const buf_t *pcm, size_t offset)
{
    uint8_t *copy = malloc(file->size + 1);
    if (!copy) abort();
    if (file->size) memcpy(copy + 1, file->data, file->size);
    const xiaotai_sound_t sound = wav_sound(copy + 1, file->size);
    xiaotai_sound_info_t info;
    memset(&info, 0xa5, sizeof info);
    xiaotai_err_t err = xiaotai_sounds_inspect(&sound, &info);
    int failed = 0;
    if (err != expected) {
        fprintf(stderr, "%s: inspect returned 0x%x, expected 0x%x\n", what, (unsigned)err, (unsigned)expected);
        failed = 1;
    } else if (err != XIAOTAI_OK) {
        failed = !info_zero(&info);
        if (failed) fprintf(stderr, "%s: refused but the result is not cleared\n", what);
    } else {
        size_t frames = pcm->size / (channels * 2U);
        failed = info.rate_hz != rate || info.channels != channels || info.frames != frames ||
                 info.duration_ms != ceil_ms(frames, rate) || info.payload != copy + 1 + offset ||
                 info.payload_size != pcm->size || memcmp(info.payload, pcm->data, pcm->size) != 0;
        if (failed) fprintf(stderr, "%s: wrong result\n", what);
    }
    /* Inspection reads only. */
    if (!failed && file->size && memcmp(copy + 1, file->data, file->size)) {
        fprintf(stderr, "%s: data modified\n", what);
        failed = 1;
    }
    free(copy);
    return failed;
}

/* --------------------------------------------- the recognition WAV matrix */

typedef struct {
    const char *name;
    buf_t file;
    xiaotai_err_t expected;
} wav_case_t;

static int test_wav_matrix(void)
{
    const uint32_t rates[] = {8000, 16000, 22050, 24000, 44100, 48000};
    unsigned valid = 0, invalid = 0;
    for (size_t r = 0; r < COUNT_OF(rates); ++r) {
        for (unsigned channels = 1; channels <= 2; ++channels) {
            buf_t file = pcm_wav(rates[r], channels);
            buf_t pcm = slice(&file, 44, file.size);
            char what[32];
            snprintf(what, sizeof what, "pcm-%u-%u", (unsigned)rates[r], channels);
            if (check_wav(what, &file, XIAOTAI_OK, rates[r], channels, &pcm, 44)) return 1;
            drop(&file);
            drop(&pcm);
            ++valid;
        }
    }

    buf_t basic = pcm_wav(24000, 1);
    CHECK(basic.size == 44 + 8192);
    buf_t after_riff = slice(&basic, 12, basic.size); /* basic[12:] */
    buf_t fmt = slice(&basic, 12, 36);                /* basic[12:36] */
    buf_t data = slice(&basic, 36, basic.size);       /* basic[36:] */
    buf_t fmt_body = slice(&basic, 20, 36);           /* basic[20:36] */
    buf_t pcm = slice(&basic, 44, basic.size);

    /* Unknown odd chunks before and after, and an 18-byte (extended) fmt. */
    {
        buf_t junk = lit("JUNK\x03\0\0\0abc\0", 12);
        buf_t list = lit("LIST\x02\0\0\0xy", 10);
        buf_t a = cat(&junk, &after_riff), chunks = cat(&a, &list), file = riff(&chunks);
        if (check_wav("metadata", &file, XIAOTAI_OK, 24000, 1, &pcm, 12 + 12 + 24 + 8)) return 1;
        drop(&junk), drop(&list), drop(&a), drop(&chunks), drop(&file);
        ++valid;
    }
    {
        buf_t head = lit("fmt \x12\0\0\0", 8), pad = lit("\0\0", 2);
        buf_t a = cat(&head, &fmt_body), b = cat(&a, &pad), chunks = cat(&b, &data), file = riff(&chunks);
        if (check_wav("extended-format", &file, XIAOTAI_OK, 24000, 1, &pcm, 12 + 26 + 8)) return 1;
        drop(&head), drop(&pad), drop(&a), drop(&b), drop(&chunks), drop(&file);
        ++valid;
    }

    wav_case_t cases[16];
    size_t n = 0;
    buf_t t1, t2;
    cases[n++] = (wav_case_t){"short", slice(&basic, 0, 20), XIAOTAI_ERR_INVALID_SIZE};
    cases[n++] = (wav_case_t){"riff-size", slice(&basic, 0, basic.size - 2), XIAOTAI_ERR_INVALID_SIZE};
    t1 = lit("x", 1);
    cases[n++] = (wav_case_t){"trailing", cat(&basic, &t1), XIAOTAI_ERR_INVALID_SIZE};
    drop(&t1);
    /* 36 bytes in all: shorter than the smallest file. */
    cases[n++] = (wav_case_t){"no-data", riff(&fmt), XIAOTAI_ERR_INVALID_SIZE};
    t1 = cat(&data, &fmt);
    cases[n++] = (wav_case_t){"data-first", riff(&t1), XIAOTAI_ERR_NOT_SUPPORTED};
    drop(&t1);
    t1 = cat(&fmt, &after_riff);
    cases[n++] = (wav_case_t){"double-format", riff(&t1), XIAOTAI_ERR_NOT_SUPPORTED};
    drop(&t1);
    t1 = cat(&after_riff, &data);
    cases[n++] = (wav_case_t){"double-data", riff(&t1), XIAOTAI_ERR_NOT_SUPPORTED};
    drop(&t1);
    t2 = lit("data\0\0\0\0", 8);
    t1 = cat(&fmt, &t2);
    cases[n++] = (wav_case_t){"zero-data", riff(&t1), XIAOTAI_ERR_NOT_SUPPORTED};
    drop(&t1), drop(&t2);
    t2 = lit("JUNK", 4);
    t1 = cat(&after_riff, &t2);
    cases[n++] = (wav_case_t){"incomplete-chunk", riff(&t1), XIAOTAI_ERR_INVALID_SIZE};
    drop(&t1), drop(&t2);
    t2 = lit("JUNK\x01\0\0\0x", 9);
    t1 = cat(&after_riff, &t2);
    cases[n++] = (wav_case_t){"missing-padding", riff(&t1), XIAOTAI_ERR_INVALID_SIZE};
    drop(&t1), drop(&t2);
    t2 = lit("data\xff\xff\xff\xff", 8);
    t1 = cat(&fmt, &t2);
    cases[n++] = (wav_case_t){"huge-chunk", riff(&t1), XIAOTAI_ERR_INVALID_SIZE};
    drop(&t1), drop(&t2);
    t2 = lit("data\x01\0\0\0x\0", 10);
    t1 = cat(&fmt, &t2);
    cases[n++] = (wav_case_t){"data-misaligned", riff(&t1), XIAOTAI_ERR_INVALID_SIZE};
    drop(&t1), drop(&t2);
    CHECK(n <= COUNT_OF(cases));
    for (size_t i = 0; i < n; ++i) {
        if (check_wav(cases[i].name, &cases[i].file, cases[i].expected, 0, 0, NULL, 0)) return 1;
        drop(&cases[i].file);
        ++invalid;
    }
    /* "empty": nothing to inspect. */
    {
        buf_t empty = {0};
        if (check_wav("empty", &empty, XIAOTAI_ERR_INVALID_ARG, 0, 0, NULL, 0)) return 1;
        ++invalid;
    }

    /* Single-field changes of the basic file. */
    static const struct {
        const char *name;
        size_t offset;
        uint32_t value;
        unsigned width;
        xiaotai_err_t expected;
    } patches[] = {
        {"riff-magic", 0, 0x58464952u /* "RIFX" */, 4, XIAOTAI_ERR_NOT_SUPPORTED},
        {"wave-magic", 8, 0x2033504du /* "MP3 " */, 4, XIAOTAI_ERR_NOT_SUPPORTED},
        {"alaw", 20, 6, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"float", 20, 3, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"eight-bit", 34, 8, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"zero-channels", 22, 0, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"six-channels", 22, 6, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"low-rate", 24, 7999, 4, XIAOTAI_ERR_NOT_SUPPORTED},
        {"high-rate", 24, 48001, 4, XIAOTAI_ERR_NOT_SUPPORTED},
        {"byte-rate", 28, 42, 4, XIAOTAI_ERR_NOT_SUPPORTED},
        {"block-align", 32, 4, 2, XIAOTAI_ERR_NOT_SUPPORTED},
        {"streaming-size", 4, 0xffffffffu, 4, XIAOTAI_ERR_INVALID_SIZE},
    };
    for (size_t i = 0; i < COUNT_OF(patches); ++i) {
        buf_t changed = slice(&basic, 0, basic.size);
        for (unsigned b = 0; b < patches[i].width; ++b)
            changed.data[patches[i].offset + b] = (uint8_t)(patches[i].value >> (8 * b));
        if (check_wav(patches[i].name, &changed, patches[i].expected, 0, 0, NULL, 0)) return 1;
        drop(&changed);
        ++invalid;
    }
    drop(&basic), drop(&after_riff), drop(&fmt), drop(&data), drop(&fmt_body), drop(&pcm);
    /* As in the original suite: 12 PCM files and 2 legal layouts accepted;
     * 13 malformed files ("empty" among them) and 12 patched fields refused. */
    CHECK(valid == 14 && invalid == 25);
    return 0;
}

/* ------------------------------------- the WAV reader test's header sweep */

static int test_wav_reader_sweep(void)
{
    const uint32_t rates[] = {8000, 8001, 11025, 12000, 15999, 16000, 16001, 22050, 24000, 32000, 44100,
                              47999, 48000};
    const size_t frame_counts[] = {1, 3, 257, 997};
    unsigned checks = 0;
    for (size_t r = 0; r < COUNT_OF(rates); ++r) {
        for (unsigned channels = 1; channels <= 2; ++channels) {
            for (size_t f = 0; f < COUNT_OF(frame_counts); ++f) {
                size_t frames = frame_counts[f], count = frames * channels;
                int16_t *samples = malloc(count * sizeof *samples);
                if (!samples) abort();
                for (size_t i = 0; i < frames; ++i) {
                    int16_t mono = (int16_t)((long)((i * 7919U) % 65536U) - 32768);
                    if (channels == 1) {
                        samples[i] = mono;
                    } else {
                        samples[2 * i] = mono;
                        samples[2 * i + 1] = 32767;
                    }
                }
                buf_t file = wav_file(rates[r], channels, samples, count);
                buf_t pcm = slice(&file, 44, file.size);
                char what[48];
                snprintf(what, sizeof what, "reader-%u-%u-%zu", (unsigned)rates[r], channels, frames);
                if (check_wav(what, &file, XIAOTAI_OK, rates[r], channels, &pcm, 44)) return 1;
                drop(&file), drop(&pcm);
                free(samples);
                ++checks;
            }
        }
    }
    /* The reader test's broken file: 13 bytes of text. */
    buf_t broken = lit("broken header", 13);
    if (check_wav("broken-header", &broken, XIAOTAI_ERR_INVALID_SIZE, 0, 0, NULL, 0)) return 1;
    drop(&broken);
    CHECK(checks == 13 * 2 * 4);
    return 0;
}

/* --------------------------------------- the file playback inspect checks */

static int test_file_playback_inspect(void)
{
    /* 1001 frames at 44100 Hz: 22.698... ms, rounded up to 23. */
    for (unsigned channels = 1; channels <= 2; ++channels) {
        const uint32_t rates[] = {8000, 16000, 44100, 48000};
        for (size_t r = 0; r < COUNT_OF(rates); ++r) {
            size_t count = 1001U * channels;
            int16_t *values = malloc(count * sizeof *values);
            if (!values) abort();
            for (size_t i = 0; i < 1001U; ++i)
                for (unsigned c = 0; c < channels; ++c)
                    values[i * channels + c] = (int16_t)((long)((i * 139U) % 16000U) - 8000);
            buf_t file = wav_file(rates[r], channels, values, count);
            free(values);
            const xiaotai_sound_t sound = wav_sound(file.data, file.size);
            xiaotai_sound_info_t info;
            CHECK(xiaotai_sounds_inspect(&sound, &info) == XIAOTAI_OK);
            CHECK(info.frames == 1001U);
            CHECK(info.duration_ms == ceil_ms(1001U, rates[r]));
            drop(&file);
        }
    }
    {
        int16_t one[1001] = {0};
        buf_t file = wav_file(44100, 1, one, 1001);
        const xiaotai_sound_t sound = wav_sound(file.data, file.size);
        xiaotai_sound_info_t info;
        CHECK(xiaotai_sounds_inspect(&sound, &info) == XIAOTAI_OK);
        CHECK(info.duration_ms == 23U);
        CHECK(xiaotai_sounds_inspect(&sound, NULL) == XIAOTAI_ERR_INVALID_ARG);
        CHECK(xiaotai_sounds_inspect(NULL, &info) == XIAOTAI_ERR_INVALID_ARG && info_zero(&info));
        /* A 10-byte file is refused and leaves nothing behind. */
        xiaotai_sound_t ten = sound;
        ten.size = 10;
        memset(&info, 0x5a, sizeof info);
        CHECK(xiaotai_sounds_inspect(&ten, &info) != XIAOTAI_OK && info_zero(&info));
        /* Above 10 MiB: refused before a byte is read (the buffer is small). */
        xiaotai_sound_t huge = sound;
        huge.size = XIAOTAI_SOUND_BYTES_MAX + 1U;
        memset(&info, 0x5a, sizeof info);
        CHECK(xiaotai_sounds_inspect(&huge, &info) == XIAOTAI_ERR_INVALID_SIZE && info_zero(&info));
        xiaotai_sound_t no_data = sound;
        no_data.data = NULL;
        CHECK(xiaotai_sounds_inspect(&no_data, &info) == XIAOTAI_ERR_INVALID_ARG);
        drop(&file);
    }
    /* Exactly 10 MiB is accepted: 5 Mi frames of 8 kHz PCM16 mono. */
    {
        uint8_t *big = calloc(XIAOTAI_SOUND_BYTES_MAX, 1);
        if (!big) abort();
        const xiaotai_sound_t raw = {.name = "probe", .encoding = XIAOTAI_CODEC_PCM16,
                                     .container = XIAOTAI_SOUND_RAW, .rate_hz = 8000, .channels = 1,
                                     .data = big, .size = XIAOTAI_SOUND_BYTES_MAX};
        xiaotai_sound_info_t info;
        CHECK(xiaotai_sounds_inspect(&raw, &info) == XIAOTAI_OK);
        CHECK(info.frames == 5U * 1024U * 1024U && info.duration_ms == 655360U);
        free(big);
    }
    return 0;
}

/* ------------------------------------------------------------------ RAW */

static int test_raw(void)
{
    static uint8_t bytes[334720];
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = (uint8_t)(i * 31U);
    xiaotai_sound_info_t info;

    /* 20 ms of 8 kHz PCM16 mono. */
    xiaotai_sound_t pcm = {.name = "probe", .encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_RAW,
                           .rate_hz = 8000, .channels = 1, .data = bytes, .size = 320};
    CHECK(xiaotai_sounds_inspect(&pcm, &info) == XIAOTAI_OK);
    CHECK(info.rate_hz == 8000 && info.channels == 1 && info.frames == 160 && info.duration_ms == 20);
    CHECK(info.payload == bytes && info.payload_size == 320);
    /* PCM16 is two bytes per sample and per channel. */
    pcm.size = 321;
    CHECK(xiaotai_sounds_inspect(&pcm, &info) == XIAOTAI_ERR_INVALID_SIZE && info_zero(&info));
    pcm.channels = 2;
    pcm.rate_hz = 16000;
    pcm.size = 322;
    CHECK(xiaotai_sounds_inspect(&pcm, &info) == XIAOTAI_ERR_INVALID_SIZE);
    pcm.size = 324;
    CHECK(xiaotai_sounds_inspect(&pcm, &info) == XIAOTAI_OK);
    CHECK(info.frames == 81 && info.channels == 2 && info.duration_ms == 6); /* 5.0625 ms */

    /* A-law and mu-law: one byte per sample, any length. The whole guidance
     * file size (334720 bytes) is 41840 ms at 8 kHz. */
    xiaotai_sound_t alaw = {.name = "probe", .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
                            .rate_hz = 8000, .channels = 1, .data = bytes, .size = sizeof bytes};
    CHECK(xiaotai_sounds_inspect(&alaw, &info) == XIAOTAI_OK);
    CHECK(info.frames == 334720U && info.duration_ms == 41840U && info.payload_size == sizeof bytes);
    alaw.size = 161;
    CHECK(xiaotai_sounds_inspect(&alaw, &info) == XIAOTAI_OK);
    CHECK(info.frames == 161 && info.duration_ms == 21); /* 20.125 ms rounded up */
    alaw.encoding = XIAOTAI_CODEC_G711U;
    alaw.size = 1;
    CHECK(xiaotai_sounds_inspect(&alaw, &info) == XIAOTAI_OK);
    CHECK(info.frames == 1 && info.duration_ms == 1);

    /* Description errors. */
    xiaotai_sound_t bad = alaw;
    bad.rate_hz = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG && info_zero(&info));
    bad = alaw;
    bad.channels = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    /* A framed encoding has no fixed sample size: not a RAW encoding. */
    bad = alaw;
    bad.encoding = XIAOTAI_CODEC_OPUS;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_NOT_SUPPORTED);
    /* Not an audio codec. */
    const int not_audio[] = {XIAOTAI_CODEC_NONE, XIAOTAI_CODEC_JPEG, XIAOTAI_CODEC_H264, 99};
    for (size_t i = 0; i < COUNT_OF(not_audio); ++i) {
        bad = alaw;
        bad.encoding = (xiaotai_codec_t)not_audio[i];
        CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    }
    /* A container this build does not know. */
    bad = alaw;
    bad.container = (xiaotai_sound_container_t)7;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_NOT_SUPPORTED);
    bad = alaw;
    bad.size = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    /* A WAV entry is PCM16 by definition. */
    bad = alaw;
    bad.container = XIAOTAI_SOUND_WAV;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    return 0;
}

/* -------------------------------------------------------------- PACKETS */

static void put_packet(buf_t *b, size_t length, uint8_t fill)
{
    put_u16(b, (unsigned)length);
    for (size_t i = 0; i < length; ++i) put(b, &fill, 1);
}

static int test_packets(void)
{
    xiaotai_sound_info_t info;
    buf_t stream = {0};
    put_packet(&stream, 1, 0x11);
    put_packet(&stream, 512, 0x22);
    put_packet(&stream, 80, 0x33);
    const xiaotai_sound_t opus = {.name = "probe", .encoding = XIAOTAI_CODEC_OPUS,
                                  .container = XIAOTAI_SOUND_PACKETS, .rate_hz = 16000, .channels = 1,
                                  .packet_ms = 20, .data = stream.data, .size = stream.size};
    CHECK(xiaotai_sounds_inspect(&opus, &info) == XIAOTAI_OK);
    CHECK(info.rate_hz == 16000 && info.channels == 1 && info.frames == 3 && info.duration_ms == 60);
    CHECK(info.payload == stream.data && info.payload_size == 2 + 1 + 2 + 512 + 2 + 80);

    /* 501 packets: 10020 ms, the walk visits every one. */
    buf_t many = {0};
    for (unsigned i = 0; i < 501; ++i) put_packet(&many, 1 + (i * 37U) % 512U, (uint8_t)i);
    xiaotai_sound_t long_one = opus;
    long_one.data = many.data;
    long_one.size = many.size;
    CHECK(xiaotai_sounds_inspect(&long_one, &info) == XIAOTAI_OK);
    CHECK(info.frames == 501 && info.duration_ms == 10020);

    /* Every malformed stream is refused as a size error. */
    struct {
        const char *name;
        buf_t data;
    } broken[6];
    size_t n = 0;
    broken[n].name = "zero-length";
    broken[n].data = (buf_t){0};
    put_packet(&broken[n].data, 10, 1);
    put_packet(&broken[n++].data, 0, 0);
    broken[n].name = "too-long";
    broken[n].data = (buf_t){0};
    put_packet(&broken[n++].data, 513, 2);
    broken[n].name = "beyond-end";
    broken[n].data = (buf_t){0};
    put_packet(&broken[n].data, 10, 3);
    put_u16(&broken[n].data, 20);
    put(&broken[n++].data, "short", 5);
    broken[n].name = "one-byte-tail";
    broken[n].data = (buf_t){0};
    put_packet(&broken[n].data, 10, 4);
    put(&broken[n++].data, "\x05", 1);
    broken[n].name = "lone-prefix";
    broken[n].data = (buf_t){0};
    put_u16(&broken[n++].data, 7);
    for (size_t i = 0; i < n; ++i) {
        xiaotai_sound_t sound = opus;
        sound.data = broken[i].data.data;
        sound.size = broken[i].data.size;
        memset(&info, 0x5a, sizeof info);
        xiaotai_err_t err = xiaotai_sounds_inspect(&sound, &info);
        if (err != XIAOTAI_ERR_INVALID_SIZE || !info_zero(&info)) {
            fprintf(stderr, "packets %s: 0x%x\n", broken[i].name, (unsigned)err);
            return 1;
        }
        drop(&broken[i].data);
    }

    /* Description errors: no packet duration, rate or channels. */
    xiaotai_sound_t bad = opus;
    bad.packet_ms = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG && info_zero(&info));
    bad = opus;
    bad.rate_hz = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    bad = opus;
    bad.channels = 0;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_ARG);
    bad = opus;
    bad.size = XIAOTAI_SOUND_BYTES_MAX + 1U;
    CHECK(xiaotai_sounds_inspect(&bad, &info) == XIAOTAI_ERR_INVALID_SIZE);
    drop(&stream);
    drop(&many);
    return 0;
}

int main(void)
{
    xiaotai_mem_host_clear();
    if (test_wav_matrix() || test_wav_reader_sweep() || test_file_playback_inspect() || test_raw() ||
        test_packets())
        return 1;
    /* Inspection reads in place: no port allocation. */
    CHECK(xiaotai_mem_host_allocations() == 0);
    printf("sounds_inspect: ok\n");
    return 0;
}
