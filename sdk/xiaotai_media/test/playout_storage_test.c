// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "downlink_ring.h"
#include "pacer.h"
#include "playout.h"
#include "resample.h"
#include "xiaotai/port/mem.h"

enum {GENERATION = 7, EPOCH = 2, CAPTURE_SAMPLES = 5000};
static int64_t clock_us = 1000000;
static bool mixed_rate, record_allocations, external_memory;
static size_t allocations[8], allocation_count;
static size_t internal_bytes;
static int16_t captured[CAPTURE_SAMPLES];
static size_t captured_count, writes, resets, maximum_packet;
static size_t prior_pending;
static bool odd_tail_decode;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    if (record_allocations) {
        assert(kind == XIAOTAI_MEM_BULK);
        assert(allocation_count < sizeof(allocations) / sizeof(*allocations));
        allocations[allocation_count++] = size;
    }
    return __real_xiaotai_mem_alloc(kind, size);
}

void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    if (record_allocations && kind == XIAOTAI_MEM_INTERNAL) internal_bytes += count * size;
    return __real_xiaotai_mem_calloc(kind, count, size);
}

bool __wrap_xiaotai_mem_has_external(void) { return external_memory; }

int64_t __wrap_xiaotai_time_us(void) { return clock_us; }

int32_t __wrap_xiaotai_media_param(const char *name, int32_t fallback)
{
    return !strcmp(name, "playout.pacer_policy") ? MEDIA_PACER_ADAPTIVE : fallback;
}

static uint32_t packet_samples(const uint8_t *data, size_t length, uint32_t rate)
{
    return data && length == 2U && data[0] <= XIAOTAI_MEDIA_PACKET_MS_MAX ? data[0] * rate / 1000U : 0;
}

static xiaotai_err_t decode(void *unused, const uint8_t *data, size_t length, int16_t *pcm,
                            size_t capacity, size_t *samples)
{
    (void)unused;
    const size_t needed = packet_samples(data, length, 16000);
    assert(needed && needed <= capacity && (uintptr_t)pcm % _Alignof(max_align_t) == 0);
    odd_tail_decode |= (prior_pending & 1U) != 0;
    if (needed > maximum_packet) maximum_packet = needed;
    const int16_t seed = (int16_t)(data[1] * 97);
    for (size_t i = 0; i < needed; ++i) pcm[i] = seed + (int16_t)(i % 401U);
    *samples = needed;
    if (data[1] == 120U) return XIAOTAI_FAIL; /* Dirty output is never committed. */
    if (data[1] == 121U) --*samples; /* Success with the wrong count is also rejected. */
    return XIAOTAI_OK;
}

static xiaotai_err_t reset(void *unused)
{
    (void)unused;
    ++resets;
    return XIAOTAI_OK;
}

static const xiaotai_audio_decoder_t variable = {
    .packet_samples = packet_samples, .decode = decode, .reset = reset,
};
extern const xiaotai_audio_decoder_t xiaotai_media_g711a_decoder;

bool __wrap_media_rx_format_at(uint8_t slot, media_rx_format_t *format)
{
    if (mixed_rate && slot == 1U) {
        *format = (media_rx_format_t){.format = {XIAOTAI_CODEC_OPUS, 16000, 1, 20}, .decoder = &variable};
        return true;
    }
    if (slot != (mixed_rate ? 2U : 1U)) return false;
    *format = (media_rx_format_t){.format = {XIAOTAI_CODEC_G711A, 8000, 1, 20},
                               .decoder = &xiaotai_media_g711a_decoder,
                               .state = (void *)(uintptr_t)8000};
    return true;
}

static xiaotai_err_t write_pcm(void *unused, void *bytes, size_t length)
{
    (void)unused;
    const size_t samples = length / sizeof(*captured);
    assert(length % sizeof(*captured) == 0 && captured_count + samples <= CAPTURE_SAMPLES);
    memcpy(captured + captured_count, bytes, length);
    captured_count += samples;
    ++writes;
    return XIAOTAI_OK;
}

static bool current(uint32_t generation, uint32_t epoch)
{
    return generation == GENERATION && epoch == EPOCH;
}

static void push(xiaotai_downlink_t *ring, uint8_t slot, uint16_t stream, uint32_t stamp,
                 uint8_t duration, const uint8_t *packet, size_t size)
{
    assert(media_downlink_push_tagged(ring, media_downlink_epoch(ring), GENERATION, stamp,
           duration * 1000U, packet, size, clock_us, stream, slot, 0, NULL));
}

static uint32_t hash(const int16_t *pcm, size_t count)
{
    uint32_t h = UINT32_C(2166136261);
    for (size_t i = 0; i < count; ++i) {
        const uint16_t sample = (uint16_t)pcm[i];
        h = (h ^ (uint8_t)sample) * UINT32_C(16777619);
        h = (h ^ (uint8_t)(sample >> 8)) * UINT32_C(16777619);
    }
    return h;
}

static size_t allocations_of(size_t bytes)
{
    size_t matches = 0;
    for (size_t i = 0; i < allocation_count; ++i) matches += allocations[i] == bytes;
    return matches;
}

