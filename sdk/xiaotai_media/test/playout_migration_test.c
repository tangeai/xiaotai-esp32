// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_playout.py
#include "downlink_ring.h"
#include "pacer.h"
#include "playout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { GENERATION = 7, EPOCH = 1, BLOCK = 320, PACKETS = 9000 };

typedef struct { uint32_t offset, samples; } pcm_packet_t;
static const xiaotai_media_format_t pcm_source = {XIAOTAI_CODEC_OPUS, 16000, 1, 20};
static int64_t clock_us;
static size_t consumed, emitted, written_blocks, pending_source;
static size_t fade_probe_offset;
static uint32_t generation;
static unsigned blocks_by_delta[3];
static bool ramp_vector, variable_rate, short_fade_probe;
static int16_t fade_first, fade_last;
static int fixed_delta;
static media_pacer_policy_t test_policy = MEDIA_PACER_TIMED;

static int16_t sample_at(size_t index)
{
    if (short_fade_probe) return 16000;
    if (ramp_vector) return index < BLOCK ? 0 : (int16_t)((int)(index - BLOCK) * 100 - 16000);
    return (int16_t)((int)(index % 30001U) - 15000);
}

int64_t __wrap_xiaotai_time_us(void) { return clock_us; }

int32_t __wrap_xiaotai_media_param(const char *name, int32_t fallback)
{
    return !strcmp(name, "playout.pacer_policy") ? (int32_t)test_policy : fallback;
}

/* The real pacer is independently checked in media_pacer. Feed each decision
 * here so the legacy source vectors exercise all three real render paths. */
int __wrap_media_pacer_timed_delta(media_pacer_t *pacer, uint32_t queued_ms, int64_t now,
                                  size_t available, size_t block)
{
    (void)queued_ms;
    assert(pacer->policy == MEDIA_PACER_TIMED && now == clock_us && block == BLOCK && !pending_source);
    int delta = variable_rate ? (int)(written_blocks % 3U) - 1 : fixed_delta;
    if (delta > 0 && available == block) delta = 0;
    pending_source = (size_t)((int)block + delta);
    return delta;
}

static uint32_t packet_samples(const uint8_t *data, size_t length, uint32_t rate)
{
    pcm_packet_t wire;
    assert(length == sizeof(wire) && rate == 16000U);
    memcpy(&wire, data, length);
    return wire.samples;
}

static xiaotai_err_t decode(void *state, const uint8_t *data, size_t length, int16_t *pcm,
                            size_t capacity, size_t *samples)
{
    (void)state;
    pcm_packet_t wire;
    assert(length == sizeof(wire));
    memcpy(&wire, data, length);
    assert(wire.samples && wire.samples <= capacity);
    for (size_t i = 0; i < wire.samples; ++i) pcm[i] = sample_at(wire.offset + i);
    *samples = wire.samples;
    return XIAOTAI_OK;
}

bool __wrap_media_rx_format_at(uint8_t slot, media_rx_format_t *format)
{
    static const xiaotai_audio_decoder_t decoder = {.packet_samples = packet_samples, .decode = decode};
    if (slot != 1U) return false;
    *format = (media_rx_format_t){.format = pcm_source, .decoder = &decoder};
    return true;
}

static int16_t faded(int16_t input, size_t out)
{
    return out < 80U ? (int16_t)((int32_t)input * (int32_t)(out + 1U) / 80) : input;
}

static xiaotai_err_t speaker_write(void *state, void *data, size_t length)
{
    (void)state;
    const int16_t *pcm = data;
    const size_t frames = length / (2U * sizeof(*pcm));
    assert(frames && frames <= BLOCK && length == frames * 2U * sizeof(*pcm));
    const size_t used = pending_source ? pending_source : frames;
    for (size_t i = 0; i < frames; ++i) {
        assert(pcm[i * 2U] == pcm[i * 2U + 1U]);
        if (!short_fade_probe && (used == frames || i == 0 || i + 1U == frames)) {
            const size_t offset = i + 1U == frames ? used - 1U : i;
            assert(pcm[i * 2U] == faded(sample_at(consumed + offset), emitted + i));
        }
        if (short_fade_probe) assert(pcm[i * 2U] == faded(16000, fade_probe_offset + i));
        if (ramp_vector && emitted >= BLOCK && i > 0)
            assert(pcm[i * 2U] >= pcm[i * 2U - 2U]);
    }
    if (short_fade_probe) {
        fade_first = pcm[0];
        fade_last = pcm[(frames - 1U) * 2U];
    }
    if (pending_source) {
        assert(frames == BLOCK && used >= BLOCK - 1U && used <= BLOCK + 1U);
        ++blocks_by_delta[used - (BLOCK - 1U)];
    }
    pending_source = 0;
    consumed += used;
    emitted += frames;
    ++written_blocks;
    return XIAOTAI_OK;
}

