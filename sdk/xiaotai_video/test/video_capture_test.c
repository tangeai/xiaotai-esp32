// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "capture.h"
#include "xiaotai/port/os.h"

enum { CAPACITY = 512 * 1024 };

static const uint8_t first[] = {
    0,0,0,1,0x67,66,0x40,31,0x80, 0,0,1,0x68,0xce, 0,0,0,1,0x65,0x80,
};
static const uint8_t missing_pps[] = {
    0,0,1,0x67,66,0x40,31,0x80, 0,0,1,0x65,0x80,
};
static const uint8_t idr_before_headers[] = {
    0,0,1,0x65,0x80, 0,0,1,0x67,66,0x40,31,0x80, 0,0,1,0x68,0xce, 0,0,1,0x65,0x80,
};
static const uint8_t picture[] = {0,0,1,0x61,0x81};
static atomic_int try_mode;
static atomic_bool fail_alloc;
static unsigned allocations;
static xiaotai_signal_t waiting, proceed, finished;

bool __real_xiaotai_mutex_lock(xiaotai_mutex_t *m, uint32_t timeout);
bool __wrap_xiaotai_mutex_lock(xiaotai_mutex_t *m, uint32_t timeout);
bool __wrap_xiaotai_mutex_lock(xiaotai_mutex_t *m, uint32_t timeout)
{
    const int mode = timeout ? 0 : atomic_exchange(&try_mode, 0);
    if (mode == 2) {
        /* Preempt an offer after reading its active ticket, before failure. */
        xiaotai_signal_give(&waiting);
        assert(xiaotai_signal_take(&proceed, 3000));
    }
    return !mode && __real_xiaotai_mutex_lock(m, timeout);
}

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes)
{
    assert(kind == XIAOTAI_MEM_BULK && bytes == CAPACITY);
    ++allocations;
    return atomic_load(&fail_alloc) ? NULL : __real_xiaotai_mem_alloc(kind, bytes);
}

static void feed(uint32_t generation, uint32_t revision, uint16_t width, bool key, const uint8_t *p, size_t n, int64_t at)
{
    const xiaotai_video_packet_t pkt = {.codec = XIAOTAI_VIDEO_CODEC_H264, .data = p, .size = n,
        .width = width, .height = 240, .key_frame = key};
    video_capture_offer(generation, revision, &pkt, at);
}

#define FEED(g, r, key, data, at) feed(g, r, 320U, key, data, sizeof(data), at)

static video_capture_status_t status(int64_t at)
{
    video_capture_status_t out;
    video_capture_status(&out, at);
    return out;
}

static void assert_stopped(int64_t now, const char *reason, size_t bytes, unsigned frames)
{
    const video_capture_status_t v = status(now);
    assert(v.phase == VIDEO_CAPTURE_STOPPED);
    assert(v.bytes == bytes && v.frames == frames && !strcmp(v.reason, reason));
}

static void clock_and_dump(void)
{
    const int64_t t = xiaotai_time_us();
    assert(video_capture_start(1, 101, t) == XIAOTAI_OK);
    FEED(1, 101, true, missing_pps, t + 1);
    FEED(1, 101, true, idr_before_headers, t + 2);
    FEED(1, 101, false, first, t + 3);
    assert(status(t + 4999999).phase == VIDEO_CAPTURE_ARMED && status(t + 4999999).bytes == 0);
    video_capture_tick(t + 5000000);
    assert_stopped(t + 5000000, "time-limit", 0, 0);
    FEED(1, 101, true, first, t + 5000001);
    assert_stopped(t + 5000001, "time-limit", 0, 0);
    video_capture_dump_t dump, second_dump;
    assert(video_capture_dump_take(false, &dump) == XIAOTAI_ERR_INVALID_STATE);
    assert(video_capture_start(1, 102, t + 6000000) == XIAOTAI_ERR_INVALID_STATE);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(1, 102, t + 7000000) == XIAOTAI_OK);
    FEED(1, 102, true, first, t + 11999999);
    video_capture_session_ended(101);
    assert(status(t + 11999999).phase == VIDEO_CAPTURE_RECORDING);
    assert(video_capture_dump_take(false, &dump) == XIAOTAI_ERR_INVALID_STATE);
    FEED(1, 102, false, picture, t + 13999998);
    FEED(1, 102, false, picture, t + 13999999);
    assert_stopped(t + 13999999, "time-limit", sizeof(first) + sizeof(picture), 2);
    assert(video_capture_dump_take(true, &dump) == XIAOTAI_ERR_INVALID_STATE);
    assert(video_capture_dump_take(false, &dump) == XIAOTAI_OK);
    assert(dump.status.width == 320 && dump.status.height == 240);
    assert(dump.status.generation == 1 && dump.status.revision == 102 && dump.status.frames == 2);
    assert(!memcmp(dump.data, first, sizeof(first)));
    assert(!memcmp(dump.data + sizeof(first), picture, sizeof(picture)));
    assert(video_capture_clear() == XIAOTAI_ERR_BUSY && !video_capture_reset());
    assert(video_capture_dump_take(false, &second_dump) == XIAOTAI_ERR_BUSY);
    video_capture_dump_release(dump.ctx);
    assert(video_capture_dump_take(false, &second_dump) == XIAOTAI_OK);
    assert(second_dump.ctx != dump.ctx && second_dump.data == dump.data);
    video_capture_dump_release(dump.ctx);
    assert(!video_capture_reset()); /* stale release cannot unpin a new export */
    video_capture_dump_release(second_dump.ctx);
    assert(video_capture_reset());
}

