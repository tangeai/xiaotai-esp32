// SPDX-License-Identifier: MIT
#include "xiaotai/media_capture.h"
#include "xiaotai/media_config.h"
#include "xiaotai/port/mem.h"

#include "frontend_pairing.h"

xiaotai_err_t __real_media_platform_register(void);

#if XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL || XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND || XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE
enum { CAPTURE_RATE_HZ = 16000, CAPTURE_SAMPLES = 160 };

/* The real pairing rule selects the processor and the microphone slot.
 * Only the vendor DSP is replaced; these samples have no echo cancellation. */
typedef struct {
    uint8_t channels, microphone;
    bool ready;
    uint32_t lost;
    int64_t captured_us;
    int16_t mono[CAPTURE_SAMPLES];
} host_frontend_t;

static xiaotai_err_t open_mic(const xiaotai_audio_input_t *input, uint8_t microphones,
                             xiaotai_processor_info_t *info, void **opaque)
{
    uint8_t slots[3];
    if (!input || !info || !opaque || !media_frontend_pairing_slots(&input->desc, microphones, slots))
        return XIAOTAI_ERR_INVALID_ARG;
    host_frontend_t *state = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*state));
    if (!state) return XIAOTAI_ERR_NO_MEM;
    state->channels = input->desc.channels;
    state->microphone = slots[0];
    *info = (xiaotai_processor_info_t){.rate_hz = CAPTURE_RATE_HZ, .feed_frames = CAPTURE_SAMPLES,
                                       .fetch_max = CAPTURE_SAMPLES, .feed_stack_bytes = 512,
                                       .fetch_stack_bytes = 512};
    *opaque = state;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *opaque, const void *input, size_t bytes, int64_t captured_us, uint32_t lost)
{
    host_frontend_t *state = opaque;
    if (!input || bytes != sizeof(state->mono) * state->channels) return XIAOTAI_ERR_INVALID_SIZE;
    if (state->ready) return XIAOTAI_ERR_NO_MEM;
    const int16_t *pcm = input;
    for (size_t i = 0; i < CAPTURE_SAMPLES; ++i) state->mono[i] = pcm[i * state->channels + state->microphone];
    state->captured_us = captured_us;
    state->lost = lost;
    state->ready = true;
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *opaque, xiaotai_processor_frame_t *frame, uint32_t timeout_ms)
{
    (void)timeout_ms;
    host_frontend_t *state = opaque;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (!state->ready) return XIAOTAI_ERR_TIMEOUT;
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = state->mono,
        .size = sizeof(state->mono), .samples = CAPTURE_SAMPLES, .captured_us = state->captured_us,
        .lost_samples = state->lost};
    state->ready = false;
    return XIAOTAI_OK;
}

#if XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL
static int match_dual(const xiaotai_audio_desc_t *input, const xiaotai_audio_desc_t *output)
{
    (void)output;
    return media_frontend_pairing_slots(input, 2, NULL) ? 3 : 0;
}

static xiaotai_err_t open_dual(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                              xiaotai_processor_info_t *info, void **state)
{
    (void)output;
    return open_mic(input, 2, info, state);
}

static const xiaotai_audio_processor_t dual = {.name = "host.aec.dual", .match = match_dual, .open = open_dual,
    .feed = feed, .fetch = fetch, .close = xiaotai_mem_free};
#endif

#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND || XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE
static int match_single(const xiaotai_audio_desc_t *input, const xiaotai_audio_desc_t *output)
{
    (void)output;
    return media_frontend_pairing_slots(input, 1, NULL) ? 2 : 0;
}

static xiaotai_err_t open_single(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                xiaotai_processor_info_t *info, void **state)
{
    (void)output;
    return open_mic(input, 1, info, state);
}
#endif

#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND
static const xiaotai_audio_processor_t single_frontend = {.name = "host.aec.single.frontend",
    .match = match_single, .open = open_single, .feed = feed, .fetch = fetch, .close = xiaotai_mem_free};
#endif

#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE
static const xiaotai_audio_processor_t single = {.name = "host.aec.single", .match = match_single,
    .open = open_single, .feed = feed, .fetch = fetch, .close = xiaotai_mem_free};
#endif
#endif

xiaotai_err_t __wrap_media_platform_register(void)
{
    xiaotai_err_t err = __real_media_platform_register();
#if XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL
    if (err == XIAOTAI_OK) err = xiaotai_media_processor_register(&dual);
#endif
#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND
    if (err == XIAOTAI_OK) err = xiaotai_media_processor_register(&single_frontend);
#endif
#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE
    if (err == XIAOTAI_OK) err = xiaotai_media_processor_register(&single);
#endif
    return err;
}