static bool current(uint32_t requested, uint32_t epoch)
{
    return requested == generation && epoch == EPOCH;
}

static void push(xiaotai_downlink_t *ring, uint32_t offset, unsigned samples)
{
    const pcm_packet_t wire = {.offset = offset, .samples = samples};
    assert(media_downlink_push_tagged(ring, media_downlink_epoch(ring), generation, offset / 16U,
           samples * 1000U / 16U, &wire, sizeof(wire), clock_us, 1, 1, (uint16_t)samples, NULL));
}

static media_playout_t *open_playout(xiaotai_downlink_t **ring)
{
    static const xiaotai_audio_output_t speaker = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 2,
                 .roles = {XIAOTAI_CHANNEL_MONO, XIAOTAI_CHANNEL_MONO}},
        .info = {.max_write_frames = BLOCK}, .write = speaker_write,
    };
    clock_us = 1000000;
    generation = GENERATION;
    consumed = emitted = pending_source = written_blocks = 0;
    memset(blocks_by_delta, 0, sizeof(blocks_by_delta));
    assert(xiaotai_downlink_create(XIAOTAI_DOWNLINK_BYTES(64, sizeof(pcm_packet_t)), sizeof(pcm_packet_t),
                                  ring) == XIAOTAI_OK);
    media_downlink_tagged(*ring, false, 64, 0);
    xiaotai_downlink_reset(*ring, generation);
    media_playout_t *player;
    assert(media_playout_create(&speaker, *ring, 1280, &player) == XIAOTAI_OK);
    media_playout_reset(player, generation, EPOCH, (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_VOIP);
    return player;
}

static bool step(media_playout_t *player, bool draining)
{
    static const xiaotai_media_session_t session = {.downlink = {XIAOTAI_CODEC_OPUS, 16000, 1, 20}};
    const media_playout_request_t request = {.session = &session, .generation = generation, .epoch = EPOCH,
        .current = current, .armed_audio = true, .draining = draining, .capture_clock = true};
    bool done;
    (void)media_playout_pass(player, &request, &done);
    clock_us += 20000;
    return done;
}

static uint32_t source_hash(size_t count)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < count; ++i) {
        const uint16_t sample = (uint16_t)sample_at(i);
        hash = (hash ^ (uint8_t)sample) * UINT32_C(16777619);
        hash = (hash ^ (uint8_t)(sample >> 8)) * UINT32_C(16777619);
    }
    return hash;
}

