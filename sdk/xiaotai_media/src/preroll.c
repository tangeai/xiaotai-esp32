// SPDX-License-Identifier: MIT
#include "preroll.h"

#include <stdatomic.h>
#include <string.h>
#include "xiaotai/media_capture.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define CAPTURE_LOCK_MS 2U
#define OWNER_LOCK_MS 20U
#define HISTORY_MS 1500U
#define WAKE_MAX_AGE_US INT64_C(1000000)
#define CAPTURE_JITTER_US INT64_C(100000)

struct media_preroll {
    xiaotai_mutex_t lock;
    int16_t *pcm;
    size_t capacity, idle_limit, head, count;
    uint32_t rate_hz, serial, token, generation;
    int64_t end_us, expires_us, replay_start_us;
    uint64_t replay_samples;
    void (*lost)(uint32_t token, void *ctx);
    void *ctx;
    media_preroll_loss_t pending;
    atomic_bool gap, discard, mic_muted;
    _Atomic uint32_t visible_token, visible_generation, visible_ms;
    _Atomic uint32_t release_token, end_generation, mic_epoch, tokens, losses, retries;
};

static int64_t sample_time(uint64_t samples, uint32_t rate_hz)
{
    const uint64_t seconds = samples / rate_hz;
    const uint64_t fraction = (uint64_t)(samples % rate_hz) * 1000000U / rate_hz;
    if (seconds > ((uint64_t)INT64_MAX - fraction) / 1000000U) return INT64_MAX;
    return (int64_t)(seconds * 1000000U + fraction);
}

static int64_t add_time(int64_t base, int64_t duration)
{
    return base > INT64_MAX - duration ? INT64_MAX : base + duration;
}

xiaotai_err_t media_preroll_create(uint32_t rate_hz, uint32_t capacity_ms, media_preroll_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    if (!rate_hz || !capacity_ms) return XIAOTAI_ERR_INVALID_ARG;
    const uint64_t capacity = (uint64_t)capacity_ms * rate_hz / 1000U;
    if (!capacity || capacity > SIZE_MAX / sizeof(int16_t)) return XIAOTAI_ERR_INVALID_SIZE;
    media_preroll_t *history = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*history));
    if (!history) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xiaotai_mutex_init(&history->lock);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(history);
        return err;
    }
    history->pcm = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)capacity * sizeof(*history->pcm));
    if (!history->pcm) {
        media_preroll_destroy(history);
        return XIAOTAI_ERR_NO_MEM;
    }
    const uint64_t idle = (uint64_t)HISTORY_MS * rate_hz / 1000U;
    history->rate_hz = rate_hz;
    history->capacity = (size_t)capacity;
    history->idle_limit = (size_t)(idle < capacity ? idle : capacity);
    atomic_init(&history->gap, false);
    atomic_init(&history->discard, false);
    atomic_init(&history->mic_muted, false);
    atomic_init(&history->mic_epoch, 0);
    atomic_init(&history->visible_token, 0);
    atomic_init(&history->visible_generation, 0);
    atomic_init(&history->visible_ms, 0);
    atomic_init(&history->release_token, 0);
    atomic_init(&history->end_generation, 0);
    atomic_init(&history->tokens, 0);
    atomic_init(&history->losses, 0);
    atomic_init(&history->retries, 0);
    *out = history;
    return XIAOTAI_OK;
}

void media_preroll_destroy(media_preroll_t *history)
{
    if (!history) return;
    xiaotai_mutex_deinit(&history->lock);
    xiaotai_mem_free(history->pcm);
    xiaotai_mem_free(history);
}

uint32_t media_preroll_capacity_ms(const media_preroll_t *history)
{
    return history ? (uint32_t)((uint64_t)history->capacity * 1000U / history->rate_hz) : 0;
}

static size_t offset(const media_preroll_t *history, size_t n)
{
    const size_t until_wrap = history->capacity - history->head;
    return n < until_wrap ? history->head + n : n - until_wrap;
}

static void discard(media_preroll_t *history, size_t samples)
{
    history->head = offset(history, samples);
    history->count -= samples;
}

static void copy_pcm(const media_preroll_t *history, int16_t *out, size_t samples)
{
    size_t before_wrap = history->capacity - history->head;
    if (before_wrap > samples) before_wrap = samples;
    memcpy(out, history->pcm + history->head, before_wrap * sizeof(*out));
    memcpy(out + before_wrap, history->pcm, (samples - before_wrap) * sizeof(*out));
}