static void stop_causes(void)
{
    const int64_t t = xiaotai_time_us();
    assert(video_capture_start(2, 201, t) == XIAOTAI_OK);
    FEED(2, 201, true, first, t);
    feed(2, 201, 322, false, picture, sizeof(picture), t);
    assert_stopped(t, "size-changed", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(2, 202, t) == XIAOTAI_OK);
    FEED(2, 202, true, first, t);
    FEED(3, 203, false, picture, t);
    assert_stopped(t, "session-changed", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(2, 204, t) == XIAOTAI_OK);
    FEED(2, 204, true, first, t);
    video_capture_session_ended(204);
    assert_stopped(t, "session-changed", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(2, 205, t) == XIAOTAI_OK);
    FEED(2, 205, true, first, t);
    atomic_store(&try_mode, 1);
    FEED(2, 205, false, picture, t);
    FEED(2, 205, false, picture, t);
    assert_stopped(t, "contention", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(2, 206, t) == XIAOTAI_OK);
    FEED(2, 206, true, first, t);
    video_capture_stop();
    video_capture_stop();
    assert_stopped(t, "manual-stop", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);
}

static void capacity(void)
{
    static uint8_t payload[CAPACITY];
    memset(payload, 0x5a, sizeof(payload));
    const int64_t t = xiaotai_time_us();
    assert(video_capture_start(3, 301, t) == XIAOTAI_OK);
    FEED(3, 301, true, first, t);
    feed(3, 301, 320, false, payload, CAPACITY - sizeof(first) + 1U, t);
    assert_stopped(t, "capacity", sizeof(first), 1);
    assert(video_capture_clear() == XIAOTAI_OK);

    assert(video_capture_start(3, 302, t) == XIAOTAI_OK);
    FEED(3, 302, true, first, t);
    feed(3, 302, 320, false, payload, CAPACITY - sizeof(first), t);
    assert_stopped(t, "capacity", CAPACITY, 2);
    video_capture_dump_t dump;
    assert(video_capture_dump_take(false, &dump) == XIAOTAI_OK);
    assert(dump.data[CAPACITY - 1U] == 0x5a);
    video_capture_dump_release(dump.ctx);
    assert(video_capture_clear() == XIAOTAI_OK);
}

static void contending_offer(void *arg)
{
    const int64_t t = *(const int64_t *)arg;
    FEED(4, 401, false, picture, t);
    xiaotai_signal_give(&finished);
    xiaotai_task_exit();
}

static void stale_offer(void)
{
    const int64_t t = xiaotai_time_us();
    assert(xiaotai_signal_init(&waiting) == XIAOTAI_OK && xiaotai_signal_init(&proceed) == XIAOTAI_OK &&
           xiaotai_signal_init(&finished) == XIAOTAI_OK);
    assert(video_capture_start(4, 401, t) == XIAOTAI_OK);
    FEED(4, 401, true, first, t);
    atomic_store(&try_mode, 2);
    xiaotai_task_t worker;
    assert(xiaotai_task_create(&(const xiaotai_task_config_t){.name = "fixture",
        .entry = contending_offer, .arg = (void *)&t, .stack = XIAOTAI_MEM_INTERNAL,
        .stack_bytes = 8192, .core = XIAOTAI_CORE_ANY}, &worker) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&waiting, 3000));
    assert(video_capture_clear() == XIAOTAI_OK);
    assert(video_capture_start(4, 402, t) == XIAOTAI_OK);
    FEED(4, 402, true, first, t);
    xiaotai_signal_give(&proceed);
    assert(xiaotai_signal_take(&finished, 3000));
    assert(status(t).phase == VIDEO_CAPTURE_RECORDING && status(t).frames == 1);
    assert(video_capture_clear() == XIAOTAI_OK);
    xiaotai_signal_deinit(&finished);
    xiaotai_signal_deinit(&proceed);
    xiaotai_signal_deinit(&waiting);
}

int main(void)
{
    assert(status(0).phase == VIDEO_CAPTURE_EMPTY && video_capture_reset());
    video_capture_tick(0);
    FEED(1, 1, true, first, 0);
    assert(allocations == 0);
    assert(video_capture_init() == XIAOTAI_OK && video_capture_init() == XIAOTAI_OK);
    atomic_store(&fail_alloc, true);
    assert(video_capture_start(1, 1, 0) == XIAOTAI_ERR_NO_MEM && status(0).phase == VIDEO_CAPTURE_EMPTY);
    atomic_store(&fail_alloc, false);
    clock_and_dump();
    stop_causes();
    capacity();
    stale_offer();
    assert(video_capture_reset() && allocations == 12U);
    return 0;
}