static size_t exercise(bool verify_storage)
{
    captured_count = writes = resets = maximum_packet = allocation_count = 0;
    internal_bytes = 0;
    prior_pending = 0;
    odd_tail_decode = false;
    xiaotai_downlink_t *ring;
    assert(xiaotai_downlink_create(4096, 512, &ring) == XIAOTAI_OK);
    media_downlink_tagged(ring, false, 0, 0);
    xiaotai_downlink_reset(ring, GENERATION);
    const xiaotai_audio_output_t speaker = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = mixed_rate ? 16000U : 8000U, .channels = 1},
        .info = {.max_write_frames = mixed_rate ? 160U : 80U}, .write = write_pcm,
    };
    media_playout_t *player;
    record_allocations = true;
    assert(media_playout_create(&speaker, ring, 500, &player) == XIAOTAI_OK);
    record_allocations = false;
    const xiaotai_media_latency_t latency = {.start_ms = 10, .base_ms = 20, .min_ms = 10, .max_ms = 500};
    xiaotai_media_session_t media = {.downlink = {XIAOTAI_CODEC_G711A, 8000, 1, 20}};
    const media_playout_request_t request = {.session = &media, .generation = GENERATION, .epoch = EPOCH,
        .armed_audio = true, .draining = true, .capture_clock = true, .current = current};
    media_playout_reset(player, GENERATION, EPOCH, latency);
    if (mixed_rate) {
        const uint8_t packets[][2] = {{20, 1}, {20, 120}, {20, 121}, {121, 1}, {120, 4}};
        for (size_t i = 0; i < sizeof(packets) / sizeof(*packets); ++i)
            push(ring, 1, 7, (uint32_t)i * 20U, packets[i][0], packets[i], sizeof(packets[i]));
    }
    uint8_t g711[160];
    for (size_t i = 0; i < sizeof(g711); ++i) g711[i] = (uint8_t)i;
    push(ring, mixed_rate ? 2U : 1U, 7, 240, 20, g711, sizeof(g711));
    if (mixed_rate) {
        const uint8_t tail[] = {10, 2};
        push(ring, 1, 7, 260, 10, tail, sizeof(tail));
    } else {
        push(ring, 1, 7, 260, 20, g711, sizeof(g711));
    }
    xiaotai_media_stats_t statistics = {0};
    bool drained = false;
    for (unsigned block = 0; block < 100U && !drained; ++block) {
        (void)media_playout_pass(player, &request, &drained);
        media_playout_stats(player, &statistics);
        prior_pending = statistics.pcm_pending;
        clock_us += 10000;
    }
    assert(drained && statistics.pcm_pending == 0 && statistics.pcm_discarded == 0);
    assert(statistics.rx_decode_failed == (mixed_rate ? 3U : 0U));
    assert(statistics.rx_played == (mixed_rate ? 4U : 2U));
    assert(statistics.pcm_decoded == (mixed_rate ? 2720U : 320U));
    assert(statistics.pcm_written == statistics.pcm_decoded);
    if (mixed_rate)
        assert(maximum_packet == 1920U && resets == 2U && statistics.fast_blocks && odd_tail_decode);
    /* Captured on the prior player with the same fixture and fake clock. */
    const uint32_t pcm_golden = mixed_rate ? UINT32_C(0xab981a5a) : UINT32_C(0x91103bc5);
    const uint32_t write_golden = mixed_rate ? UINT32_C(0x4c00ec3b) : UINT32_C(0x13de296a);
    assert(statistics.pcm_out_hash == pcm_golden && hash(captured, captured_count) == write_golden);
    assert(captured_count == (mixed_rate ? 2694U : 320U) && writes == (mixed_rate ? 17U : 4U));
    printf("mode=%s pcm=%08x output=%08x writes=%zu samples=%zu fast=%u slow=%u\n",
           mixed_rate ? "mixed" : "direct", (unsigned)statistics.pcm_out_hash,
           (unsigned)hash(captured, captured_count), writes, captured_count,
           (unsigned)statistics.fast_blocks, (unsigned)statistics.slow_blocks);
    if (verify_storage) {
        fflush(stdout);
        assert(allocations_of(mixed_rate ? 3840U : 320U) == 0);
        assert(allocations_of(mixed_rate ? 320U : 160U) == (mixed_rate ? 3U : 2U));
    }
    media_playout_destroy(player);
    xiaotai_downlink_destroy(ring);
    return internal_bytes;
}

int main(int argc, char **argv)
{
    const bool verify_storage = argc != 2 || strcmp(argv[1], "golden");
    mixed_rate = true;
    const size_t mixed_internal = exercise(verify_storage);
    mixed_rate = false;
    const size_t native_internal = exercise(verify_storage);
    external_memory = true;
    const size_t external_internal = exercise(verify_storage);
    if (verify_storage) {
        assert(mixed_internal == external_internal);
        assert(external_internal == native_internal + sizeof(media_resampler_storage_t));
    }
    return 0;
}