static void publish(media_preroll_t *history)
{
    atomic_store(&history->visible_ms, (uint32_t)((uint64_t)history->count * 1000U / history->rate_hz));
    atomic_store(&history->visible_generation, history->generation);
    atomic_store(&history->visible_token, history->token);
}

static void clear(media_preroll_t *history, bool lost)
{
    if (lost && history->token) {
        history->pending = (media_preroll_loss_t){
            .token = history->token, .generation = history->generation, .lost = history->lost, .ctx = history->ctx,
        };
        atomic_fetch_add(&history->losses, 1);
    }
    history->head = history->count = 0;
    history->replay_samples = 0;
    history->token = history->generation = 0;
    history->end_us = history->expires_us = history->replay_start_us = 0;
    history->lost = NULL;
    history->ctx = NULL;
    publish(history);
}

static void reconcile(media_preroll_t *history, int64_t time_us)
{
    const uint32_t released = atomic_exchange(&history->release_token, 0);
    if (released && released == history->token && !history->generation) clear(history, false);
    const uint32_t closed = atomic_exchange(&history->end_generation, 0);
    if (closed && closed == history->generation) clear(history, false);
    if (atomic_exchange(&history->discard, false)) clear(history, false);

    const bool gap = atomic_exchange(&history->gap, false);
    if (gap || (history->token && !history->generation && time_us > history->expires_us)) clear(history, true);
}

static bool mic_allowed(const media_preroll_t *history, uint32_t epoch)
{
    return !atomic_load(&history->mic_muted) && atomic_load(&history->mic_epoch) == epoch;
}

