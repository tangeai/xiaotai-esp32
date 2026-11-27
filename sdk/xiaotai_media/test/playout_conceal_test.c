// SPDX-License-Identifier: MIT
/* What playout does about packets that are lost or late, with a decoder that
 * plays the packet's number and conceals with negative ones. */
#include "downlink_ring.h"
#include "loudness.h"
#include "pacer.h"
#include "playout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { GENERATION = 7, EPOCH = 1, BLOCK = 320, RATE = 16000, MADE_UP = -500, WITH_NEXT = -600 };

static int64_t clock_us;
static bool can_conceal = true;
static int16_t played[64];
static unsigned blocks, conceals, with_next, resets, loudest, heard_made_up, first_voice;
static bool first_known;
static size_t packet = BLOCK;
static int32_t gap_limit = 120, dry_limit = 40;

int64_t __wrap_xiaotai_time_us(void) { return clock_us; }

int32_t __wrap_xiaotai_media_param(const char *name, int32_t fallback)
{
    if (!strcmp(name, "playout.conceal_gap_ms")) return gap_limit;
    if (!strcmp(name, "playout.conceal_dry_ms")) return dry_limit;
    return !strcmp(name, "playout.pacer_policy") ? MEDIA_PACER_ADAPTIVE : fallback;
}

static uint32_t packet_samples(const uint8_t *data, size_t size, uint32_t rate)
{
    (void)data;
    return size == 2U && rate == RATE ? (uint32_t)packet : 0;
}

static xiaotai_err_t decode(void *state, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity, size_t *samples)
{
    (void)state;
    int16_t value;
    assert(size == sizeof(value) && capacity >= packet);
    memcpy(&value, data, sizeof(value));
    for (size_t i = 0; i < packet; ++i) pcm[i] = value;
    *samples = packet;
    return XIAOTAI_OK;
}

static xiaotai_err_t conceal(void *state, const uint8_t *next, size_t size, int16_t *pcm, size_t samples)
{
    (void)state;
    assert(!next == !size && samples && samples <= packet);
    ++conceals;
    with_next += next != NULL;
    for (size_t i = 0; i < samples; ++i) pcm[i] = next ? WITH_NEXT : MADE_UP;
    return XIAOTAI_OK;
}

static xiaotai_err_t reset(void *state)
{
    (void)state;
    ++resets;
    return XIAOTAI_OK;
}

bool __wrap_media_rx_format_at(uint8_t slot, media_rx_format_t *format)
{
    static const xiaotai_audio_decoder_t able = {.packet_samples = packet_samples, .decode = decode, .reset = reset,
                                                 .conceal = conceal};
    static const xiaotai_audio_decoder_t unable = {.packet_samples = packet_samples, .decode = decode, .reset = reset};
    if (slot != 1U) return false;
    *format = (media_rx_format_t){.format = {XIAOTAI_CODEC_OPUS, RATE, 1, 20}, .decoder = can_conceal ? &able : &unable};
    return true;
}

static xiaotai_err_t speaker_write(void *state, void *data, size_t length)
{
    (void)state;
    const int16_t *pcm = data;
    assert(length / sizeof(*pcm) <= BLOCK && blocks < sizeof(played) / sizeof(*played));
    played[blocks++] = pcm[length / sizeof(*pcm) - 1U];
    return XIAOTAI_OK;
}

static uint32_t output_epoch = EPOCH;
static bool current(uint32_t generation, uint32_t epoch) { return generation == GENERATION && epoch == output_epoch; }

static xiaotai_downlink_t *ring;
static media_playout_t *player;

static void begin(void)
{
    static const xiaotai_audio_output_t speaker = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MONO}},
        .info = {.max_write_frames = BLOCK}, .write = speaker_write,
    };
    clock_us = 1000000;
    blocks = conceals = with_next = resets = loudest = heard_made_up = 0;
    assert(xiaotai_downlink_create(XIAOTAI_DOWNLINK_BYTES(64, 2), 2, &ring) == XIAOTAI_OK);
    media_downlink_tagged(ring, false, 64, 0);
    xiaotai_downlink_reset(ring, GENERATION);
    assert(media_playout_create(&speaker, ring, 1280, &player) == XIAOTAI_OK);
    media_playout_reset(player, GENERATION, EPOCH, (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_VOIP);
}

static void push(int16_t number, uint32_t stamp)
{
    assert(media_downlink_push_tagged(ring, media_downlink_epoch(ring), GENERATION, stamp,
                                      (uint32_t)(packet * 1000000U / RATE), &number, sizeof(number), clock_us, 1, 1,
                                      (uint32_t)packet, NULL));
}

static void run(unsigned passes, bool draining)
{
    static const xiaotai_media_session_t session = {.downlink = {XIAOTAI_CODEC_OPUS, RATE, 1, 20}};
    const media_playout_request_t request = {.session = &session, .generation = GENERATION, .epoch = output_epoch,
        .current = current, .armed_audio = true, .draining = draining, .capture_clock = true};
    for (unsigned i = 0; i < passes; ++i) {
        bool done;
        const unsigned before = blocks;
        (void)media_playout_pass(player, &request, &done);
        /* What a mouth would follow, and what was written while it did. */
        xiaotai_media_loudness_t voice;
        media_loudness_read(&voice, clock_us);
        if (voice.playout > loudest) loudest = voice.playout;
        if (blocks > before && !first_known) {
            first_known = true;
            first_voice = voice.playout;
        }
        heard_made_up += blocks > before && played[blocks - 1U] <= MADE_UP && voice.playout >= (unsigned)-MADE_UP;
        clock_us += 20000;
    }
}

