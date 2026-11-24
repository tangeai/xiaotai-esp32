// SPDX-License-Identifier: MIT
#include "portable_audio.h"

#include <string.h>
#include "power.h"
#include "registry.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_critical_t lock;
    xiaotai_processor_stats_t stats;
    const xiaotai_audio_decoder_t *decoder;
    xiaotai_codec_t encoding;
    uint32_t rate, packet_samples, samples, frame_loss, pending_loss;
    int64_t captured_us;
    size_t size;
    uint8_t *packet;
} passthrough_t;

static int match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    if (!in || !XIAOTAI_CODEC_IS_AUDIO(in->encoding) || in->encoding == XIAOTAI_CODEC_PCM16 ||
        in->channels != 1 || !in->rate_hz || !in->frame_ms || in->frame_ms > XIAOTAI_MEDIA_PACKET_MS_MAX) return 0;
    const uint64_t product = (uint64_t)in->rate_hz * in->frame_ms;
    const uint64_t samples = product / 1000U;
    if (product % 1000U || (in->encoding == XIAOTAI_CODEC_G711A && samples > XIAOTAI_MEDIA_PACKET_BYTES_MAX)) return 0;
    return 1;
}

static void close_encoded(void *opaque)
{
    passthrough_t *pass = opaque;
    xiaotai_mem_free(pass->packet);
    xiaotai_mem_free(pass);
}

static xiaotai_err_t open_encoded(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                  xiaotai_processor_info_t *info, void **state)
{
    if (!info || !state || !match(in ? &in->desc : NULL, out ? &out->desc : NULL)) return XIAOTAI_ERR_INVALID_ARG;
    passthrough_t *pass = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*pass));
    if (!pass) return XIAOTAI_ERR_NO_MEM;
    pass->packet = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, XIAOTAI_MEDIA_PACKET_BYTES_MAX);
    if (!pass->packet) {
        close_encoded(pass);
        return XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_critical_init(&pass->lock);
    pass->encoding = in->desc.encoding;
    pass->rate = in->desc.rate_hz;
    pass->packet_samples = (uint32_t)((uint64_t)pass->rate * in->desc.frame_ms / 1000U);
    const xiaotai_media_format_t format = {.encoding = pass->encoding, .rate_hz = pass->rate, .channels = 1,
                                          .packet_ms = in->desc.frame_ms};
    pass->decoder = media_select_decoder(&format);
    *info = (xiaotai_processor_info_t){.flags = XIAOTAI_PROCESSOR_ENCODED, .rate_hz = pass->rate, .feed_frames = 0,
        .fetch_max = XIAOTAI_MEDIA_PACKET_BYTES_MAX, .feed_stack_bytes = 512, .fetch_stack_bytes = 512};
    *state = pass;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *opaque, const void *data, size_t size, int64_t end_us, uint32_t lost)
{
    passthrough_t *pass = opaque;
    const uint64_t lost_samples = (uint64_t)lost * pass->packet_samples;
    pass->pending_loss = media_add_sat_u32(pass->pending_loss, lost_samples > UINT32_MAX ? UINT32_MAX
                                                                                        : (uint32_t)lost_samples);
    uint32_t samples = 0;
    if (data && size && size <= XIAOTAI_MEDIA_PACKET_BYTES_MAX) {
        samples = pass->decoder ? pass->decoder->packet_samples(data, size, pass->rate) : pass->packet_samples;
        /* The HAL declares fixed-duration packets; a mismatch breaks the input timeline. */
        if (samples != pass->packet_samples) samples = 0;
    }
    if (!samples || pass->size) {
        pass->pending_loss = media_add_sat_u32(pass->pending_loss, pass->packet_samples);
        xiaotai_critical_enter(&pass->lock);
        if (samples) ++pass->stats.input_full;
        else ++pass->stats.errors;
        xiaotai_critical_exit(&pass->lock);
        return samples ? XIAOTAI_ERR_NO_MEM : XIAOTAI_ERR_INVALID_SIZE;
    }
    memcpy(pass->packet, data, size);
    pass->size = size;
    pass->samples = samples;
    pass->captured_us = end_us;
    pass->frame_loss = pass->pending_loss;
    pass->pending_loss = 0;
    xiaotai_critical_enter(&pass->lock);
    ++pass->stats.feeds;
    xiaotai_critical_exit(&pass->lock);
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *opaque, xiaotai_processor_frame_t *frame, uint32_t timeout_ms)
{
    (void)timeout_ms;
    passthrough_t *pass = opaque;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (pass->size == 0) return XIAOTAI_ERR_TIMEOUT;
    *frame = (xiaotai_processor_frame_t){.encoding = pass->encoding, .data = pass->packet, .size = pass->size,
        .samples = pass->samples, .captured_us = pass->captured_us, .lost_samples = pass->frame_loss};
    pass->size = 0;
    xiaotai_critical_enter(&pass->lock);
    ++pass->stats.frames;
    pass->stats.lost_samples = media_add_sat_u32(pass->stats.lost_samples, frame->lost_samples);
    xiaotai_critical_exit(&pass->lock);
    return XIAOTAI_OK;
}

static void stats(void *opaque, xiaotai_processor_stats_t *out)
{
    passthrough_t *pass = opaque;
    xiaotai_critical_enter(&pass->lock);
    *out = pass->stats;
    xiaotai_critical_exit(&pass->lock);
}

const xiaotai_audio_processor_t media_passthrough = {.name = "passthrough", .match = match, .open = open_encoded,
    .feed = feed, .fetch = fetch, .stats = stats, .close = close_encoded};
