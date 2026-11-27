// SPDX-License-Identifier: MIT
#include "record.h"

#include <stdatomic.h>
#include <string.h>
#include "g711.h"
#include "xiaotai/framework.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_media_record_fn_t callback;
    void *ctx;
    uint32_t id, samples;
    xiaotai_err_t result;
} completion_t;

static xiaotai_critical_t s_lock;
static atomic_bool s_initialized;
static struct {
    xiaotai_media_record_t request;
    uint8_t *alaw;
    uint32_t serial, id, rate_hz, copied, taken;
    int64_t accept_us, last_end_us;
    bool writing, ended;
    xiaotai_err_t result;
    completion_t pending;
    uint32_t pending_progress, posted_id;
    uint32_t attempts, completed, failed, retries;
} s_record;

void media_record_init(void)
{
    if (!atomic_load(&s_initialized)) {
        xiaotai_critical_init(&s_lock);
        atomic_store(&s_initialized, true);
    }
}

static void finish_locked(xiaotai_err_t result)
{
    if (!s_record.id || s_record.ended) return;
    s_record.ended = true;
    s_record.result = result;
}

static void close_locked(void)
{
    if (!s_record.ended || s_record.writing) return;
    s_record.pending = (completion_t){
        .id = s_record.id, .samples = s_record.copied, .result = s_record.result,
        .callback = s_record.request.done, .ctx = s_record.request.ctx,
    };
    if (s_record.result == XIAOTAI_OK) ++s_record.completed;
    else if (s_record.result != XIAOTAI_ERR_INVALID_STATE) ++s_record.failed;
    s_record.request = (xiaotai_media_record_t){0};
    s_record.alaw = NULL;
    s_record.id = s_record.copied = 0;
    s_record.pending_progress = 0;
    s_record.ended = false;
}

void media_record_reset(void)
{
    if (!atomic_load(&s_initialized)) return;
    xiaotai_critical_enter(&s_lock);
    finish_locked(XIAOTAI_ERR_INVALID_STATE);
    close_locked();
    s_record.attempts = s_record.completed = s_record.failed = s_record.retries = 0;
    xiaotai_critical_exit(&s_lock);
    (void)media_record_service();
}

bool media_record_active(void)
{
    if (!atomic_load(&s_initialized)) return false;
    xiaotai_critical_enter(&s_lock);
    const bool active = s_record.id && !s_record.ended;
    xiaotai_critical_exit(&s_lock);
    return active;
}

void media_record_status(xiaotai_media_status_t *status)
{
    if (!atomic_load(&s_initialized) || !status) return;
    xiaotai_critical_enter(&s_lock);
    status->recording = s_record.ended ? 0 : s_record.id;
    status->recorded_samples = status->recording ? s_record.copied : 0;
    xiaotai_critical_exit(&s_lock);
}

void media_record_stats(xiaotai_media_stats_t *stats)
{
    if (!atomic_load(&s_initialized) || !stats) return;
    xiaotai_critical_enter(&s_lock);
    stats->recordings = s_record.attempts;
    stats->recordings_completed = s_record.completed;
    stats->recordings_failed = s_record.failed;
    stats->owner_post_retries += s_record.retries;
    xiaotai_critical_exit(&s_lock);
}

