// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_opus_receive.py
#include "opus_host_fixture.h"
#include "product_opus_scene.h"
#include "product_test.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#include <stdio.h>

enum { RATE = 16000, PACKET_CAPACITY = 1500, WAIT_MS = 5000, PEER_STREAM = 7 };

static xiaotai_link_media_stats_t counts(uint32_t generation)
{
    xiaotai_link_stats_t stats;
    assert(xiaotai_link_stats(generation, &stats));
    return stats.audio;
}

static void play_until(const char *scene, unsigned packets, unsigned samples)
{
    xiaotai_media_stats_t stats;
    for (unsigned n = 0; n < WAIT_MS; n += 5) {
        xiaotai_media_stats(&stats);
        if (stats.rx_played == packets && stats.pcm_written == samples) break;
        if (stats.rx_decode_failed || stats.pcm_written > samples) break;
        xiaotai_delay_ms(5);
    }
    printf("%s %s: Opus packets=%u/%u decoded=%u written=%u/%u pending=%u bad-decode=%u\n",
           XIAOTAI_PRODUCT_NAME, scene, stats.rx_played, packets, stats.pcm_decoded, stats.pcm_written,
           samples, stats.pcm_pending, stats.rx_decode_failed);
    fflush(stdout);
    assert(stats.rx_played == packets && stats.pcm_decoded == samples && stats.pcm_written == samples);
    assert(!stats.pcm_discarded && !stats.pcm_pending && !stats.rx_decode_failed && !stats.rx_overflow);
}

void product_opus_legacy_scene(uintptr_t peer, uint32_t generation, const char *scene,
                               uint8_t receive_stream, bool accepts_peer_stream)
{
    xiaotai_media_status_t status;
    for (unsigned n = 0; n < WAIT_MS; n += 5) {
        xiaotai_media_status(&status);
        if (status.active && status.generation == generation) break;
        xiaotai_delay_ms(5);
    }
    assert(status.active && status.generation == generation);
    const xiaotai_audio_output_t *const speaker = xiaotai_hal_audio_output(0);
    assert(speaker && speaker->desc.rate_hz == RATE && speaker->info.max_write_frames == 240U);

    uint8_t packet[PACKET_CAPACITY];
    xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = receive_stream,
        .format = {XIAOTAI_LINK_CODEC_OPUS, RATE, 1U}};
    const xiaotai_link_media_stats_t before = counts(generation);
    assert(!before.received && !before.receive_filtered && !before.receive_refused);
    static const unsigned lengths[] = {40, 80, 160, 320, 640, 960, 1920};
    unsigned samples = 0;
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) {
        const size_t bytes = host_opus_packet((int)lengths[i], packet, sizeof(packet));
        frame.timestamp_ms = samples / 16U;
        xiaotai_transport_host_frame(peer, &frame, packet, bytes);
        const xiaotai_link_media_stats_t accepted = counts(generation);
        if (accepted.received != i + 1U)
            fprintf(stderr, "%s %s: %u-sample packet got received=%u filtered=%u refused=%u\n",
                    XIAOTAI_PRODUCT_NAME, scene, lengths[i], accepted.received,
                    accepted.receive_filtered, accepted.receive_refused);
        assert(accepted.received == i + 1U && !accepted.receive_filtered && !accepted.receive_refused);
        samples += lengths[i];
    }
    play_until(scene, 7U, samples);

    const size_t bytes = host_opus_packet(320, packet, sizeof(packet));
    frame.timestamp_ms = samples / 16U;
    xiaotai_transport_host_frame(peer + 1U, &frame, packet, bytes);
    frame.format.rate_hz = 8000U;
    xiaotai_transport_host_frame(peer, &frame, packet, bytes);
    frame.format.rate_hz = RATE;
    frame.format.channels = 2U;
    xiaotai_transport_host_frame(peer, &frame, packet, bytes);
    frame.format.channels = 1U;
    frame.stream = PEER_STREAM;
    xiaotai_transport_host_frame(peer, &frame, packet, bytes);
    const xiaotai_link_media_stats_t after = counts(generation);
    const unsigned extra = accepts_peer_stream ? 1U : 0U;
    printf("%s %s: accepted=%u rejected-format-or-stream=%u alternate-stream7=%u\n",
           XIAOTAI_PRODUCT_NAME, scene, after.received, after.receive_filtered, after.received - 7U);
    fflush(stdout);
    assert(after.received == 7U + extra && after.receive_filtered == 3U - extra && !after.receive_refused);
    play_until(scene, 7U + extra, samples + extra * 320U);
}
