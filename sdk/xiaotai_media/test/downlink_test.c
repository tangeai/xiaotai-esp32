// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rx_repacketize.c::rx_repacketize
/* Downlink buffer: the one-second burst contract, drops only when full, stale
 * and oversize packets, entries wrapping around the ring, resets, concurrent
 * bursts against real-time playout, the media engine sizing the buffer per
 * requirement from the modules' declared needs, and formats taken from part
 * descriptors with the one codec vocabulary. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "downlink_ring.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/port/os.h"

/* One codec vocabulary (XR-CODEC): a part descriptor's encoding, a session
 * format's and a downlink packet's are the same type, so a value passes from
 * the part or the link into the engine without a conversion table. A separate
 * enumeration in any of them selects the default branch and fails here. */
#define IS_CODEC_TYPE(expression) _Generic((expression), xiaotai_codec_t: 1, default: 0)
_Static_assert(IS_CODEC_TYPE(((const xiaotai_audio_desc_t *)0)->encoding), "part encodings are codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_media_format_t *)0)->encoding), "formats are codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_media_packet_t *)0)->encoding), "packets are codec values");

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

#define PACKET_US 20000U
static xiaotai_downlink_t *s_stress;
static atomic_bool s_producing;
static atomic_uint s_sent;
static int64_t s_epoch_us;

/* Sleep until an absolute time, like a clock-driven sender or audio DMA. */
static void wait_until(int64_t due_us)
{
    while (xiaotai_time_us() < due_us) xiaotai_delay_ms(1);
}

static size_t stress_size(uint32_t timestamp) { return 80U + (timestamp / 20U) % 81U; }

/* AI-like downlink: bursts of 50 packets (one second of audio) at once, one
 * burst per second of audio, on a 1:10 time scale (2 ms per packet). Packet
 * sizes vary, so entries of different lengths wrap around the ring. */
static void producer(void *arg)
{
    (void)arg;
    uint8_t packet[160];
    uint32_t timestamp = 0;
    for (int burst = 0; burst < 10; ++burst) {
        wait_until(s_epoch_us + (int64_t)burst * 100000);
        for (int i = 0; i < 50; ++i) {
            memset(packet, (int)(timestamp & 0xff), sizeof(packet));
            if (xiaotai_downlink_push(s_stress, 3, timestamp, PACKET_US, packet, stress_size(timestamp),
                                      xiaotai_time_us())) {
                atomic_fetch_add(&s_sent, 1);
            }
            timestamp += 20;
        }
    }
    atomic_store(&s_producing, false);
    xiaotai_task_exit();
}

/* A stand-in Opus decoder: only its presence matters for sizing. */
static int opus_match(const xiaotai_media_format_t *format) { return format->encoding == XIAOTAI_CODEC_OPUS; }
static uint32_t opus_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    (void)data;
    (void)size;
    return rate_hz / 50U;
}
static xiaotai_err_t opus_open(const xiaotai_media_format_t *format, void **state)
{
    (void)format;
    *state = NULL;
    return XIAOTAI_OK;
}
static xiaotai_err_t opus_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity,
                                 size_t *samples)
{
    (void)state;
    (void)data;
    (void)size;
    (void)pcm;
    (void)capacity;
    *samples = 0;
    return XIAOTAI_FAIL;
}
static void opus_close(void *state) { (void)state; }
static const xiaotai_audio_decoder_t s_opus = {
    .name = "opus", .encoding = XIAOTAI_CODEC_OPUS, .match = opus_match, .packet_samples = opus_samples,
    .open = opus_open, .decode = opus_decode, .close = opus_close,
};