static xiaotai_err_t begin(const xiaotai_media_record_t *request, uint8_t *alaw, uint32_t rate_hz,
                           int64_t now_us, uint32_t *id)
{
    if (id) *id = 0;
    if (!request || (!alaw && (!request->pcm || (uintptr_t)request->pcm % _Alignof(int16_t))) ||
        !request->done || !request->samples || !id || now_us < 0) return XIAOTAI_ERR_INVALID_ARG;
    if (!rate_hz) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (request->samples > (uint64_t)rate_hz * XIAOTAI_MEDIA_RECORD_SECONDS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (request->ring_samples && (alaw || request->ring_samples > request->samples)) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_initialized)) return XIAOTAI_ERR_INVALID_STATE;

    xiaotai_critical_enter(&s_lock);
    if (s_record.id || s_record.pending.id) {
        xiaotai_critical_exit(&s_lock);
        return XIAOTAI_ERR_BUSY;
    }
    if (++s_record.serial == 0) ++s_record.serial;
    s_record.id = s_record.serial;
    s_record.request = *request;
    s_record.alaw = alaw;
    s_record.rate_hz = rate_hz;
    s_record.accept_us = now_us;
    s_record.last_end_us = 0;
    s_record.copied = s_record.taken = 0;
    ++s_record.attempts;
    *id = s_record.id;
    xiaotai_critical_exit(&s_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t media_record_begin(const xiaotai_media_record_t *request, uint32_t rate_hz,
                                 int64_t now_us, uint32_t *id)
{
    return begin(request, NULL, rate_hz, now_us, id);
}

xiaotai_err_t media_record_begin_alaw(uint8_t *data, uint32_t samples, xiaotai_media_record_fn_t done,
                                      void *ctx, uint32_t rate_hz, int64_t now_us, uint32_t *id)
{
    const xiaotai_media_record_t request = {.samples = samples, .done = done, .ctx = ctx};
    return begin(&request, data, rate_hz, now_us, id);
}

void media_record_end(uint32_t id, xiaotai_err_t result)
{
    if (!atomic_load(&s_initialized)) return;
    xiaotai_critical_enter(&s_lock);
    if (!id || id == s_record.id) {
        finish_locked(result);
        close_locked();
    }
    xiaotai_critical_exit(&s_lock);
}

void media_record_feed(const int16_t *pcm, size_t samples, int64_t end_us, uint32_t lost_samples)
{
    if (!atomic_load(&s_initialized)) return;
    xiaotai_critical_enter(&s_lock);
    if (!s_record.id || s_record.ended || s_record.writing) {
        xiaotai_critical_exit(&s_lock);
        return;
    }
    if (lost_samples || !pcm || !samples || samples > XIAOTAI_PROCESSOR_SAMPLES_MAX || end_us < 0) {
        finish_locked(XIAOTAI_FAIL);
        close_locked();
        xiaotai_critical_exit(&s_lock);
        return;
    }
    const int64_t duration = (int64_t)((uint64_t)samples * 1000000U + s_record.rate_hz - 1U) /
                             (int64_t)s_record.rate_hz;
    if (end_us - duration < s_record.accept_us) {
        xiaotai_critical_exit(&s_lock);
        return; /* Do not record an AFE backlog block straddling the request. */
    }
    if (s_record.copied && end_us <= s_record.last_end_us) {
        finish_locked(XIAOTAI_FAIL);
        close_locked();
        xiaotai_critical_exit(&s_lock);
        return;
    }
    uint32_t take = s_record.request.samples - s_record.copied;
    if (take > samples) take = (uint32_t)samples;
    const uint32_t copied = s_record.copied, ring = s_record.request.ring_samples;
    if (ring && copied + take - s_record.taken > ring) {
        finish_locked(XIAOTAI_FAIL);     /* whoever takes them away fell behind */
        close_locked();
        xiaotai_critical_exit(&s_lock);
        return;
    }
    uint8_t *const alaw = s_record.alaw;
    int16_t *const dest = s_record.request.pcm;
    s_record.writing = true;
    xiaotai_critical_exit(&s_lock);

    /* Ownership remains borrowed until this writer returns and the owner
     * dispatches done. A concurrent stop may drop this in-flight block. */
    if (alaw) xiaotai_media_alaw_encode(pcm, take, alaw + copied);
    else if (ring) {
        const uint32_t at = copied % ring, first = take < ring - at ? take : ring - at;
        memcpy(dest + at, pcm, first * sizeof(*pcm));
        memcpy(dest, pcm + first, (take - first) * sizeof(*pcm));
    } else memcpy(dest + copied, pcm, take * sizeof(*pcm));
    xiaotai_critical_enter(&s_lock);
    s_record.writing = false;
    if (!s_record.ended) {
        s_record.copied += take;
        s_record.last_end_us = end_us;
        if (s_record.copied == s_record.request.samples) finish_locked(XIAOTAI_OK);
        else if (s_record.request.progress && s_record.copied / s_record.rate_hz > copied / s_record.rate_hz)
            s_record.pending_progress = s_record.copied / s_record.rate_hz * s_record.rate_hz;
    }
    close_locked();
    xiaotai_critical_exit(&s_lock);
}

static void deliver(void *ctx)
{
    const uint32_t id = (uint32_t)(uintptr_t)ctx;
    completion_t done = {0};
    xiaotai_media_record_progress_fn_t progress = NULL;
    uint32_t samples = 0;
    void *progress_ctx = NULL;
    xiaotai_critical_enter(&s_lock);
    if (s_record.posted_id == id) {
        s_record.posted_id = 0;
        /* A terminal result replaces any queued progress for this recording. */
        if (s_record.pending.id == id) {
            done = s_record.pending;
            s_record.pending = (completion_t){0};
        } else if (s_record.id == id && !s_record.ended && s_record.pending_progress) {
            progress = s_record.request.progress;
            progress_ctx = s_record.request.ctx;
            samples = s_record.pending_progress;
            s_record.pending_progress = 0;
        }
    }
    xiaotai_critical_exit(&s_lock);
    if (done.callback) {
        done.callback(done.id, done.result, done.samples, done.ctx);
        media_session_record_output_free();
    } else if (progress) progress(id, samples, progress_ctx);
}

bool media_record_service(void)
{
    if (!atomic_load(&s_initialized)) return false;
    xiaotai_critical_enter(&s_lock);
    uint32_t post = 0;
    if (!s_record.posted_id) {
        post = s_record.pending.id;
        if (!post && !s_record.ended && s_record.pending_progress) post = s_record.id;
        s_record.posted_id = post;
    }
    xiaotai_critical_exit(&s_lock);

    if (!post) return false;
    const xiaotai_err_t report = xiaotai_run(deliver, (void *)(uintptr_t)post);
    if (report == XIAOTAI_OK) return false;
    xiaotai_critical_enter(&s_lock);
    if (s_record.posted_id == post) s_record.posted_id = 0;
    if (report == XIAOTAI_ERR_NO_MEM) ++s_record.retries;
    xiaotai_critical_exit(&s_lock);
    return true;
}

xiaotai_err_t xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id)
{
    if (!request || !request->pcm || !request->done || !request->samples || !id) return XIAOTAI_ERR_INVALID_ARG;
    *id = 0;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    return media_session_record_target(request, NULL, id);
}

xiaotai_err_t media_session_record_alaw(uint8_t *data, uint32_t samples, xiaotai_media_record_fn_t done,
                                        void *ctx, uint32_t *id)
{
    if (id) *id = 0;
    if (!data || !samples || !done || !id) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_media_record_t request = {.samples = samples, .done = done, .ctx = ctx};
    return media_session_record_target(&request, data, id);
}

void xiaotai_media_record_taken(uint32_t id, uint32_t samples)
{
    if (!id || !atomic_load(&s_initialized)) return;
    xiaotai_critical_enter(&s_lock);
    if (id == s_record.id && s_record.request.ring_samples && samples > s_record.taken && samples <= s_record.copied)
        s_record.taken = samples;
    xiaotai_critical_exit(&s_lock);
}

void xiaotai_media_record_stop(uint32_t id)
{
    if (id && xiaotai_on_owner()) media_session_record_stop(id);
}
