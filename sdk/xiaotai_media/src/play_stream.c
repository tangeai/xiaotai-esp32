// SPDX-License-Identifier: MIT
#include "play_stream.h"

#include <stdatomic.h>
#include <string.h>
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum { STREAM_BLOCK = 64, PACKET_HEADER = 4 };

struct media_stream_buffer {
    xiaotai_media_format_t format;
    const xiaotai_audio_decoder_t *decoder;
    xiaotai_critical_t lock;
    xiaotai_mutex_t writer;
    xiaotai_signal_t space;
    atomic_uint refs;
    atomic_bool revoked;
    size_t limit, queue_bytes, scratch_bytes, pcm_capacity;
    size_t head, tail, used, frames;
    int64_t activity_us;
    uint8_t sample_bytes;
    bool finished;
    int16_t *storage;
};

static uint8_t *queue(media_stream_buffer_t *b) { return (uint8_t *)b->storage; }
static uint8_t *scratch(media_stream_buffer_t *b) { return queue(b) + b->queue_bytes; }

xiaotai_err_t media_stream_buffer_create(const xiaotai_media_format_t *format,
                                         const xiaotai_audio_decoder_t *decoder, media_stream_buffer_t **out)
{
    if (out) *out = NULL;
    if (!out || !format || format->rate_hz < 8000U || format->rate_hz > 48000U || format->channels != 1U)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!XIAOTAI_CODEC_IS_AUDIO(format->encoding) || (decoder && decoder->encoding != format->encoding) ||
        (format->encoding != XIAOTAI_CODEC_PCM16 && format->encoding != XIAOTAI_CODEC_G711A && !decoder))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    const uint8_t width = format->encoding == XIAOTAI_CODEC_PCM16 ? 2U :
        (format->encoding == XIAOTAI_CODEC_G711A || format->encoding == XIAOTAI_CODEC_G711U ? 1U : 0U);
    const size_t limit = format->rate_hz * XIAOTAI_MEDIA_STREAM_BUFFER_MS / 1000U;
    const size_t scratch_bytes = width ? STREAM_BLOCK * sizeof(int16_t) : XIAOTAI_MEDIA_PACKET_BYTES_MAX;
    const size_t pcm_capacity = width ? STREAM_BLOCK : format->rate_hz * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U;
    const size_t queue_bytes = limit * sizeof(int16_t);
    /* The synchronization objects live in internal memory. The larger audio
     * payload and the output worker's scratch do not need that restriction. */
    media_stream_buffer_t *b = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*b));
    if (!b) return XIAOTAI_ERR_NO_MEM;
    b->storage = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, queue_bytes + scratch_bytes + pcm_capacity * sizeof(int16_t));
    if (!b->storage) { xiaotai_mem_free(b); return XIAOTAI_ERR_NO_MEM; }
    const xiaotai_err_t locked = xiaotai_mutex_init(&b->writer);
    if (locked != XIAOTAI_OK) { xiaotai_mem_free(b->storage); xiaotai_mem_free(b); return locked; }
    const xiaotai_err_t signal = xiaotai_signal_init(&b->space);
    if (signal != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&b->writer);
        xiaotai_mem_free(b->storage);
        xiaotai_mem_free(b);
        return signal;
    }
    xiaotai_critical_init(&b->lock);
    atomic_init(&b->refs, 1U);
    atomic_init(&b->revoked, false);
    b->format = *format;
    b->decoder = decoder;
    b->sample_bytes = width;
    b->limit = limit;
    b->queue_bytes = queue_bytes;
    b->scratch_bytes = scratch_bytes;
    b->pcm_capacity = pcm_capacity;
    b->activity_us = xiaotai_time_us();
    *out = b;
    return XIAOTAI_OK;
}

void media_stream_buffer_retain(media_stream_buffer_t *b)
{
    if (b) (void)atomic_fetch_add_explicit(&b->refs, 1U, memory_order_relaxed);
}