static unsigned count(int16_t value)
{
    unsigned n = 0;
    for (unsigned i = 0; i < blocks; ++i) n += played[i] == value;
    return n;
}

static xiaotai_media_stats_t end(void)
{
    xiaotai_media_stats_t stats = {0};
    media_playout_stats(player, &stats);
    media_playout_destroy(player);
    xiaotai_downlink_destroy(ring);
    return stats;
}

int main(void)
{
    /* Packets 3 and 4 of eight never came: two pieces are made up, the last
     * with the help of packet 5, and everything that came is played. */
    begin();
    for (int16_t i = 1; i <= 8; ++i) if (i != 3 && i != 4) push(i, (uint32_t)i * 20U);
    run(40, true);
    xiaotai_media_stats_t stats = end();
    assert(conceals == 2 && with_next == 1 && stats.rx_concealed_ms == 40 && stats.rx_played == 6);
    assert(!stats.rx_decode_failed && resets == 1);
    /* What fills a hole in the middle of what is said is voice. */
    assert(loudest == (unsigned)-WITH_NEXT && heard_made_up == 2);

    /* A hole longer than the limit is silence the sender never sent. */
    begin();
    push(1, 20);
    push(2, 40);
    push(3, 1000);
    push(4, 1020);
    run(40, true);
    stats = end();
    assert(!conceals && !stats.rx_concealed_ms && stats.rx_played == 4);

    /* Repeated and backward stamps are not holes. */
    begin();
    push(1, 20);
    push(2, 20);
    push(3, 10);
    run(40, true);
    stats = end();
    assert(!conceals && stats.rx_played == 3);

    /* Nothing comes any more: a bounded stretch is made up, then it stays
     * silent, and the packet that comes late is played. */
    begin();
    for (int16_t i = 1; i <= 6; ++i) push(i, (uint32_t)i * 20U);
    run(30, false);
    assert(conceals == 2 && !with_next && count(MADE_UP) >= 1U);
    /* The made-up tail is heard, but nobody speaks in it: a mouth that follows the voice is shut by then. */
    assert(loudest && loudest <= 9U && !heard_made_up);
    const unsigned silent = conceals;
    run(10, false);
    assert(conceals == silent);
    /* What comes then is voice again, from its first block on. */
    loudest = 0;
    first_known = false;
    push(7, 140);
    push(8, 160);
    push(9, 180);
    run(20, true);
    assert(loudest == 9U && first_known && first_voice == 7U);
    stats = end();
    assert(stats.rx_concealed_ms == 40 && stats.rx_played == 9);

    /* Packets of a block and a half: in a block that holds the end of the made-up tail and the voice that
     * comes after it, the voice is what is loud. */
    packet = BLOCK * 3 / 2;
    begin();
    for (int16_t i = 1; i <= 6; ++i) push(i, (uint32_t)i * 30U);
    while (!conceals) run(1, false);
    first_known = false;
    for (int16_t i = 7; i <= 9; ++i) push(90, (uint32_t)i * 30U);
    while (!first_known) run(1, false);
    /* (where the two meet, one sample is made of both) */
    assert(first_voice >= 90U && first_voice < (unsigned)-MADE_UP && played[blocks - 1U] == 90);
    (void)end();

    /* What begins anew while a made-up tail is pending (the far end was interrupted) does not count it. */
    packet = BLOCK * 3;
    dry_limit = 60;
    begin();
    for (int16_t i = 1; i <= 6; ++i) push(i, (uint32_t)i * 60U);
    while (!conceals) run(1, false);
    xiaotai_downlink_reset(ring, GENERATION);
    media_playout_reset(player, GENERATION, ++output_epoch, (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_VOIP);
    first_known = false;
    for (int16_t i = 1; i <= 6; ++i) push((int16_t)(80 + i), (uint32_t)i * 60U);
    run(20, false);
    assert(first_known && first_voice == 81U);
    (void)end();
    output_epoch = EPOCH;
    packet = BLOCK;

    /* Switched off, or a decoder that cannot: as before. */
    gap_limit = dry_limit = 0;
    begin();
    for (int16_t i = 1; i <= 8; ++i) if (i != 4) push(i, (uint32_t)i * 20U);
    run(40, true);
    stats = end();
    assert(!conceals && stats.rx_played == 7);
    gap_limit = 120;
    dry_limit = 40;
    can_conceal = false;
    begin();
    for (int16_t i = 1; i <= 8; ++i) if (i != 4) push(i, (uint32_t)i * 20U);
    run(40, false);
    stats = end();
    assert(!conceals && !stats.rx_concealed_ms && stats.rx_played == 7);
    puts("PASS: gaps, long gaps, stamps, dry spell, half blocks, reset, switched off");
    return 0;
}
