// SPDX-License-Identifier: MIT
#include "capture.h"

#include <stdatomic.h>
#include <string.h>
#include "h264_au.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum {
    CAPTURE_BYTES = 512 * 1024, WAIT_KEY_US = 5000000, RECORD_US = 2000000,
    TAG_MASK = 3, OFFERING = 1, CONTENDED = 2, SESSION_ENDED = 3,
};

static xiaotai_mutex_t capture_lock;
static atomic_bool initialized;
/* Carry the attempt identity with the asynchronous stop cause. A delayed
 * worker can only abort the exact attempt that it encountered. */
static atomic_uint_least32_t ticket, armed_revision;
static struct {
    video_capture_status_t status;
    uint8_t *data;
    int64_t deadline;
    uint32_t next_ticket, next_dump, dump;
} capture = {.status = {.reason = "empty"}};

static int64_t after(int64_t now, int64_t delta)
{
    return now > INT64_MAX - delta ? INT64_MAX : now + delta;
}

static bool lock(void)
{
    return atomic_load(&initialized) && xiaotai_mutex_lock(&capture_lock, XIAOTAI_WAIT_FOREVER);
}

static void unlock(void) { xiaotai_mutex_unlock(&capture_lock); }

static void retire(uint_least32_t old, unsigned cause)
{
    const uint_least32_t next = (old & ~(uint_least32_t)TAG_MASK) | cause;
    (void)atomic_compare_exchange_strong(&ticket, &old, next);
}

static void stop_locked(const char *reason)
{
    const uint_least32_t t = atomic_exchange(&ticket, 0U);
    if (!t) return;
    if ((t & TAG_MASK) == CONTENDED) reason = "contention";
    else if ((t & TAG_MASK) == SESSION_ENDED) reason = "session-changed";
    capture.status.phase = VIDEO_CAPTURE_STOPPED;
    capture.status.reason = reason;
}

static void settle_locked(int64_t now)
{
    const uint_least32_t t = atomic_load(&ticket);
    if (t && ((t & TAG_MASK) != OFFERING || now >= capture.deadline))
        stop_locked("time-limit");
}

xiaotai_err_t video_capture_init(void)
{
    if (atomic_load(&initialized)) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_mutex_init(&capture_lock);
    if (err == XIAOTAI_OK) atomic_store(&initialized, true);
    return err;
}

bool video_capture_reset(void) { return video_capture_clear() == XIAOTAI_OK; }

xiaotai_err_t video_capture_start(uint32_t generation, uint32_t revision, int64_t now_us)
{
    if (!generation || !revision) return XIAOTAI_ERR_INVALID_ARG;
    if (!lock()) return XIAOTAI_ERR_INVALID_STATE;
    settle_locked(now_us);
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (!capture.data) {
        capture.data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, CAPTURE_BYTES);
        if (!capture.data) err = XIAOTAI_ERR_NO_MEM;
        else {
            capture.status = (video_capture_status_t){.phase = VIDEO_CAPTURE_ARMED, .generation = generation,
                .revision = revision, .reason = "armed"};
            capture.deadline = after(now_us, WAIT_KEY_US);
            capture.next_ticket += TAG_MASK + 1U;
            atomic_store(&armed_revision, revision);
            atomic_store(&ticket, capture.next_ticket | OFFERING);
            err = XIAOTAI_OK;
        }
    }
    unlock();
    return err;
}

void video_capture_status(video_capture_status_t *out, int64_t now_us)
{
    if (!out) return;
    *out = (video_capture_status_t){.reason = "empty"};
    if (!lock()) return;
    settle_locked(now_us);
    *out = capture.status;
    unlock();
}

void video_capture_stop(void)
{
    if (!lock()) return;
    stop_locked("manual-stop");
    unlock();
}

xiaotai_err_t video_capture_clear(void)
{
    if (!lock()) return XIAOTAI_OK;
    if (capture.dump) { unlock(); return XIAOTAI_ERR_BUSY; }
    atomic_store(&ticket, 0U);
    uint8_t *const bytes = capture.data;
    capture.data = NULL;
    capture.status = (video_capture_status_t){.reason = "empty"};
    unlock();
    xiaotai_mem_free(bytes);
    return XIAOTAI_OK;
}

xiaotai_err_t video_capture_dump_take(bool session_open, video_capture_dump_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (video_capture_dump_t){0};
    if (session_open || !lock()) return XIAOTAI_ERR_INVALID_STATE;
    settle_locked(xiaotai_time_us());
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (capture.dump) err = XIAOTAI_ERR_BUSY;
    else if (capture.status.phase == VIDEO_CAPTURE_STOPPED && capture.status.bytes) {
        if (++capture.next_dump == 0U) ++capture.next_dump;
        capture.dump = capture.next_dump;
        *out = (video_capture_dump_t){.data = capture.data, .status = capture.status,
            .ctx = (void *)(uintptr_t)capture.dump};
        err = XIAOTAI_OK;
    }
    unlock();
    return err;
}

void video_capture_dump_release(void *ctx)
{
    if (!ctx || !lock()) return;
    if ((uintptr_t)ctx == capture.dump) capture.dump = 0U;
    unlock();
}

void video_capture_offer(uint32_t generation, uint32_t revision, const xiaotai_video_packet_t *packet,
                         int64_t now_us)
{
    const uint_least32_t t = atomic_load(&ticket);
    if ((t & TAG_MASK) != OFFERING || !packet || packet->codec != XIAOTAI_VIDEO_CODEC_H264 ||
        !packet->data || !packet->size || !packet->width || !packet->height) return;
    if (!xiaotai_mutex_lock(&capture_lock, 0)) { retire(t, CONTENDED); return; }
    if (atomic_load(&ticket) != t) goto done;
    settle_locked(now_us);
    if (atomic_load(&ticket) != t) goto done;
    if (generation != capture.status.generation || revision != capture.status.revision) {
        stop_locked("session-changed");
        goto done;
    }
    if (capture.status.phase == VIDEO_CAPTURE_ARMED) {
        if (!packet->key_frame) goto done;
        const video_h264_au_t headers = video_h264_inspect(packet->data, packet->size);
        if (!headers.valid || !headers.idr_with_headers) goto done;
        capture.status.phase = VIDEO_CAPTURE_RECORDING;
        capture.status.width = packet->width;
        capture.status.height = packet->height;
        capture.status.reason = "recording";
        capture.deadline = after(now_us, RECORD_US);
    }
    if (packet->width != capture.status.width || packet->height != capture.status.height)
        stop_locked("size-changed");
    else if (packet->size > CAPTURE_BYTES - capture.status.bytes)
        stop_locked("capacity");
    else {
        memcpy(capture.data + capture.status.bytes, packet->data, packet->size);
        if (atomic_load(&ticket) == t) {
            capture.status.bytes += packet->size;
            ++capture.status.frames;
            if (capture.status.bytes == CAPTURE_BYTES) stop_locked("capacity");
        }
    }
done:
    unlock();
}

void video_capture_session_ended(uint32_t revision)
{
    const uint_least32_t t = atomic_load(&ticket);
    if ((t & TAG_MASK) == OFFERING && revision == atomic_load(&armed_revision)) retire(t, SESSION_ENDED);
}

void video_capture_tick(int64_t now_us)
{
    const uint_least32_t t = atomic_load(&ticket);
    if ((t & TAG_MASK) != OFFERING || !xiaotai_mutex_lock(&capture_lock, 0)) return;
    if (atomic_load(&ticket) == t) settle_locked(now_us);
    unlock();
}
