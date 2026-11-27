// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_opus_receive.py
#include "opus_host_fixture.h"
#include "product_test.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai_net_host.h"

#include <stdio.h>

enum { GENERATION = 7U, OPUS_RATE = 16000U, BUFFER = 1500U };
extern const xiaotai_audio_decoder_t media_opus_decoder;
xiaotai_err_t __real_xiaotai_engine_media_init(void);
xiaotai_err_t __wrap_xiaotai_engine_media_init(void);
bool __real_xiaotai_net_host_wait_calls(xiaotai_net_host_call_t call, size_t count, uint32_t timeout_ms);
bool __wrap_xiaotai_net_host_wait_calls(xiaotai_net_host_call_t call, size_t count, uint32_t timeout_ms);
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);

static _Thread_local int64_t pushed_at_us;

int64_t __wrap_xiaotai_time_us(void)
{
    return pushed_at_us ? pushed_at_us : __real_xiaotai_time_us();
}

xiaotai_err_t __wrap_xiaotai_engine_media_init(void)
{
    const xiaotai_err_t err = __real_xiaotai_engine_media_init();
    return err ? err : xiaotai_media_decoder_register(&media_opus_decoder);
}

static void open_held(void *opaque)
{
    const xiaotai_media_session_t session = {.generation = *(uint32_t *)opaque,
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 1000U, .flags = XIAOTAI_MEDIA_HOLD};
    assert(xiaotai_media_open(&session) == XIAOTAI_OK);
}

static void close_session(void *opaque) { xiaotai_media_close(*(uint32_t *)opaque); }

static bool push_opus(uint32_t generation, uint32_t pts_ms, unsigned samples, int64_t at_us)
{
    uint8_t packet[BUFFER];
    const size_t bytes = host_opus_packet((int)samples, packet, sizeof(packet));
    const xiaotai_media_packet_t frame = {.encoding = XIAOTAI_CODEC_OPUS, .rate_hz = OPUS_RATE,
        .timestamp_ms = pts_ms, .stream = 10, .data = packet, .size = bytes};
    pushed_at_us = at_us;
    const bool accepted = xiaotai_media_push(generation, &frame);
    pushed_at_us = 0;
    return accepted;
}

static void check_gate(xiaotai_downlink_t *ring, uint32_t generation)
{
    xiaotai_media_stats_t stats;
    assert(xiaotai_media_set_speaker_muted(true) == XIAOTAI_OK);
    assert(!push_opus(generation, 0, 320, 1000000));
    xiaotai_media_stats(&stats);
    assert(stats.rx_blocked == 1 && !stats.rx_invalid && !stats.rx_received);
    assert(!xiaotai_downlink_count(ring));
    assert(xiaotai_media_set_speaker_muted(false) == XIAOTAI_OK);

    assert(!push_opus(generation - 1U, 0, 320, 1000000));
    xiaotai_media_stats(&stats);
    assert(stats.rx_stale == 1 && stats.rx_blocked == 1 && !stats.rx_received);
    assert(!xiaotai_downlink_count(ring));
    assert(push_opus(generation, 0, 320, 1000000));
    assert(xiaotai_downlink_count(ring) == 1U);
    assert(xiaotai_media_set_speaker_muted(true) == XIAOTAI_OK);
    assert(!xiaotai_downlink_count(ring));
    assert(xiaotai_media_set_speaker_muted(false) == XIAOTAI_OK);
}

static void check_pcma(xiaotai_downlink_t *ring, uint32_t generation)
{
    uint8_t packet[751] = {0};
    xiaotai_media_packet_t frame = {.encoding = XIAOTAI_CODEC_G711A, .rate_hz = 8000U,
        .timestamp_ms = 1000, .stream = 12, .data = packet, .size = 750};
    assert(xiaotai_media_push(generation, &frame));
    assert(xiaotai_downlink_queued_us(ring) == 93750U);
    frame.size = sizeof(packet);
    assert(!xiaotai_media_push(generation, &frame));
    assert(xiaotai_downlink_queued_us(ring) == 93750U);
}

static void check_burst(xiaotai_downlink_t *ring, uint32_t generation, unsigned count)
{
    unsigned samples = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned duration = i < 10U ? 640U : count == 20U ? 960U : 320U;
        const int64_t arrival = INT64_C(3000000) + (int64_t)(i * 14U / (count - 1U)) * 1000U;
        assert(push_opus(generation, samples / 16U, duration, arrival));
        samples += duration;
    }
    assert(samples == 16000U && xiaotai_downlink_count(ring) == count);
    assert(xiaotai_downlink_queued_us(ring) == 1000000U);
    const bool extra = push_opus(generation, 1000U, 320U, 3015000);
    const size_t kept = xiaotai_downlink_count(ring);
    const uint32_t buffered_us = xiaotai_downlink_queued_us(ring);
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(kept == count + extra && buffered_us == 1000000U + (extra ? 20000U : 0U));
    assert(stats.rx_received == kept && stats.rx_overflow == (unsigned)!extra);
    printf("product=%s burst=%u first_1s_pcm=%u extra20ms=%u buffered_us=%u overflow=%u\n",
           XIAOTAI_PRODUCT_NAME, count, samples, (unsigned)extra, buffered_us, stats.rx_overflow);
}

bool __wrap_xiaotai_net_host_wait_calls(xiaotai_net_host_call_t call, size_t count, uint32_t timeout_ms)
{
    const bool ready = __real_xiaotai_net_host_wait_calls(call, count, timeout_ms);
    if (!ready || call != XIAOTAI_NET_HOST_JOIN || count != 1) return ready;

    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    const xiaotai_media_format_t opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    assert(ring && xiaotai_media_can_receive(&opus));
    printf("product=%s ring_bytes=%zu reported_hold_ms=%u\n",
           XIAOTAI_PRODUCT_NAME, xiaotai_downlink_capacity(ring), info.downlink_ms);
    uint32_t gen = GENERATION;
    product_on_owner(open_held, &gen);
    check_gate(ring, gen);
    check_pcma(ring, gen);
    product_on_owner(close_session, &gen);
    puts("PASS: mute, old generation, PCMA 750/751 bytes");
    static const unsigned vectors[] = {20, 40};
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        ++gen;
        product_on_owner(open_held, &gen);
        check_burst(ring, gen, vectors[i]);
        product_on_owner(close_session, &gen);
    }
    return ready;
}