void media_stream_buffer_release(media_stream_buffer_t *b)
{
    if (!b || atomic_fetch_sub_explicit(&b->refs, 1U, memory_order_acq_rel) != 1U) return;
    xiaotai_signal_deinit(&b->space);
    xiaotai_mutex_deinit(&b->writer);
    xiaotai_mem_free(b->storage);
    xiaotai_mem_free(b);
}

void media_stream_buffer_revoke(media_stream_buffer_t *b)
{
    if (b) atomic_store_explicit(&b->revoked, true, memory_order_release);
}

void media_stream_buffer_notify(media_stream_buffer_t *b) { if (b) xiaotai_signal_give(&b->space); }
const xiaotai_media_format_t *media_stream_buffer_format(const media_stream_buffer_t *b) { return &b->format; }
const xiaotai_audio_decoder_t *media_stream_buffer_decoder(const media_stream_buffer_t *b) { return b->decoder; }

int16_t *media_stream_buffer_pcm(media_stream_buffer_t *b, size_t *capacity)
{
    *capacity = b->pcm_capacity;
    return (int16_t *)(scratch(b) + b->scratch_bytes);
}

static void copy_into(uint8_t *ring, size_t cap, size_t at, const uint8_t *source, size_t bytes)
{
    const size_t room = cap - at < bytes ? cap - at : bytes;
    memcpy(ring + at, source, room);
    if (room < bytes) memcpy(ring, source + room, bytes - room);
}

static void copy_from(uint8_t *target, const uint8_t *ring, size_t cap, size_t at, size_t bytes)
{
    const size_t room = cap - at < bytes ? cap - at : bytes;
    memcpy(target, ring + at, room);
    if (room < bytes) memcpy(target + room, ring, bytes - room);
}

static uint32_t remaining_ms(int64_t deadline)
{
    if (deadline == INT64_MAX) return XIAOTAI_WAIT_FOREVER;
    const int64_t delta = deadline - xiaotai_time_us();
    return delta > 0 ? (uint32_t)((delta + 999) / 1000) : 0;
}

