// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rx_repacketize.c::rx_repacketize
#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/media_rx_ingress.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {FRAME_BYTES = 160U, HALF = FRAME_BYTES / 2U, FRAME_MS = 20U, SLOT_COUNT = 10U,
      PREFILL_FRAMES = 5U, WAIT_MS = 5000U};
typedef enum {NO_INTERRUPTION, OVERLAP, RESELECT, LINK_CLOSE} interruption_t;
static uintptr_t peer;
static atomic_bool early_seen;
static bool closed_epoch, count_publications;
static interruption_t interrupt_on_publish;
static unsigned publications;
static atomic_bool subscription_replaced;
static const uint8_t *reentrant_input;
static _Thread_local unsigned allocation_depth, allocations;

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
int __real_posix_memalign(void **p, size_t align, size_t size);

void *__wrap_malloc(size_t size)
{
    if (allocation_depth) ++allocations;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t n, size_t size)
{
    if (allocation_depth) ++allocations;
    return __real_calloc(n, size);
}

void *__wrap_realloc(void *p, size_t size)
{
    if (allocation_depth) ++allocations;
    return __real_realloc(p, size);
}

int __wrap_posix_memalign(void **p, size_t align, size_t size)
{
    if (allocation_depth) ++allocations;
    return __real_posix_memalign(p, align, size);
}

static unsigned allocation_scope_begin(void)
{
    ++allocation_depth;
    return allocations;
}

static void allocation_scope_end(unsigned before)
{
    assert(allocation_depth && allocations == before);
    --allocation_depth;
}

static void frame_at(uintptr_t conn, const uint8_t *data, size_t size, uint32_t pts, uint8_t stream)
{
    const xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = stream, .timestamp_ms = pts,
        .format = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U}};
    const unsigned before = allocation_scope_begin();
    xiaotai_transport_host_frame(conn, &frame, data, size);
    allocation_scope_end(before);
}

static void frame(const uint8_t *data, size_t size, uint32_t pts) { frame_at(peer, data, size, pts, 1); }

static void before_subscribe_reply(void *ctx, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    (void)ctx;
    if (kind == XIAOTAI_TRANSPORT_HOST_SUBSCRIBE && (value & 1) && !atomic_exchange(&early_seen, true)) {
        static const uint8_t early[HALF];
        frame_at(conn, early, sizeof(early), 950U, 1U);
    }
}

uint32_t __real_xiaotai_media_ingress_epoch(uint32_t generation);
uint32_t __wrap_xiaotai_media_ingress_epoch(uint32_t generation)
{
    /* Exercise admission observing the closed side of a same-generation flush
     * even if the ring is open again by the time a fallback would run. */
    return closed_epoch ? 0U : __real_xiaotai_media_ingress_epoch(generation);
}

