// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_opus_receive.py
#include "downlink_ring.h"
#include "opus_host_fixture.h"
#include "xiaotai/media_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const xiaotai_audio_decoder_t media_opus_decoder;

enum { PCM_RATE = 16000, MAX_SAMPLES = 1920, MAX_PACKET = 1500, GENERATION = 7 };
static void *open_at(uint32_t rate)
{
    const xiaotai_media_format_t format = {XIAOTAI_CODEC_OPUS, rate, 1, 20};
    void *state = NULL;
    assert(media_opus_decoder.match(&format));
    assert(media_opus_decoder.open(&format, &state) == XIAOTAI_OK && state);
    return state;
}

static void burst(void *decoder, size_t count)
{
    uint8_t packet[MAX_PACKET];
    int16_t current[MAX_SAMPLES], expected[MAX_SAMPLES];
    xiaotai_downlink_t *ring = NULL;
    /* A dedicated FIFO isolates duration/PCM from the generated product's
     * separate packet-size and backlog policy. */
    assert(xiaotai_downlink_create(XIAOTAI_DOWNLINK_BYTES(40, MAX_PACKET), MAX_PACKET, &ring) == XIAOTAI_OK);
    media_downlink_tagged(ring, false, 0, 0);
    xiaotai_downlink_reset(ring, GENERATION);
    unsigned submitted = 0;
    uint32_t fingerprint = UINT32_C(2166136261);
    for (size_t i = 0; i < count; ++i) {
        const unsigned samples = i < 10 ? 640U : count == 20 ? 960U : 320U;
        const size_t length = host_opus_packet((int)samples, packet, sizeof(packet));
        const int64_t arrival = INT64_C(1000) * (3000 + (int64_t)(i * 14U / (count - 1U)));
        for (size_t j = 0; j < length; ++j) fingerprint = (fingerprint ^ packet[j]) * UINT32_C(16777619);
        assert(media_downlink_push_tagged(ring, media_downlink_epoch(ring), GENERATION, submitted / 16U,
               samples * 1000U / 16U, packet, length, arrival, 1, 1, (uint16_t)samples, NULL));
        submitted += samples;
    }
    assert(submitted == 16000 && xiaotai_downlink_count(ring) == count);
    assert(xiaotai_downlink_queued_us(ring) == 1000000);

    int error = -1;
    OpusDecoder *reference = opus_decoder_create(PCM_RATE, 1, &error);
    assert(reference && error == 0 && media_opus_decoder.reset(decoder) == XIAOTAI_OK);
    size_t decoded = 0;
    for (size_t i = 0; i < count; ++i) {
        xiaotai_downlink_packet_t entry;
        uint16_t stream, stored;
        assert(media_downlink_pop_tagged(ring, packet, sizeof(packet), &entry, &stream, NULL, &stored));
        const unsigned samples = i < 10 ? 640U : count == 20 ? 960U : 320U;
        assert(entry.generation == GENERATION && entry.timestamp_ms == decoded / 16U);
        assert(entry.received_us == INT64_C(1000) * (3000 + (int64_t)(i * 14U / (count - 1U))));
        assert(entry.duration_us == samples * 1000U / 16U && stream == 1 && stored == samples);
        const int reference_samples = opus_decode(reference, packet, (int32_t)entry.size, expected, MAX_SAMPLES, 0);
        size_t actual_samples = 0;
        assert(media_opus_decoder.decode(decoder, packet, entry.size, current, MAX_SAMPLES, &actual_samples) == XIAOTAI_OK);
        assert(reference_samples == (int)samples && actual_samples == samples);
        assert(!memcmp(current, expected, actual_samples * sizeof(*current)));
        decoded += actual_samples;
    }
    assert(decoded == 16000 && !xiaotai_downlink_count(ring));
    printf("PASS: %zu packets / 14 ms fingerprint=%08x: FIFO/PCM=%zu samples\n", count, fingerprint, decoded);
    opus_decoder_destroy(reference);
    xiaotai_downlink_destroy(ring);
}

static void duration(void)
{
    static const int lengths[] = {40, 80, 160, 320, 640, 960, 1920};
    uint8_t packet[MAX_PACKET + 1];
    int16_t pcm[MAX_SAMPLES];
    void *decoder = open_at(PCM_RATE);
    for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) {
        const size_t length = host_opus_packet(lengths[i], packet, MAX_PACKET);
        assert(media_opus_decoder.packet_samples(packet, length, PCM_RATE) == (unsigned)lengths[i]);
        size_t count = 0;
        assert(media_opus_decoder.decode(decoder, packet, length, pcm, MAX_SAMPLES, &count) == XIAOTAI_OK);
        assert(count == (size_t)lengths[i]);
    }
    packet[0] = 3;  /* Missing frame-count byte. */
    size_t count = 42;
    assert(media_opus_decoder.decode(decoder, packet, 1, pcm, MAX_SAMPLES, &count) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!count);
    assert(media_opus_decoder.decode(decoder, packet, sizeof(packet), pcm, MAX_SAMPLES, &count) ==
           XIAOTAI_ERR_INVALID_SIZE && !count);
    burst(decoder, 20);
    burst(decoder, 40);
    media_opus_decoder.close(decoder);
}

static void configured_rates(void)
{
    static const uint32_t rates[] = {8000, 12000, 16000, 24000, 48000};
    uint8_t packet[MAX_PACKET];
    int16_t pcm[MAX_SAMPLES];
    const size_t length = host_opus_packet(320, packet, sizeof(packet));
    for (size_t i = 0; i < sizeof(rates) / sizeof(*rates); ++i) {
        void *decoder = open_at(rates[i]);
        size_t count = 0;
        assert(media_opus_decoder.decode(decoder, packet, length, pcm, MAX_SAMPLES, &count) == XIAOTAI_OK);
        assert(count == rates[i] / 50U);
        media_opus_decoder.close(decoder);
    }
}

int main(int argc, char **argv)
{
    puts(opus_get_version_string());
    (void)argv;
    assert(argc == 1);
    duration();
    configured_rates();
    return 0;
}