xiaotai_err_t media_stream_buffer_write(media_stream_buffer_t *b, const void *data, size_t size, uint32_t timeout_ms)
{
    if (!data || !size) return XIAOTAI_ERR_INVALID_ARG;
    if (!b || atomic_load_explicit(&b->revoked, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    size_t frames;
    size_t prefix = 0;
    if (b->sample_bytes) {
        if (size % b->sample_bytes) return XIAOTAI_ERR_INVALID_SIZE;
        frames = size / b->sample_bytes;
    } else {
        if (size > XIAOTAI_MEDIA_PACKET_BYTES_MAX) return XIAOTAI_ERR_INVALID_SIZE;
        frames = b->decoder->packet_samples(data, size, b->format.rate_hz);
        if (!frames || frames > b->pcm_capacity) return XIAOTAI_ERR_INVALID_SIZE;
        prefix = PACKET_HEADER;
    }
    if (frames > b->limit || size > b->queue_bytes - prefix) return XIAOTAI_ERR_INVALID_SIZE;
    const int64_t deadline = timeout_ms == XIAOTAI_WAIT_FOREVER ? INT64_MAX :
                             xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    if (!xiaotai_mutex_lock(&b->writer, timeout_ms)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t result;
    for (;;) {
        xiaotai_critical_enter(&b->lock);
        const bool open = !b->finished && !atomic_load_explicit(&b->revoked, memory_order_acquire);
        const bool fits = open && frames <= b->limit - b->frames && size + prefix <= b->queue_bytes - b->used;
        const size_t at = b->tail;
        xiaotai_critical_exit(&b->lock);
        if (!open) { result = XIAOTAI_ERR_INVALID_STATE; break; }
        if (fits) {
            /* The writer mutex owns unpublished tail bytes; the reader only
             * releases head space after copying the bytes it already owns. */
            if (prefix) {
                const uint8_t header[PACKET_HEADER] = {(uint8_t)size, (uint8_t)(size >> 8U),
                                                       (uint8_t)frames, (uint8_t)(frames >> 8U)};
                copy_into(queue(b), b->queue_bytes, at, header, prefix);
            }
            copy_into(queue(b), b->queue_bytes, (at + prefix) % b->queue_bytes, data, size);
            xiaotai_critical_enter(&b->lock);
            if (!b->finished && !atomic_load_explicit(&b->revoked, memory_order_acquire)) {
                b->tail = (at + prefix + size) % b->queue_bytes;
                b->used += prefix + size;
                b->frames += frames;
                b->activity_us = xiaotai_time_us();
                result = XIAOTAI_OK;
            } else result = XIAOTAI_ERR_INVALID_STATE;
            xiaotai_critical_exit(&b->lock);
            break;
        }
        const uint32_t wait = remaining_ms(deadline);
        if (!wait || !xiaotai_signal_take(&b->space, wait)) {
            xiaotai_critical_enter(&b->lock);
            const bool stopped = b->finished || atomic_load_explicit(&b->revoked, memory_order_acquire);
            xiaotai_critical_exit(&b->lock);
            result = stopped ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_TIMEOUT;
            break;
        }
    }
    xiaotai_mutex_unlock(&b->writer);
    return result;
}

xiaotai_err_t media_stream_buffer_finish(media_stream_buffer_t *b)
{
    if (!b || atomic_load_explicit(&b->revoked, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&b->lock);
    const bool open = !b->finished && !atomic_load_explicit(&b->revoked, memory_order_acquire);
    if (open) b->finished = true;
    xiaotai_critical_exit(&b->lock);
    if (open) media_stream_buffer_notify(b);
    return open ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

media_stream_read_t media_stream_buffer_read(media_stream_buffer_t *b, size_t raw_limit,
                                             const uint8_t **data, size_t *bytes, size_t *samples)
{
    *data = NULL;
    *bytes = *samples = 0;
    xiaotai_critical_enter(&b->lock);
    if (atomic_load_explicit(&b->revoked, memory_order_acquire)) {
        xiaotai_critical_exit(&b->lock);
        return MEDIA_STREAM_REVOKED;
    }
    if (!b->used) {
        const bool timeout = !b->finished && xiaotai_time_us() - b->activity_us >=
                             (int64_t)XIAOTAI_MEDIA_STREAM_IDLE_MS * 1000;
        const media_stream_read_t result = b->finished ? MEDIA_STREAM_END :
                                          timeout ? MEDIA_STREAM_TIMEOUT : MEDIA_STREAM_STARVED;
        xiaotai_critical_exit(&b->lock);
        return result;
    }
    const size_t at = b->head;
    const size_t prefix = b->sample_bytes ? 0U : PACKET_HEADER;
    if (prefix) {
        uint8_t header[PACKET_HEADER];
        copy_from(header, queue(b), b->queue_bytes, at, prefix);
        *bytes = (size_t)header[0] | (size_t)header[1] << 8U;
        *samples = (size_t)header[2] | (size_t)header[3] << 8U;
    } else {
        const size_t limit = raw_limit < STREAM_BLOCK ? raw_limit : STREAM_BLOCK;
        *samples = b->frames < limit ? b->frames : limit;
        *bytes = *samples * b->sample_bytes;
    }
    xiaotai_critical_exit(&b->lock);
    copy_from(scratch(b), queue(b), b->queue_bytes, (at + prefix) % b->queue_bytes, *bytes);
    *data = scratch(b);
    xiaotai_critical_enter(&b->lock);
    b->head = (at + prefix + *bytes) % b->queue_bytes;
    b->used -= prefix + *bytes;
    b->frames -= *samples;
    xiaotai_critical_exit(&b->lock);
    media_stream_buffer_notify(b);
    return MEDIA_STREAM_DATA;
}