/* A stand-in PCM16 pass-through: it serves exactly the vocabulary's PCM16. */
static int pcm_match(const xiaotai_media_format_t *format) { return format->encoding == XIAOTAI_CODEC_PCM16; }
static uint32_t pcm_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    (void)data;
    (void)rate_hz;
    return (uint32_t)(size / 2U);
}
static const xiaotai_audio_decoder_t s_pcm = {
    .name = "pcm16", .encoding = XIAOTAI_CODEC_PCM16, .match = pcm_match, .packet_samples = pcm_samples,
    .open = opus_open, .decode = opus_decode, .close = opus_close,
};

/* Descriptors as boards write them: every encoding named explicitly. */
static const xiaotai_audio_desc_t s_speaker = {
    .encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000U, .channels = 1U, .roles = {XIAOTAI_CHANNEL_MONO}};
static const xiaotai_audio_desc_t s_voice_chip = {
    .encoding = XIAOTAI_CODEC_OPUS, .rate_hz = 16000U, .channels = 1U, .roles = {XIAOTAI_CHANNEL_MIC},
    .frame_ms = 20U};

/* A session format taken field by field from a part descriptor, unconverted. */
static xiaotai_media_format_t format_of(const xiaotai_audio_desc_t *desc, uint8_t packet_ms)
{
    const xiaotai_media_format_t format = {desc->encoding, desc->rate_hz, desc->channels, packet_ms};
    return format;
}

/* Starts the engine and returns the ring size it chose; 0 without a downlink. */
static size_t engine_bytes(void)
{
    if (xiaotai_engine_media_init() != XIAOTAI_OK) return SIZE_MAX;
    xiaotai_engine_media_start();
    return xiaotai_media_downlink() ? xiaotai_downlink_capacity(xiaotai_media_downlink()) : 0;
}