static void replay_9000(bool variable)
{
    static const unsigned steady[] = {320, 640, 1280, 1, 159, 73, 1500, 27, 7};
    static const unsigned paced[] = {320, 1, 640, 159, 1280};
    xiaotai_downlink_t *ring;
    media_playout_t *player = open_playout(&ring);
    variable_rate = variable;
    ramp_vector = false;
    fixed_delta = 0;
    uint32_t produced = 0;
    for (unsigned n = 0; n < PACKETS; ++n) {
        const unsigned samples = variable ? paced[n % 5U] : steady[n % 9U];
        push(ring, produced, samples);
        produced += samples;
        for (unsigned pass = 0; xiaotai_downlink_count(ring) >= 8U; ++pass) {
            assert(pass < 8U);
            assert(!step(player, false));
        }
    }
    bool done = false;
    for (unsigned pass = 0; pass < 144U && !done; ++pass) done = step(player, true);
    xiaotai_media_stats_t stats = {0};
    media_playout_stats(player, &stats);
    assert(done && !pending_source && !xiaotai_downlink_count(ring));
    assert(stats.rx_played == PACKETS && stats.pcm_decoded == produced && stats.pcm_written == produced);
    assert(!stats.rx_decode_failed && !stats.pcm_pending && !stats.pcm_discarded && !stats.write_failed);
    assert(consumed == produced && stats.pcm_out_hash == source_hash(produced));
    assert(stats.fast_blocks == blocks_by_delta[2] && stats.slow_blocks == blocks_by_delta[0]);
    if (variable) assert(stats.fast_blocks && stats.slow_blocks);
    else assert(emitted == produced && !stats.fast_blocks && !stats.slow_blocks);
    printf("PASS: %s 9000 legacy packets, PCM=%u, hash=%08x, 319/320/321 blocks=%u/%u/%u\n",
           variable ? "paced" : "fixed", produced, stats.pcm_out_hash,
           blocks_by_delta[0], blocks_by_delta[1], blocks_by_delta[2]);
    media_playout_destroy(player);
    xiaotai_downlink_destroy(ring);
}

static void replay_ramp(void)
{
    variable_rate = false;
    ramp_vector = true;
    for (unsigned samples = BLOCK - 1U; samples <= BLOCK + 1U; ++samples) {
        fixed_delta = 0;
        xiaotai_downlink_t *ring;
        media_playout_t *player = open_playout(&ring);
        push(ring, 0, BLOCK); /* Isolate endpoint interpolation after the new-stream fade. */
        assert(!step(player, false) && consumed == BLOCK && emitted == BLOCK);
        push(ring, BLOCK, samples);
        if (samples < BLOCK) push(ring, BLOCK + samples, 1);
        fixed_delta = (int)samples - BLOCK;
        assert(!step(player, true));
        assert(consumed == BLOCK + samples && emitted == BLOCK * 2U);
        assert(blocks_by_delta[samples - (BLOCK - 1U)] == (samples == BLOCK ? 2U : 1U));
        media_playout_destroy(player);
        xiaotai_downlink_destroy(ring);
    }
    puts("PASS: exact legacy 319/320/321 increasing ramp and rendered endpoints");
}

static void replay_short_fade(media_pacer_policy_t policy, bool new_session)
{
    const bool timed = policy == MEDIA_PACER_TIMED;
    test_policy = policy;
    short_fade_probe = true;
    fade_probe_offset = 0;
    xiaotai_downlink_t *ring;
    media_playout_t *player = open_playout(&ring);
    push(ring, 0, 7);
    if (timed) {
        for (unsigned pass = 0; pass < 3; ++pass) {
            assert(!step(player, false));
            assert(!emitted);
        }
    }
    assert(!step(player, !timed));
    assert(emitted == 7 && fade_first == 200 && fade_last == 1400);
    if (new_session) {
        assert(timed);
        xiaotai_downlink_reset(ring, ++generation);
        media_playout_reset(player, generation, EPOCH, (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_VOIP);
    }
    fade_probe_offset = new_session ? 0U : timed ? 7U : 80U;
    push(ring, 7, BLOCK);
    assert(!step(player, !timed));
    assert(emitted == 7U + BLOCK && fade_first == faded(16000, fade_probe_offset) && fade_last == 16000);
    printf("PASS: policy=%d new_session=%u short 7-sample start, next block sample0=%d\n",
           (int)policy, (unsigned)new_session, fade_first);
    media_playout_destroy(player);
    xiaotai_downlink_destroy(ring);
    short_fade_probe = false;
}

int main(int argc, char **argv)
{
    const bool fade_only = argc == 2 && !strcmp(argv[1], "short-fade");
    assert(argc == 1 || fade_only);
    if (!fade_only) {
        replay_9000(false);
        replay_9000(true);
        replay_ramp();
    }
    replay_short_fade(MEDIA_PACER_TIMED, false);
    replay_short_fade(MEDIA_PACER_TIMED, true);
    replay_short_fade(MEDIA_PACER_ADAPTIVE, false);
    replay_short_fade(MEDIA_PACER_FIXED, false);
    return 0;
}