xiaotai_err_t media_preroll_append_granted(media_preroll_t *history, const int16_t *pcm, size_t samples,
                                           int64_t end_us, uint32_t lost_samples, uint32_t mic_epoch)
{
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!mic_allowed(history, mic_epoch)) return XIAOTAI_ERR_INVALID_STATE;
    if (!pcm || !samples || samples > XIAOTAI_PROCESSOR_SAMPLES_MAX || end_us <= 0) {
        atomic_store(&history->gap, true);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!xiaotai_mutex_lock(&history->lock, CAPTURE_LOCK_MS)) {
        if (mic_allowed(history, mic_epoch)) atomic_store(&history->gap, true);
        return XIAOTAI_ERR_TIMEOUT;
    }
    if (!mic_allowed(history, mic_epoch)) {
        xiaotai_mutex_unlock(&history->lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    reconcile(history, end_us);
    const int64_t duration = sample_time(samples, history->rate_hz);
    if (lost_samples || (history->end_us && (end_us <= history->end_us ||
            end_us - history->end_us > add_time(duration, CAPTURE_JITTER_US)))) clear(history, true);

    if (history->token && samples > history->capacity - history->count) clear(history, true);
    const size_t limit = history->token ? history->capacity : history->idle_limit;
    if (samples > limit) {
        pcm += samples - limit;
        samples = limit;
    }
    if (samples > limit - history->count) discard(history, history->count - (limit - samples));
    const size_t tail = offset(history, history->count);
    size_t before_wrap = history->capacity - tail;
    if (before_wrap > samples) before_wrap = samples;
    memcpy(history->pcm + tail, pcm, before_wrap * sizeof(*pcm));
    memcpy(history->pcm, pcm + before_wrap, (samples - before_wrap) * sizeof(*pcm));
    history->count += samples;
    history->end_us = end_us;
    publish(history);
    xiaotai_mutex_unlock(&history->lock);
    return XIAOTAI_OK;
}

xiaotai_err_t media_preroll_append(media_preroll_t *history, const int16_t *pcm, size_t samples,
                                   int64_t end_us, uint32_t lost_samples)
{
    return media_preroll_append_granted(history, pcm, samples, end_us, lost_samples, 0);
}

xiaotai_err_t media_preroll_hold(media_preroll_t *history, const xiaotai_media_preroll_t *request,
                                 int64_t now_us, uint32_t *token)
{
    if (token) *token = 0;
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!request || !token) return XIAOTAI_ERR_INVALID_ARG;
    if (request->wake_us <= 0 || request->wake_us > now_us ||
        now_us - request->wake_us > WAKE_MAX_AGE_US) return XIAOTAI_ERR_INVALID_STATE;
    if (!xiaotai_mutex_lock(&history->lock, OWNER_LOCK_MS)) return XIAOTAI_ERR_TIMEOUT;
    reconcile(history, now_us);
    if (atomic_load(&history->mic_muted) || history->token || history->pending.token || !history->count ||
        history->end_us < request->wake_us || history->end_us > now_us) {
        xiaotai_mutex_unlock(&history->lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }

    const uint64_t keep_us = (uint64_t)(history->end_us - request->wake_us) +
                             (uint64_t)request->keep_before_ms * 1000U;
    const uint64_t keep = (keep_us / 1000000U) * history->rate_hz +
                          (keep_us % 1000000U) * history->rate_hz / 1000000U;
    if (keep < history->count) discard(history, history->count - (size_t)keep);
    if (++history->serial == 0) ++history->serial;
    history->token = history->serial;
    history->replay_start_us = history->end_us - sample_time(history->count, history->rate_hz);
    history->expires_us = add_time(now_us, (int64_t)request->ttl_ms * 1000);
    history->lost = request->lost;
    history->ctx = request->ctx;
    *token = history->token;
    atomic_fetch_add(&history->tokens, 1);
    publish(history);
    xiaotai_mutex_unlock(&history->lock);
    return XIAOTAI_OK;
}

void media_preroll_release(media_preroll_t *history, uint32_t token)
{
    if (!history || !token || atomic_load(&history->visible_token) != token) return;
    atomic_store(&history->release_token, token);
    if (xiaotai_mutex_lock(&history->lock, 0)) {
        reconcile(history, 0);
        xiaotai_mutex_unlock(&history->lock);
    }
}

uint32_t media_preroll_token(const media_preroll_t *history)
{
    if (!history || atomic_load(&history->mic_muted) || atomic_load(&history->gap) || atomic_load(&history->discard)) return 0;
    const uint32_t token = atomic_load(&history->visible_token);
    const uint32_t generation = atomic_load(&history->visible_generation);
    return ((!generation && atomic_load(&history->release_token) == token) ||
            (generation && atomic_load(&history->end_generation) == generation)) ? 0 : token;
}

void media_preroll_status(const media_preroll_t *history, xiaotai_media_status_t *status)
{
    if (!history || !status) return;
    status->preroll_token = media_preroll_token(history);
    status->preroll_bound = status->preroll_token && atomic_load(&history->visible_generation);
    status->preroll_ms = atomic_load(&history->mic_muted) || atomic_load(&history->gap) ||
                         atomic_load(&history->discard) ? 0 : atomic_load(&history->visible_ms);
}

void media_preroll_stats(const media_preroll_t *history, xiaotai_media_stats_t *stats)
{
    if (!history || !stats) return;
    stats->preroll_tokens = atomic_load(&history->tokens);
    stats->preroll_lost = atomic_load(&history->losses);
    stats->owner_post_retries += atomic_load(&history->retries);
}

static xiaotai_err_t validate_or_bind(media_preroll_t *history, uint32_t token, uint32_t generation,
                                     int64_t now_us)
{
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!token) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_mutex_lock(&history->lock, OWNER_LOCK_MS)) return XIAOTAI_ERR_TIMEOUT;
    reconcile(history, now_us);
    const bool bound = history->token == token && !history->generation;
    if (bound && generation) {
        history->generation = generation;
        publish(history);
    }
    xiaotai_mutex_unlock(&history->lock);
    return bound ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t media_preroll_validate(media_preroll_t *history, uint32_t token, int64_t now_us)
{
    return validate_or_bind(history, token, 0, now_us);
}

xiaotai_err_t media_preroll_bind(media_preroll_t *history, uint32_t token, uint32_t generation, int64_t now_us)
{
    return generation ? validate_or_bind(history, token, generation, now_us) : XIAOTAI_ERR_INVALID_ARG;
}

void media_preroll_discard(media_preroll_t *history, bool notify)
{
    if (!history) return;
    if (notify) atomic_store(&history->gap, true);
    else atomic_store(&history->discard, true);
    if (xiaotai_mutex_lock(&history->lock, 0)) {
        reconcile(history, 0);
        xiaotai_mutex_unlock(&history->lock);
    }
}

void media_preroll_mic_transition(media_preroll_t *history, uint32_t mic_epoch, bool muted)
{
    if (!history) return;
    if (muted) atomic_store(&history->mic_muted, true);
    atomic_store(&history->mic_epoch, mic_epoch);
    if (muted) media_preroll_discard(history, true);
    else atomic_store(&history->mic_muted, false);
}