static void revoke_and_arm(void *ctx)
{
    const uint32_t generation = *(const uint32_t *)ctx;
    assert(xiaotai_on_owner());
    xiaotai_link_receive_stop(generation, XIAOTAI_LINK_AUDIO);
    assert(xiaotai_link_receive(generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    atomic_store(&subscription_replaced, true);
}

static void retire_pinned_receiver(uint32_t generation)
{
    static uint32_t owner_generation;
    owner_generation = generation;
    const uint32_t epoch = __real_xiaotai_media_ingress_epoch(generation);
    assert(epoch && reentrant_input);
    assert(xiaotai_run(revoke_and_arm, &owner_generation) == XIAOTAI_OK);
    const int64_t until = xiaotai_time_us() + WAIT_MS * 1000;
    while (__real_xiaotai_media_ingress_epoch(generation) == epoch && xiaotai_time_us() < until)
        xiaotai_delay_ms(1U);
    /* The retired receiver cannot be replaced until this producer unpins. */
    assert(__real_xiaotai_media_ingress_epoch(generation) != epoch && !atomic_load(&subscription_replaced));
    frame(reentrant_input, HALF, 9000U);
}

bool __real_media_downlink_push_tagged(xiaotai_downlink_t *, uint32_t, uint32_t, uint32_t, uint32_t,
                                      const void *, size_t, int64_t, uint16_t, uint8_t, uint16_t, unsigned *);
bool __wrap_media_downlink_push_tagged(xiaotai_downlink_t *ring, uint32_t epoch, uint32_t generation,
                                      uint32_t pts, uint32_t duration, const void *data, size_t size,
                                      int64_t received, uint16_t stream, uint8_t format, uint16_t samples,
                                      unsigned *evicted)
{
    const bool kept = __real_media_downlink_push_tagged(ring, epoch, generation, pts, duration, data, size,
                                                        received, stream, format, samples, evicted);
    if (kept && count_publications) ++publications;
    const interruption_t interrupt = interrupt_on_publish;
    interrupt_on_publish = NO_INTERRUPTION;
    if (interrupt == OVERLAP) {
        static const uint8_t overlapping[HALF];
        frame(overlapping, sizeof(overlapping), pts + HALF / 8U);
    } else if (interrupt == RESELECT) {
        retire_pinned_receiver(generation);
    } else if (interrupt == LINK_CLOSE) {
        xiaotai_transport_host_closed(peer);
    }
    return kept;
}

static void pop(xiaotai_downlink_t *ring, uint32_t generation, const uint8_t *expected, uint32_t pts)
{
    uint8_t data[FRAME_BYTES];
    xiaotai_downlink_packet_t packet;
    assert(xiaotai_downlink_pop(ring, data, sizeof(data), &packet));
    assert(packet.generation == generation && packet.size == sizeof(data) && packet.timestamp_ms == pts);
    assert(packet.duration_us == FRAME_MS * 1000U && !memcmp(data, expected, sizeof(data)));
}

static void reselect(void *ctx)
{
    const unsigned before = allocation_scope_begin();
    assert(xiaotai_link_receive(*(const uint32_t *)ctx, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    allocation_scope_end(before);
}

static void stop(void *ctx) { xiaotai_link_receive_stop(*(const uint32_t *)ctx, XIAOTAI_LINK_AUDIO); }

static void subscription_change(void *ctx, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    if (kind != XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) return;
    atomic_uint *changes = ctx;
    const unsigned index = atomic_fetch_add(changes, 1);
    assert(conn == peer && index < 2U);
    assert(value == (((int64_t)XIAOTAI_LINK_AUDIO << 16) | (1U << 8) | index));
}

static void flush(void *ctx) { assert(xiaotai_media_flush(*(const uint32_t *)ctx) == XIAOTAI_OK); }

static xiaotai_link_media_stats_t audio(uint32_t generation)
{
    xiaotai_link_stats_t stats;
    assert(xiaotai_link_stats(generation, &stats));
    return stats.audio;
}

static void interrupted(const uint8_t data[640], interruption_t how)
{
    publications = 0U;
    count_publications = true;
    interrupt_on_publish = how;
    frame(data, 640U, 19000U);
    count_publications = false;
    assert(interrupt_on_publish == NO_INTERRUPTION && publications == 1U);
}

static void reentrant_subscription(xiaotai_downlink_t *ring, uint32_t generation)
{
    uint8_t input[1760];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 17U + i / FRAME_BYTES);
    const xiaotai_link_media_stats_t before = audio(generation);
    xiaotai_media_stats_t media_before, media_after;
    xiaotai_media_stats(&media_before);
    atomic_store(&subscription_replaced, false);
    reentrant_input = input + 1600U;
    publications = 0;
    count_publications = true;
    interrupt_on_publish = RESELECT;
    frame(input, 640U, 1000U);
    count_publications = false;
    reentrant_input = NULL;
    product_on_owner(NULL, NULL); /* The queued replacement is complete before the next packet. */
    assert(atomic_load(&subscription_replaced));
    assert(interrupt_on_publish == NO_INTERRUPTION && publications == 1U && xiaotai_downlink_count(ring) == 1U);
    const xiaotai_link_media_stats_t after = audio(generation);
    xiaotai_media_stats(&media_after);
    assert(after.receive_closed == before.receive_closed + 1U && after.receive_refused == before.receive_refused + 1U);
    assert(media_after.rx_received == media_before.rx_received + 1U && media_after.rx_invalid == media_before.rx_invalid);
    pop(ring, generation, input, 1000U);
    frame(input + 1600U, HALF, 9000U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + 1680U, HALF, 9010U);
    pop(ring, generation, input + 1600U, 9000U);
    puts("Pinned producer: subscription epoch retires 640 B after one block; nested 80 B does not contaminate the replacement");
}

static void oversized_chunks(xiaotai_downlink_t *ring, uint32_t generation)
{
    uint8_t data[12U * FRAME_BYTES + HALF];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 17U + i / FRAME_BYTES);
    xiaotai_media_stats_t before, after;
    xiaotai_media_stats(&before);
    frame(data, 9U * FRAME_BYTES + HALF, 23000U); /* The legacy 1520-byte input. */
    xiaotai_media_stats(&after);
    assert(xiaotai_downlink_count(ring) == 8U);
    assert(after.rx_received == before.rx_received + 8U && after.rx_overflow == before.rx_overflow + 2U);
    assert(after.rx_invalid == before.rx_invalid && after.rx_packet_max_bytes == 9U * FRAME_BYTES + HALF);
    for (unsigned i = 0; i < 8U; ++i)
        pop(ring, generation, data + i * FRAME_BYTES, 23000U + i * FRAME_MS);

    /* The previous chunk's unused tail must not join the next one. */
    frame(data, HALF, 24000U);
    assert(!xiaotai_downlink_count(ring));
    frame(data + HALF, HALF, 29000U);
    pop(ring, generation, data, 24000U);

    for (unsigned i = 0; i < 6U; ++i) frame(data, FRAME_BYTES, 30000U + i * FRAME_MS);
    xiaotai_media_stats(&before);
    frame(data, sizeof(data), 31000U);
    xiaotai_media_stats(&after);
    assert(xiaotai_downlink_count(ring) == SLOT_COUNT && xiaotai_downlink_queued_us(ring) == 200000U);
    assert(after.rx_received == before.rx_received + 4U && after.rx_overflow == before.rx_overflow + 9U);
    assert(after.rx_invalid == before.rx_invalid && after.rx_packet_max_bytes == sizeof(data));
    for (unsigned i = 0; i < 6U; ++i) pop(ring, generation, data, 30000U + i * FRAME_MS);
    for (unsigned i = 0; i < 4U; ++i)
        pop(ring, generation, data + i * FRAME_BYTES, 31000U + i * FRAME_MS);

    for (unsigned i = 0; i < SLOT_COUNT - 1U; ++i) frame(data, FRAME_BYTES, 32000U + i * FRAME_MS);
    frame(data, FRAME_BYTES - 1U, 33000U);
    assert(xiaotai_downlink_count(ring) == SLOT_COUNT - 1U);
    const uint8_t last = data[FRAME_BYTES - 1U];
    xiaotai_media_stats(&before);
    /* Fault injection: only the first byte is readable. The last free slot
     * takes it; the byte length of the dropped remainder must not wrap on C3. */
    frame(&last, UINT32_MAX, 33019U);
    xiaotai_media_stats(&after);
    const uint32_t dropped = (uint32_t)(((uint64_t)UINT32_MAX - 1U + FRAME_BYTES - 1U) / FRAME_BYTES);
    assert(after.rx_received == before.rx_received + 1U && after.rx_overflow == before.rx_overflow + dropped);
    assert(after.rx_invalid == before.rx_invalid && after.rx_packet_max_bytes == UINT32_MAX);
    for (unsigned i = 0; i < SLOT_COUNT - 1U; ++i) pop(ring, generation, data, 32000U + i * FRAME_MS);
    pop(ring, generation, data, 33000U);

    xiaotai_media_packet_t packet = {.encoding = XIAOTAI_CODEC_G711A, .rate_hz = 8000U, .stream = 1U,
        .timestamp_ms = 34000U, .data = data, .size = sizeof(data)};
    assert(!xiaotai_media_push(generation, &packet)); /* General ingress still has the 1500-byte limit. */
    packet.encoding = XIAOTAI_CODEC_G711U;
    assert(!xiaotai_media_ingress_selected(generation, &packet, xiaotai_media_ingress_epoch(generation)));
    xiaotai_media_stats(&before);
    assert(!xiaotai_downlink_count(ring) && before.rx_invalid == after.rx_invalid + 2U);
    puts("Selected PCMA oversized input: 1520 B and larger, byte-exact 160 B blocks, bounded queue and tail");
}

static void armed(uintptr_t conn)
{
    xiaotai_transport_host_media_hook(NULL, NULL);
    peer = conn;
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    const uint32_t gen = status.generation;
    assert(gen && !status.active && ring && !xiaotai_downlink_count(ring));
    assert(xiaotai_downlink_capacity(ring) == XIAOTAI_DOWNLINK_BYTES(SLOT_COUNT, FRAME_BYTES));
    assert(atomic_load(&early_seen) && audio(gen).receive_closed == 1U);

    uint8_t input[640];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 17U + i / FRAME_BYTES);
    const unsigned refused = audio(gen).receive_refused;
    closed_epoch = true;
    frame(input, HALF, 975U);
    closed_epoch = false;
    assert(!xiaotai_downlink_count(ring) && audio(gen).receive_refused == refused + 1U);

    frame(input, HALF, 1000U);
    assert(!xiaotai_downlink_count(ring));
    const unsigned filtered = audio(gen).receive_filtered;
    frame_at(peer, input, HALF, 1010U, 2U);
    assert(audio(gen).receive_filtered == filtered + 1U);
    product_on_owner(reselect, &status.generation);
    frame(input + HALF, HALF, 9000U); /* Sender time is not a continuity signal. */
    assert(xiaotai_downlink_count(ring) == 1U);
    pop(ring, gen, input, 1000U);

    const uint32_t start = UINT32_MAX - 19U;
    frame(input, 341U, start);
    frame(input + 341U, 299U, 22U);
    assert(xiaotai_downlink_count(ring) == 4U);
    for (unsigned i = 0; i < 4U; ++i) pop(ring, gen, input + i * FRAME_BYTES, start + i * FRAME_MS);

    frame(input, 1U, 1550U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + 1U, FRAME_BYTES - 1U, 6000U);
    pop(ring, gen, input, 1550U);
    for (unsigned i = 0; i < FRAME_BYTES; ++i) frame(input + i, 1U, 1700U + i / 8U);
    assert(xiaotai_downlink_count(ring) == 1U);
    pop(ring, gen, input, 1700U);

    for (unsigned i = 0; i < SLOT_COUNT; ++i) frame(input, FRAME_BYTES, 10000U + i * FRAME_MS);
    frame(input, HALF, 10000U + SLOT_COUNT * FRAME_MS);
    assert(xiaotai_downlink_count(ring) == SLOT_COUNT &&
           xiaotai_downlink_queued_us(ring) == SLOT_COUNT * FRAME_MS * 1000U);
    assert(audio(gen).receive_refused == refused + 2U);
    for (unsigned i = 0; i < SLOT_COUNT; ++i) pop(ring, gen, input, 10000U + i * FRAME_MS);
    frame(input + FRAME_BYTES, HALF, 11000U);
    assert(!xiaotai_downlink_count(ring)); /* A full ring must not retain a partial frame. */
    product_on_owner(flush, &status.generation);
    frame(input + FRAME_BYTES, HALF, 12000U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + FRAME_BYTES + HALF, HALF, 12010U);
    pop(ring, gen, input + FRAME_BYTES, 12000U);

    atomic_uint changes = 0;
    xiaotai_transport_host_media_hook(subscription_change, &changes);
    frame(input, HALF, 13000U);
    product_on_owner(stop, &status.generation);
    assert(xiaotai_transport_host_settle(WAIT_MS) && atomic_load(&changes) == 1U);
    const unsigned stopped = audio(gen).receive_closed;
    frame(input + HALF, HALF, 13010U);
    assert(!xiaotai_downlink_count(ring) && audio(gen).receive_closed == stopped + 1U);
    product_on_owner(reselect, &status.generation);
    assert(atomic_load(&changes) == 2U);
    xiaotai_transport_host_media_hook(NULL, NULL);
    frame(input + FRAME_BYTES, HALF, 14000U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + FRAME_BYTES + HALF, HALF, 14010U);
    pop(ring, gen, input + FRAME_BYTES, 14000U);

    const xiaotai_link_media_stats_t before = audio(gen);
    interrupted(input, OVERLAP);
    const xiaotai_link_media_stats_t after = audio(gen);
    assert(after.receive_overlapped == before.receive_overlapped + 1U);
    assert(after.receive_refused == before.receive_refused + 1U && after.received == before.received);
    assert(xiaotai_downlink_count(ring) == 1U);
    pop(ring, gen, input, 19000U);
    frame(input + FRAME_BYTES, HALF, 20000U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + FRAME_BYTES + HALF, HALF, 20010U);
    pop(ring, gen, input + FRAME_BYTES, 20000U);

    uint8_t burst[8U * FRAME_BYTES + 1U];
    for (unsigned i = 0; i < sizeof(burst); ++i) burst[i] = (uint8_t)(i * 23U + i / FRAME_BYTES);
    const unsigned refused_at_prefill = audio(gen).receive_refused;
    for (unsigned i = 0; i < PREFILL_FRAMES; ++i)
        frame(burst + i * FRAME_BYTES, FRAME_BYTES, 20300U + i * FRAME_MS);
    assert(xiaotai_downlink_count(ring) == PREFILL_FRAMES);
    for (unsigned i = 0; i < PREFILL_FRAMES; ++i)
        pop(ring, gen, burst + i * FRAME_BYTES, 20300U + i * FRAME_MS);
    frame(burst, PREFILL_FRAMES * FRAME_BYTES, 20600U);
    assert(xiaotai_downlink_count(ring) == PREFILL_FRAMES);
    for (unsigned i = 0; i < PREFILL_FRAMES; ++i)
        pop(ring, gen, burst + i * FRAME_BYTES, 20600U + i * FRAME_MS);
    assert(audio(gen).receive_refused == refused_at_prefill);

    xiaotai_media_stats_t media_before, media_after;
    xiaotai_media_stats(&media_before);
    const unsigned refused_before = audio(gen).receive_refused;
    frame(burst, sizeof(burst), 21000U);
    xiaotai_media_stats(&media_after);
    assert(xiaotai_downlink_count(ring) == 8U && audio(gen).receive_refused == refused_before + 1U);
    assert(media_after.rx_received == media_before.rx_received + 8U &&
           media_after.rx_overflow == media_before.rx_overflow + 1U);
    for (unsigned i = 0; i < 8U; ++i) pop(ring, gen, burst + i * FRAME_BYTES, 21000U + i * FRAME_MS);
    frame(input, HALF, 22000U);
    assert(!xiaotai_downlink_count(ring));
    frame(input + HALF, HALF, 22010U);
    pop(ring, gen, input, 22000U);
    reentrant_subscription(ring, gen);
    oversized_chunks(ring, gen);
    puts("200 ms selected RX: 100 ms prefill, fragments, bounded burst, zero synchronous allocations");
}

void product_ai_fixed_receive(void)
{
    product_platform_online();
    xiaotai_platform_status_t platform;
    unsigned elapsed = 0;
    do {
        xiaotai_platform_status(&platform);
        if (platform.profile_ready && !xiaotai_platform_exchanges_busy()) break;
        xiaotai_delay_ms(5U);
        elapsed += 5U;
    } while (elapsed < WAIT_MS);
    assert(elapsed < WAIT_MS);
    product_ai_prepare();
    xiaotai_transport_host_media_hook(before_subscribe_reply, NULL);
    product_ai_connect_armed(product_ai_request(), armed);
    static const uint8_t burst[640];
    interrupted(burst, LINK_CLOSE);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    xiaotai_media_status_t media;
    elapsed = 0;
    do {
        xiaotai_media_status(&media);
        if (!media.generation) return;
        xiaotai_delay_ms(5U);
        elapsed += 5U;
    } while (elapsed < WAIT_MS);
    assert(false && "closed link did not retire the media owner");
}