int main(void)
{
    /* The configurable newest-audio policy retains tags and original packet
     * timing across an eviction and wrap without changing the entry size. */
    for (unsigned replace = 0; replace < 2; ++replace) {
        xiaotai_downlink_t *ring = NULL;
        uint8_t payload[160] = {0};
        CHECK(xiaotai_downlink_create(360, 160, &ring) == XIAOTAI_OK);
        media_downlink_tagged(ring, replace != 0, 0, 0);
        xiaotai_downlink_reset(ring, 1);
        const uint32_t epoch = media_downlink_epoch(ring);
        unsigned evictions = 0;
        CHECK(media_downlink_push_tagged(ring, epoch, 1, 0, PACKET_US, payload, 160, 10, 32, 4, 640, &evictions));
        CHECK(media_downlink_push_tagged(ring, epoch, 1, 20, PACKET_US, payload, 160, 10, 32,
                                         4 | MEDIA_DOWNLINK_CONTINUATION, 640, &evictions));
        CHECK(media_downlink_push_tagged(ring, epoch, 1, 40, PACKET_US, payload, 160, 12, 33, 5, 160, &evictions) ==
              (replace != 0));
        CHECK(evictions == replace && xiaotai_downlink_count(ring) == 2);
        xiaotai_downlink_packet_t view;
        uint16_t source, original_samples;
        uint8_t codec;
        CHECK(media_downlink_pop_tagged(ring, payload, sizeof(payload), &view, &source, &codec, &original_samples));
        CHECK(source == 32 && original_samples == 640 && view.timestamp_ms == replace * 20U);
        CHECK(codec == (4 | (replace ? MEDIA_DOWNLINK_CONTINUATION : 0)));
        CHECK(xiaotai_downlink_queued_us(ring) == PACKET_US && view.duration_us == PACKET_US);
        xiaotai_downlink_destroy(ring);
    }
    CHECK(XIAOTAI_DOWNLINK_PACKETS(1000U, 20U) == 60U);
    CHECK(XIAOTAI_DOWNLINK_PACKETS(0U, 20U) == 10U);
    CHECK(XIAOTAI_DOWNLINK_ENTRY_BYTES(160U) == 180U && XIAOTAI_DOWNLINK_ENTRY_BYTES(41U) == 64U);
    CHECK(XIAOTAI_DOWNLINK_BYTES(60U, 160U) == 61U * 180U);

    /* A one-second burst (plus headroom) arrives at once: nothing is lost,
     * also when the burst starts mid-ring and its entries wrap. */
    xiaotai_downlink_t *downlink = NULL;
    CHECK(xiaotai_downlink_create(XIAOTAI_DOWNLINK_BYTES(60U, 160U), 160, &downlink) == XIAOTAI_OK);
    CHECK(xiaotai_downlink_capacity(downlink) == 61U * 180U);
    uint8_t packet[160], out[160];
    CHECK(!xiaotai_downlink_push(downlink, 7, 0, PACKET_US, packet, sizeof(packet), 0)); /* no session yet */
    xiaotai_downlink_reset(downlink, 7);
    xiaotai_downlink_packet_t header;
    for (uint32_t round = 0; round < 3; ++round) {
        /* One smaller packet stays queued in the middle of the ring. */
        for (uint32_t i = 0; i < 25; ++i) CHECK(xiaotai_downlink_push(downlink, 7, 9999, PACKET_US, packet, 100, 0));
        for (uint32_t i = 0; i < 24; ++i) CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
        for (uint32_t i = 0; i < 60; ++i) {
            if (i == 59) {
                CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
                CHECK(header.size == 100 && header.timestamp_ms == 9999);
            }
            memset(packet, (int)i, sizeof(packet));
            CHECK(xiaotai_downlink_push(downlink, 7, i * 20, PACKET_US, packet, sizeof(packet), i));
        }
        CHECK(xiaotai_downlink_count(downlink) == 60 && xiaotai_downlink_queued_us(downlink) == 60U * PACKET_US);
        uint32_t extra = 0;
        while (xiaotai_downlink_push(downlink, 7, 1200, PACKET_US, packet, sizeof(packet), 0)) ++extra;
        CHECK(extra <= 1); /* truly full: at most the one spare entry */
        for (uint32_t i = 0; i < 60; ++i) {
            CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
            CHECK(header.timestamp_ms == i * 20 && header.size == 160 && out[0] == (uint8_t)i &&
                  header.generation == 7 && header.received_us == (int64_t)i);
        }
        while (xiaotai_downlink_pop(downlink, out, sizeof(out), &header)) {
        }
        CHECK(xiaotai_downlink_count(downlink) == 0 && xiaotai_downlink_queued_us(downlink) == 0);
    }
    xiaotai_downlink_stats_t stats;
    xiaotai_downlink_stats(downlink, &stats);
    CHECK(stats.overflow == 3 && stats.peak_packets >= 60 && stats.peak_bytes <= 61U * 180U);

    /* Small packets take only their own room, not a slot of the largest. */
    xiaotai_downlink_reset(downlink, 7);
    uint32_t small_count = 0;
    while (xiaotai_downlink_push(downlink, 7, 0, PACKET_US, packet, 40, 0)) ++small_count;
    CHECK(small_count == (61U * 180U) / XIAOTAI_DOWNLINK_ENTRY_BYTES(40U));

    /* Other sessions' packets and oversize packets are dropped and counted. */
    xiaotai_downlink_reset(downlink, 7);
    xiaotai_downlink_stats(downlink, &stats);
    const uint32_t stale_before = stats.stale, oversize_before = stats.oversize;
    CHECK(!xiaotai_downlink_push(downlink, 6, 0, PACKET_US, packet, sizeof(packet), 0));
    uint8_t big[200] = {0};
    CHECK(!xiaotai_downlink_push(downlink, 7, 0, PACKET_US, big, sizeof(big), 0));
    xiaotai_downlink_stats(downlink, &stats);
    CHECK(stats.stale == stale_before + 1 && stats.oversize == oversize_before + 1);

    /* A reset drops everything held and switches sessions. */
    for (int i = 0; i < 10; ++i) CHECK(xiaotai_downlink_push(downlink, 7, 0, PACKET_US, packet, sizeof(packet), 0));
    xiaotai_downlink_reset(downlink, 8);
    CHECK(xiaotai_downlink_count(downlink) == 0 && xiaotai_downlink_queued_us(downlink) == 0);
    CHECK(!xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
    CHECK(!xiaotai_downlink_push(downlink, 7, 0, PACKET_US, packet, sizeof(packet), 0));
    CHECK(xiaotai_downlink_push(downlink, 8, 0, PACKET_US, packet, sizeof(packet), 0));
    /* A consumer buffer smaller than the packet drops it rather than stalling. */
    uint8_t small[8];
    CHECK(!xiaotai_downlink_pop(downlink, small, sizeof(small), &header));
    CHECK(xiaotai_downlink_count(downlink) == 0);
    xiaotai_downlink_destroy(downlink);
    CHECK(xiaotai_downlink_create(100, 160, &downlink) == XIAOTAI_ERR_INVALID_ARG); /* not one entry */

    /* Concurrent bursts of mixed sizes against real-time playout: no loss, order kept. */
    CHECK(xiaotai_downlink_create(XIAOTAI_DOWNLINK_BYTES(60U, 160U), 160, &s_stress) == XIAOTAI_OK);
    xiaotai_downlink_reset(s_stress, 3);
    atomic_store(&s_producing, true);
    s_epoch_us = xiaotai_time_us();
    xiaotai_task_t task;
    const xiaotai_task_config_t config = {.name = "producer", .entry = producer, .stack_bytes = 8192,
                                          .stack = XIAOTAI_MEM_INTERNAL, .priority = 5, .core = XIAOTAI_CORE_ANY};
    CHECK(xiaotai_task_create(&config, &task) == XIAOTAI_OK);
    uint32_t received = 0, expected_timestamp = 0;
    while (atomic_load(&s_producing) || xiaotai_downlink_count(s_stress) > 0) {
        /* Playout takes one packet per 2 ms on an absolute schedule, like DMA. */
        wait_until(s_epoch_us + (int64_t)received * 2000);
        if (xiaotai_downlink_pop(s_stress, out, sizeof(out), &header)) {
            CHECK(header.timestamp_ms == expected_timestamp && out[0] == (uint8_t)(expected_timestamp & 0xff));
            CHECK(header.size == stress_size(expected_timestamp));
            expected_timestamp += 20;
            ++received;
        } else {
            xiaotai_delay_ms(1);
        }
    }
    xiaotai_downlink_stats(s_stress, &stats);
    CHECK(received == 500 && atomic_load(&s_sent) == 500 && stats.overflow == 0 && stats.stale == 0);
    xiaotai_downlink_destroy(s_stress);

    /* The engine sizes each requirement on its own and keeps the largest;
     * G.711 is stored as nominal 160-byte pieces whatever max_packet_bytes
     * says, and a format this build cannot decode does not count. */
    const xiaotai_media_requirement_t ai = {
        .downlink_burst_ms = 1000, .packet_ms = 20, .max_packet_bytes = 160,
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K};
    const xiaotai_media_requirement_t room = {
        .downlink_burst_ms = 200, .packet_ms = 20, .max_packet_bytes = 640,
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .latency = XIAOTAI_MEDIA_LATENCY_RELAYED};
    const xiaotai_media_requirement_t stream_opus = {
        .downlink_burst_ms = 200, .packet_ms = 20, .max_packet_bytes = 640,
        .downlink = XIAOTAI_MEDIA_FORMAT_OPUS_16K};
    const xiaotai_media_requirement_t preroll_only = {.preroll_ms = 20000};
    CHECK(xiaotai_media_require(&ai) == XIAOTAI_OK && xiaotai_media_require(&room) == XIAOTAI_OK);
    CHECK(xiaotai_media_require(&stream_opus) == XIAOTAI_OK && xiaotai_media_require(&preroll_only) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(60U, 160U)); /* AI: 1200 ms of 180-byte entries */
    CHECK(xiaotai_media_require(&ai) == XIAOTAI_ERR_INVALID_STATE); /* too late once sized */
    xiaotai_media_reset();

    /* A relayed path keeps what its pacer may hold (560 + 320 ms), not only its burst. */
    CHECK(xiaotai_media_require(&room) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(44U, 160U));
    xiaotai_media_reset();

    /* With a decoder registered the Opus stream counts, with its declared largest packet. */
    CHECK(xiaotai_media_decoder_register(&s_opus) == XIAOTAI_OK);
    CHECK(xiaotai_media_require(&room) == XIAOTAI_OK && xiaotai_media_require(&stream_opus) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(32U, 640U)); /* BURSTY pacing: 640 ms */
    xiaotai_media_reset();

    /* A framed downlink with a declared bitrate costs what its audio carries:
     * a voice call of 16 kHz Opus (VOIP pacing keeps 100 + 160 ms, above the
     * 0 + 200 ms burst) at 32 kbit/s with packets of up to 512 bytes takes the
     * audio, a header per packet and one spare largest entry, far below 14
     * slots of 512 bytes and below the 6 x 520-byte queue it replaces. */
    const xiaotai_media_requirement_t call_opus = {
        .packet_ms = 20, .max_packet_bytes = 512, .max_bitrate_bps = 32000,
        .uplink = XIAOTAI_MEDIA_FORMAT_OPUS_16K, .downlink = XIAOTAI_MEDIA_FORMAT_OPUS_16K,
        .latency = XIAOTAI_MEDIA_LATENCY_VOIP};
    CHECK(XIAOTAI_DOWNLINK_BYTES_AT_RATE(260U, 32000U, 13U, 512U) == 1040U + 13U * 23U + 532U);
    CHECK(xiaotai_media_decoder_register(&s_opus) == XIAOTAI_OK && xiaotai_media_require(&call_opus) == XIAOTAI_OK);
    size_t bytes = engine_bytes();
    CHECK(bytes == 1872U && bytes < XIAOTAI_DOWNLINK_BYTES(13U, 512U) && bytes <= 6U * 520U);
    xiaotai_media_reset();

    /* Small Opus packets hit the time limit long before the ring runs out of bytes. */
    static const xiaotai_media_param_t time_limit[] = {
        {"downlink.max_queued_ms", 200},
        {"downlink.drop_oldest_when_full", 1},
    };
    for (unsigned replace = 0; replace < 2; ++replace) {
        CHECK(xiaotai_media_tune(time_limit, replace ? 2 : 1) == XIAOTAI_OK);
        CHECK(xiaotai_media_decoder_register(&s_opus) == XIAOTAI_OK && xiaotai_media_require(&call_opus) == XIAOTAI_OK);
        CHECK(engine_bytes() == bytes);
        xiaotai_media_info_t info;
        xiaotai_media_info(&info);
        CHECK(info.downlink_ms == 200U);
        downlink = xiaotai_media_downlink();
        xiaotai_downlink_reset(downlink, 8);
        /* Eight 10 ms packets and three 40 ms packets reach exactly 200 ms. */
        for (unsigned i = 0; i < 8U; ++i)
            CHECK(xiaotai_downlink_push(downlink, 8, i * 10U, 10000U, packet, 40, 0));
        for (unsigned i = 0; i < 3U; ++i)
            CHECK(xiaotai_downlink_push(downlink, 8, 80U + i * 40U, 40000U, packet, 40, 0));
        CHECK(xiaotai_downlink_queued_us(downlink) == 200000U);
        CHECK(xiaotai_downlink_push(downlink, 8, 200U, PACKET_US, packet, 40, 0) == (replace != 0));
        CHECK(xiaotai_downlink_queued_us(downlink) == 200000U);
        CHECK(xiaotai_downlink_count(downlink) == 11U - replace);
        CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header) && header.timestamp_ms == replace * 20U);
        xiaotai_downlink_stats(downlink, &stats);
        CHECK(stats.peak_us == 200000U && stats.overflow == 1U + replace);
        xiaotai_downlink_reset(downlink, 9);
        CHECK(xiaotai_downlink_queued_us(downlink) == 0);
        CHECK(xiaotai_downlink_push(downlink, 9, 0, PACKET_US, packet, 40, 0));
        xiaotai_media_reset();
    }

    /* The packet rule wins when it is the smaller bound. */
    const xiaotai_media_requirement_t small_packets = {
        .packet_ms = 20, .max_packet_bytes = 120, .max_bitrate_bps = 64000,
        .downlink = XIAOTAI_MEDIA_FORMAT_OPUS_16K, .latency = XIAOTAI_MEDIA_LATENCY_VOIP};
    CHECK(xiaotai_media_decoder_register(&s_opus) == XIAOTAI_OK && xiaotai_media_require(&small_packets) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(13U, 120U));
    xiaotai_media_reset();

    /* A bitrate alone: the largest packet is what 120 ms carries at that rate. */
    const xiaotai_media_requirement_t rate_only = {
        .packet_ms = 20, .max_bitrate_bps = 24000, .downlink = XIAOTAI_MEDIA_FORMAT_OPUS_16K,
        .latency = XIAOTAI_MEDIA_LATENCY_VOIP};
    CHECK(xiaotai_media_decoder_register(&s_opus) == XIAOTAI_OK && xiaotai_media_require(&rate_only) == XIAOTAI_OK);
    bytes = engine_bytes();
    CHECK(bytes == ((XIAOTAI_DOWNLINK_BYTES_AT_RATE(260U, 24000U, 13U, 360U) + 3U) & ~(size_t)3U));
    xiaotai_downlink_stats_t ring_stats;
    uint8_t opus_packet[400] = {0};
    xiaotai_downlink_reset(xiaotai_media_downlink(), 5);
    CHECK(xiaotai_downlink_push(xiaotai_media_downlink(), 5, 0, PACKET_US, opus_packet, 360, 0));
    CHECK(!xiaotai_downlink_push(xiaotai_media_downlink(), 5, 20, PACKET_US, opus_packet, 361, 0));
    xiaotai_downlink_stats(xiaotai_media_downlink(), &ring_stats);
    CHECK(ring_stats.oversize == 1);
    xiaotai_media_reset();

    /* A framed downlink with neither bound is refused on every board. */
    const xiaotai_media_requirement_t unbounded = {.packet_ms = 20, .downlink = XIAOTAI_MEDIA_FORMAT_OPUS_16K};
    CHECK(xiaotai_media_require(&unbounded) == XIAOTAI_ERR_INVALID_ARG);

    /* A product may lower the headroom (a board without external RAM). */
    static const xiaotai_media_param_t tight[] = {{"downlink.headroom_ms", 0}};
    CHECK(xiaotai_media_tune(tight, 1) == XIAOTAI_OK && xiaotai_media_param("downlink.headroom_ms", 200) == 0);
    CHECK(xiaotai_media_require(&ai) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(50U, 160U));
    xiaotai_media_reset();

    /* The configured slot count, not the variable byte occupancy, bounds
     * admission. At a full queue, keep the earliest packets in FIFO order. */
    static const xiaotai_media_param_t limited[] = {{"downlink.capacity_packets", 8}};
    CHECK(xiaotai_media_tune(limited, 1) == XIAOTAI_OK);
    CHECK(xiaotai_media_require(&ai) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(8U, 160U));
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    CHECK(info.downlink_ms == 160U);
    downlink = xiaotai_media_downlink();
    xiaotai_downlink_reset(downlink, 9);
    for (unsigned i = 0; i < 8U; ++i) {
        memset(packet, (int)i, sizeof(packet));
        CHECK(xiaotai_downlink_push(downlink, 9, i * 20U, PACKET_US, packet, sizeof(packet), 0));
    }
    CHECK(!xiaotai_downlink_push(downlink, 9, 160U, PACKET_US, packet, sizeof(packet), 0));
    for (unsigned i = 0; i < 3U; ++i) {
        CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
        CHECK(header.timestamp_ms == i * 20U && out[0] == (uint8_t)i);
    }
    for (unsigned i = 8U; i < 11U; ++i) {
        memset(packet, (int)i, sizeof(packet));
        CHECK(xiaotai_downlink_push(downlink, 9, i * 20U, PACKET_US, packet, sizeof(packet), 0));
    }
    CHECK(!xiaotai_downlink_push(downlink, 9, 220U, PACKET_US, packet, sizeof(packet), 0));
    for (unsigned i = 3U; i < 11U; ++i) {
        CHECK(xiaotai_downlink_pop(downlink, out, sizeof(out), &header));
        CHECK(header.timestamp_ms == i * 20U && out[0] == (uint8_t)i);
    }
    for (unsigned i = 0; i < 8U; ++i) {
        CHECK(xiaotai_downlink_push(downlink, 9, i * 5U, 5000U, packet, 40U, 0));
    }
    CHECK(!xiaotai_downlink_push(downlink, 9, 40U, 5000U, packet, 40U, 0));
    xiaotai_downlink_stats(downlink, &stats);
    CHECK(stats.overflow == 3U && stats.peak_packets == 8U && stats.peak_us == 160000U);
    xiaotai_media_reset();

    /* The same 200 ms reservation holds twice the G.711 payload in PCM16. */
    static const xiaotai_media_param_t pcm_limit[] = {{"downlink.capacity_packets", 10}};
    const xiaotai_media_requirement_t pcm8 = {.downlink = {XIAOTAI_CODEC_PCM16, 8000U, 1U, 20U}};
    uint8_t pcm_packet[320] = {0};
    CHECK(xiaotai_media_tune(pcm_limit, 1) == XIAOTAI_OK);
    CHECK(xiaotai_media_decoder_register(&s_pcm) == XIAOTAI_OK && xiaotai_media_require(&pcm8) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(10U, sizeof(pcm_packet)));
    downlink = xiaotai_media_downlink();
    xiaotai_downlink_reset(downlink, 9);
    for (unsigned i = 0; i < 10U; ++i) {
        CHECK(xiaotai_downlink_push(downlink, 9, i * 20U, PACKET_US, pcm_packet, sizeof(pcm_packet), 0));
    }
    CHECK(xiaotai_downlink_queued_us(downlink) == 200000U);
    CHECK(!xiaotai_downlink_push(downlink, 9, 200U, PACKET_US, pcm_packet, sizeof(pcm_packet), 0));
    xiaotai_media_reset();

    static const xiaotai_media_param_t undersized[] = {{"downlink.capacity_packets", 1}};
    static const xiaotai_media_param_t negative[] = {{"downlink.capacity_packets", -1}};
    static const xiaotai_media_param_t too_many[] = {{"downlink.capacity_packets", INT32_MAX}};
    static const xiaotai_media_param_t negative_time[] = {{"downlink.max_queued_ms", -1}};
    static const xiaotai_media_param_t too_long[] = {{"downlink.max_queued_ms", INT32_MAX}};
    CHECK(xiaotai_media_tune(undersized, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_tune(negative, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_tune(too_many, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_tune(negative_time, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_tune(too_long, 1) == XIAOTAI_ERR_INVALID_ARG);

    /* No module receives audio: no downlink at all. Malformed requirements are refused. */
    CHECK(xiaotai_media_require(&preroll_only) == XIAOTAI_OK);
    CHECK(engine_bytes() == 0);
    xiaotai_media_reset();
    const xiaotai_media_requirement_t no_duration = {.downlink = {XIAOTAI_CODEC_G711A, 8000U, 1U, 0U}};
    const xiaotai_media_requirement_t too_big = {.max_packet_bytes = XIAOTAI_MEDIA_PACKET_BYTES_MAX + 1U};
    CHECK(xiaotai_media_require(&no_duration) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(&too_big) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(NULL) == XIAOTAI_ERR_INVALID_ARG);

    /* Encodings taken from part descriptors unconverted (XR-CODEC). PCM16 is
     * the vocabulary's 5: the engine knows it as one word per sample, so a PCM
     * downlink in the speaker's format is stored as nominal 640-byte pieces
     * (16 kHz, 20 ms) once a decoder serves it, whatever max_packet_bytes says;
     * an engine that still read 0 as PCM16 would size it as a framed encoding
     * by its 160-byte packet bound. Without a decoder it is only unavailable. */
    const xiaotai_media_requirement_t pcm_down = {
        .downlink_burst_ms = 1000, .packet_ms = 20, .max_packet_bytes = 160,
        .downlink = format_of(&s_speaker, 20U)};
    CHECK(pcm_down.downlink.encoding == XIAOTAI_CODEC_PCM16 && (int)pcm_down.downlink.encoding == 5);
    CHECK(xiaotai_media_require(&pcm_down) == XIAOTAI_OK);
    CHECK(engine_bytes() == 0);
    xiaotai_media_reset();
    CHECK(xiaotai_media_decoder_register(&s_pcm) == XIAOTAI_OK && xiaotai_media_require(&pcm_down) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(60U, 640U));
    CHECK(XIAOTAI_DOWNLINK_BYTES(60U, 640U) != XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    xiaotai_media_reset();

    /* A G.711 part named with the former spelling is the built-in A-law. */
    const xiaotai_audio_desc_t g711_part = {.encoding = XIAOTAI_AUDIO_G711A, .rate_hz = 8000U, .channels = 1U};
    const xiaotai_media_requirement_t g711_down = {
        .downlink_burst_ms = 1000, .packet_ms = 20, .max_packet_bytes = 640, .downlink = format_of(&g711_part, 20U)};
    CHECK(xiaotai_media_require(&g711_down) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    xiaotai_media_reset();

    /* An encoded capture (a voice chip's Opus) is an uplink format as its part
     * delivers it; it receives nothing, so there is no downlink. */
    const xiaotai_media_requirement_t chip_up = {
        .packet_ms = 20, .uplink = format_of(&s_voice_chip, s_voice_chip.frame_ms)};
    CHECK(chip_up.uplink.encoding == XIAOTAI_CODEC_OPUS && chip_up.uplink.packet_ms == 20U);
    CHECK(xiaotai_media_require(&chip_up) == XIAOTAI_OK);
    CHECK(engine_bytes() == 0);
    xiaotai_media_reset();

    /* A descriptor that forgot its encoding (zero-filled: NONE, no longer
     * PCM16) cannot become a used direction in either way. */
    const xiaotai_audio_desc_t forgotten = {.rate_hz = 16000U, .channels = 1U};
    CHECK(forgotten.encoding == XIAOTAI_CODEC_NONE);
    const xiaotai_media_requirement_t forgotten_up = {.packet_ms = 20, .uplink = format_of(&forgotten, 20U)};
    const xiaotai_media_requirement_t forgotten_down = {
        .downlink_burst_ms = 200, .packet_ms = 20, .max_packet_bytes = 640, .downlink = format_of(&forgotten, 20U)};
    CHECK(xiaotai_media_require(&forgotten_up) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(&forgotten_down) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(engine_bytes() == 0);
    xiaotai_media_reset();

    printf("PASS: downlink byte ring, 1 s burst contract, wrap, resets, concurrent bursts, per-requirement sizing, "
           "part encodings unconverted (PCM16 is 5, zero is NONE)\n");
    return 0;
}