void media_preroll_close_binding(media_preroll_t *history, uint32_t generation)
{
    if (!history || !generation || atomic_load(&history->visible_generation) != generation) return;
    atomic_store(&history->end_generation, generation);
    if (xiaotai_mutex_lock(&history->lock, 0)) {
        reconcile(history, 0);
        xiaotai_mutex_unlock(&history->lock);
    }
}

static void consume_locked(media_preroll_t *history, size_t samples)
{
    history->replay_samples = samples > UINT64_MAX - history->replay_samples ?
                              UINT64_MAX : history->replay_samples + samples;
    discard(history, samples);
    publish(history);
}

static xiaotai_err_t read_block(media_preroll_t *history, uint32_t generation, int16_t *pcm, size_t limit,
                               size_t *samples, int64_t *start_us, bool finish)
{
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!generation || !pcm || !limit || limit > XIAOTAI_PROCESSOR_SAMPLES_MAX || !start_us) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!xiaotai_mutex_lock(&history->lock, CAPTURE_LOCK_MS)) return XIAOTAI_ERR_TIMEOUT;
    reconcile(history, 0);
    if (history->generation != generation || (finish ? history->count >= limit : history->count < limit)) {
        xiaotai_mutex_unlock(&history->lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const size_t take = finish ? history->count : limit;
    *start_us = add_time(history->replay_start_us, sample_time(history->replay_samples, history->rate_hz));
    if (take) copy_pcm(history, pcm, take);
    if (finish) {
        *samples = take;
        clear(history, false);
    } else consume_locked(history, take);
    xiaotai_mutex_unlock(&history->lock);
    return XIAOTAI_OK;
}

xiaotai_err_t media_preroll_read(media_preroll_t *history, uint32_t generation,
                                 int16_t *pcm, size_t samples, int64_t *start_us)
{
    return read_block(history, generation, pcm, samples, NULL, start_us, false);
}

xiaotai_err_t media_preroll_replay(media_preroll_t *history, uint32_t generation, int16_t *scratch,
                                   size_t samples, bool (*offer)(const int16_t *, size_t, int64_t, void *),
                                   void *ctx)
{
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!generation || !scratch || !samples || samples > XIAOTAI_PROCESSOR_SAMPLES_MAX || !offer) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!xiaotai_mutex_lock(&history->lock, CAPTURE_LOCK_MS)) return XIAOTAI_ERR_TIMEOUT;
    reconcile(history, 0);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (history->generation == generation && history->count >= samples) {
        const int64_t start = add_time(history->replay_start_us, sample_time(history->replay_samples, history->rate_hz));
        copy_pcm(history, scratch, samples);
        if (offer(scratch, samples, start, ctx)) {
            consume_locked(history, samples);
            result = XIAOTAI_OK;
        } else result = XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_mutex_unlock(&history->lock);
    return result;
}

xiaotai_err_t media_preroll_finish(media_preroll_t *history, uint32_t generation, int16_t *pcm,
                                   size_t packet_samples, size_t *samples, int64_t *start_us)
{
    if (!samples) return XIAOTAI_ERR_INVALID_ARG;
    *samples = 0;
    return read_block(history, generation, pcm, packet_samples, samples, start_us, true);
}

bool media_preroll_take_loss(media_preroll_t *history, int64_t now_us, media_preroll_loss_t *event)
{
    if (!history || !event || !xiaotai_mutex_lock(&history->lock, OWNER_LOCK_MS)) return false;
    reconcile(history, now_us);
    const bool present = history->pending.token != 0;
    if (present) {
        *event = history->pending;
        history->pending = (media_preroll_loss_t){0};
    }
    xiaotai_mutex_unlock(&history->lock);
    return present;
}

xiaotai_err_t media_preroll_poll_loss(media_preroll_t *history, int64_t now_us, bool *pending)
{
    if (!pending) return XIAOTAI_ERR_INVALID_ARG;
    *pending = false;
    if (!history) return XIAOTAI_OK;
    if (!xiaotai_mutex_lock(&history->lock, 0)) return XIAOTAI_ERR_TIMEOUT;
    reconcile(history, now_us);
    *pending = history->pending.token != 0;
    xiaotai_mutex_unlock(&history->lock);
    return XIAOTAI_OK;
}

void media_preroll_post_retry(media_preroll_t *history)
{
    if (history) atomic_fetch_add(&history->retries, 1);
}
