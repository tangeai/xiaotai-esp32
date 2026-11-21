// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_binding_prompt_cancel.py
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_os_host.h"

enum {
    RATE = 8000, ALT_RATE = 16000, UNDECLARED_RATE = 24000, NO_MATCH_RATE = 12000,
    BLOCK = 160, QUEUE = 1600, EXTRA = 80, FLUSH = 17, WAIT_MS = 3000,
};
static xiaotai_signal_t first, first_release, first_data, data_release, completion, writer_entered;
static atomic_uint blocks, calls, consumed;
static atomic_bool hold_data, writer_done;
static int16_t pcm[4096], payload[QUEUE], extra[EXTRA];
static uint32_t completed_id;
static xiaotai_err_t completed_result;
static struct { uint32_t rate; unsigned opens, calls, resets, closes; } decoders[3] = {{.rate = RATE},
    {.rate = ALT_RATE}, {.rate = UNDECLARED_RATE}};

static xiaotai_err_t out_open(void *self) { (void)self; assert(!xiaotai_on_owner()); return XIAOTAI_OK; }
static xiaotai_err_t out_close(void *self) { (void)self; assert(!xiaotai_on_owner()); return XIAOTAI_OK; }
static size_t out_flush(void *self) { (void)self; return FLUSH; }

static xiaotai_err_t out_write(void *self, void *data, size_t bytes)
{
    (void)self;
    assert(!xiaotai_on_owner() && bytes && bytes % sizeof(int16_t) == 0 && bytes <= BLOCK * sizeof(int16_t));
    const size_t count = bytes / sizeof(int16_t);
    const unsigned offset = atomic_load(&consumed);
    assert(offset + count <= sizeof(pcm) / sizeof(pcm[0]));
    memcpy(pcm + offset, data, bytes);
    atomic_store(&consumed, offset + (unsigned)count);
    if (atomic_fetch_add(&blocks, 1) == 0) {
        xiaotai_signal_give(&first);
        assert(xiaotai_signal_take(&first_release, WAIT_MS));
    } else if (atomic_exchange(&hold_data, false)) {
        xiaotai_signal_give(&first_data);
        assert(xiaotai_signal_take(&data_release, WAIT_MS));
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index) { (void)index; return NULL; }

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t output = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1},
        .info = {.max_write_frames = BLOCK, .flags = XIAOTAI_AUDIO_OUTPUT_PACED},
        .open = out_open, .write = out_write, .flush_frames = out_flush, .close = out_close,
    };
    return index == 0 ? &output : NULL;
}

static void done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == pcm);
    completed_id = id;
    completed_result = result;
    assert(atomic_fetch_add(&calls, 1) == 0);
    xiaotai_signal_give(&completion);
}

static int match(const xiaotai_media_format_t *format)
{
    return format->encoding == XIAOTAI_CODEC_OPUS && format->channels == 1 && format->packet_ms == 20 &&
           (format->rate_hz == RATE || format->rate_hz == ALT_RATE || format->rate_hz == UNDECLARED_RATE);
}

static xiaotai_err_t decoder_open(const xiaotai_media_format_t *format, void **state)
{
    assert(xiaotai_on_owner() && match(format));
    for (unsigned i = 0; i < sizeof(decoders) / sizeof(decoders[0]); ++i) {
        if (decoders[i].rate != format->rate_hz) continue;
        assert(!decoders[i].opens++);
        *state = &decoders[i];
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static uint32_t packet_samples(const uint8_t *data, size_t bytes, uint32_t rate)
{
    if (!data || bytes != 2U || (rate != RATE && rate != ALT_RATE && rate != UNDECLARED_RATE)) return 0;
    return data[0] == 255U ? rate * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U + 1U : data[0] * (rate / RATE);
}

static unsigned decoder_index(void *state)
{
    for (unsigned i = 0; i < sizeof(decoders) / sizeof(decoders[0]); ++i) {
        if (state == &decoders[i]) return i;
    }
    assert(false);
    return 0;
}

static xiaotai_err_t decode(void *state, const uint8_t *data, size_t size, int16_t *out, size_t cap, size_t *samples)
{
    const unsigned i = decoder_index(state);
    const uint32_t count = packet_samples(data, size, decoders[i].rate);
    assert(!xiaotai_on_owner() && count && count <= cap);
    ++decoders[i].calls;
    for (uint32_t sample = 0; sample < count; ++sample) out[sample] = (int16_t)(data[1] * 9);
    *samples = count;
    return XIAOTAI_OK;
}

static xiaotai_err_t decoder_reset(void *state) { ++decoders[decoder_index(state)].resets; return XIAOTAI_OK; }
static void decoder_close(void *state) { ++decoders[decoder_index(state)].closes; }

static xiaotai_err_t init(void)
{
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_NONE, RATE) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, RATE - 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, 48001) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, RATE) == XIAOTAI_OK);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, ALT_RATE) == XIAOTAI_OK);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, NO_MATCH_RATE) == XIAOTAI_OK);
    for (uint32_t extra_rate = RATE; extra_rate < RATE + 5; ++extra_rate)
        assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_G711U, extra_rate) == XIAOTAI_OK);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, ALT_RATE) == XIAOTAI_OK);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_G711U, RATE + 5) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_PCM16, ALT_RATE) == XIAOTAI_OK);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_G711A, ALT_RATE) == XIAOTAI_OK);
    assert(!decoders[0].opens && !decoders[1].opens);

    static const xiaotai_audio_decoder_t codec = {.name = "stream.fixture", .encoding = XIAOTAI_CODEC_OPUS,
        .stack_bytes = 512, .match = match, .open = decoder_open, .packet_samples = packet_samples,
        .decode = decode, .reset = decoder_reset, .close = decoder_close};
    static const uint8_t packetized[] = {2, 0, 160, 67};
    static const xiaotai_sound_t native_clip = {.name = "test.native", .encoding = XIAOTAI_CODEC_OPUS,
        .container = XIAOTAI_SOUND_PACKETS, .rate_hz = RATE, .channels = 1, .packet_ms = 20,
        .data = packetized, .size = sizeof(packetized)};
    static const xiaotai_sound_pack_t pack = {.name = "stream.fixture", .sounds = &native_clip, .count = 1};
    assert(xiaotai_media_decoder_register(&codec) == XIAOTAI_OK);
    assert(xiaotai_sounds_register(&pack) == XIAOTAI_OK);
    return xiaotai_engine_media_init();
}

static void reset_output(bool hold_second)
{
    atomic_store(&blocks, 0);
    atomic_store(&calls, 0);
    atomic_store(&consumed, 0);
    atomic_store(&hold_data, hold_second);
    memset(pcm, 0x66, sizeof(pcm));
}

static uint32_t start(xiaotai_codec_t encoding, uint32_t rate_hz, bool hold_second)
{
    reset_output(hold_second);
    const xiaotai_media_stream_t request = {.encoding = encoding, .rate_hz = rate_hz,
                                            .priority = XIAOTAI_PLAY_NORMAL, .done = done, .ctx = pcm};
    uint32_t id = 0;
    assert(xiaotai_media_play_stream(&request, &id) == XIAOTAI_OK && id);
    assert(xiaotai_signal_take(&first, WAIT_MS) && atomic_load(&consumed) == BLOCK);
    for (unsigned i = 0; i < BLOCK; ++i) assert(!pcm[i]);
    assert(!atomic_load(&calls));
    return id;
}

static void wait_done(uint32_t id, xiaotai_err_t result)
{
    assert(xiaotai_signal_take(&completion, WAIT_MS));
    assert(completed_id == id && completed_result == result && atomic_load(&calls) == 1);
    assert(xiaotai_media_play_finish(id) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_media_play_write(id, payload, sizeof(int16_t), 0) == XIAOTAI_ERR_INVALID_STATE);
}

static void *writer(void *ctx)
{
    const uint32_t id = *(uint32_t *)ctx;
    xiaotai_signal_give(&writer_entered);
    assert(xiaotai_media_play_write(id, extra, sizeof(extra), 2000) == XIAOTAI_OK);
    assert(xiaotai_media_play_finish(id) == XIAOTAI_OK);
    atomic_store(&writer_done, true);
    return NULL;
}

static void raw(void)
{
    uint32_t id = start(XIAOTAI_CODEC_PCM16, RATE, true);
    assert(xiaotai_media_play_write(id, NULL, sizeof(int16_t), 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_play_write(id, payload, 1, 0) == XIAOTAI_ERR_INVALID_SIZE);
    for (unsigned i = 0; i < QUEUE; ++i) payload[i] = (int16_t)(i + 1);
    for (unsigned i = 0; i < EXTRA; ++i) extra[i] = (int16_t)(3000 + i);
    assert(xiaotai_media_play_write(id, payload, sizeof(payload), 0) == XIAOTAI_OK);
    assert(xiaotai_media_play_write(id, extra, sizeof(extra), 0) == XIAOTAI_ERR_TIMEOUT);

    pthread_t task;
    assert(pthread_create(&task, NULL, writer, &id) == 0 && xiaotai_signal_take(&writer_entered, WAIT_MS));
    assert(!atomic_load(&writer_done) && atomic_load(&consumed) == BLOCK);
    xiaotai_signal_give(&first_release);
    assert(xiaotai_signal_take(&first_data, WAIT_MS));
    assert(pthread_join(task, NULL) == 0 && atomic_load(&writer_done));
    assert(xiaotai_media_play_write(id, extra, sizeof(extra), 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&data_release);
    wait_done(id, XIAOTAI_OK);
    assert(atomic_load(&consumed) == BLOCK + QUEUE + EXTRA + FLUSH);
    assert(memcmp(pcm + BLOCK, payload, sizeof(payload)) == 0);
    assert(memcmp(pcm + BLOCK + QUEUE, extra, sizeof(extra)) == 0);
    for (size_t i = BLOCK + QUEUE + EXTRA; i < BLOCK + QUEUE + EXTRA + FLUSH; ++i) assert(!pcm[i]);
}

static void framed(void)
{
    uint32_t id = start(XIAOTAI_CODEC_OPUS, RATE, false);
    assert(decoders[0].opens == 1 && decoders[0].resets == 1);
    const uint8_t wrong[] = {0, 1, 2};
    assert(xiaotai_media_play_write(id, wrong, sizeof(wrong), 0) == XIAOTAI_ERR_INVALID_SIZE);
    const uint8_t too_long[] = {255, 1};
    assert(xiaotai_media_play_write(id, too_long, sizeof(too_long), 0) == XIAOTAI_ERR_INVALID_SIZE);
    for (uint8_t i = 0; i < 10U; ++i) {
        const uint8_t packet[] = {160, (uint8_t)(i + 1U)};
        assert(xiaotai_media_play_write(id, packet, sizeof(packet), 0) == XIAOTAI_OK);
    }
    const uint8_t would_exceed[] = {160, 250};
    /* Even though the compressed byte ring has lots of space, 200 ms is full. */
    assert(xiaotai_media_play_write(id, would_exceed, sizeof(would_exceed), 0) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_media_play_finish(id) == XIAOTAI_OK);
    xiaotai_signal_give(&first_release);
    wait_done(id, XIAOTAI_OK);
    assert(decoders[0].opens == 1 && decoders[0].calls == 10U && atomic_load(&consumed) == BLOCK + QUEUE + FLUSH);
    for (unsigned i = 0; i < QUEUE; ++i) assert(pcm[BLOCK + i] == (int16_t)(1 + i / BLOCK) * 9);
}

static void alternate_rate(void)
{
    const uint32_t id = start(XIAOTAI_CODEC_OPUS, ALT_RATE, false);
    const uint8_t packet[] = {160, 67};
    assert(decoders[1].opens == 1 && decoders[1].resets == 1);
    assert(xiaotai_media_play_write(id, packet, sizeof(packet), 0) == XIAOTAI_OK);
    assert(xiaotai_media_play_finish(id) == XIAOTAI_OK);
    xiaotai_signal_give(&first_release);
    wait_done(id, XIAOTAI_OK);
    assert(decoders[1].opens == 1 && decoders[1].calls == 1);
    const unsigned frames = atomic_load(&consumed);
    /* There are 160 nominal source-time samples plus the causal filter tail.
     * The HAL flush follows both, and the steady signal keeps its DC level. */
    int16_t decoded[2 * BLOCK + 40 + FLUSH];
    assert(frames > 2 * BLOCK + FLUSH && frames <= sizeof(decoded) / sizeof(decoded[0]));
    for (unsigned i = BLOCK + 50; i < 2 * BLOCK - 50; ++i) assert(pcm[i] >= 602 && pcm[i] <= 604);
    for (unsigned i = frames - FLUSH; i < frames; ++i) assert(!pcm[i]);
    memcpy(decoded, pcm, frames * sizeof(*pcm));

    const uint32_t raw_id = start(XIAOTAI_CODEC_PCM16, ALT_RATE, false);
    int16_t samples[2 * BLOCK];
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) samples[i] = 603;
    assert(xiaotai_media_play_write(raw_id, samples, sizeof(samples), 0) == XIAOTAI_OK);
    assert(xiaotai_media_play_finish(raw_id) == XIAOTAI_OK);
    xiaotai_signal_give(&first_release);
    wait_done(raw_id, XIAOTAI_OK);
    assert(atomic_load(&consumed) == frames && memcmp(decoded, pcm, frames * sizeof(*pcm)) == 0);
}

static void *cancelled_writer(void *ctx)
{
    const uint32_t id = *(uint32_t *)ctx;
    xiaotai_signal_give(&writer_entered);
    assert(xiaotai_media_play_write(id, extra, sizeof(extra), 2000) == XIAOTAI_ERR_INVALID_STATE);
    return NULL;
}

static void stopped(void)
{
    uint32_t id = start(XIAOTAI_CODEC_PCM16, RATE, false);
    assert(xiaotai_media_play_write(id, payload, sizeof(payload), 0) == XIAOTAI_OK);
    pthread_t task;
    assert(pthread_create(&task, NULL, cancelled_writer, &id) == 0 && xiaotai_signal_take(&writer_entered, WAIT_MS));
    xiaotai_media_stop(id);
    assert(!atomic_load(&calls) && atomic_load(&consumed) == BLOCK);
    xiaotai_signal_give(&first_release);
    assert(pthread_join(task, NULL) == 0);
    wait_done(id, XIAOTAI_ERR_INVALID_STATE);
    assert(atomic_load(&consumed) == BLOCK); /* neither queued audio nor a stop flush */
}

int main(void)
{
    xiaotai_signal_t *const signals[] = {&first, &first_release, &first_data, &data_release, &completion, &writer_entered};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t module = {.name = "wave5.media", .init = init, .start = xiaotai_engine_media_start};
    static const xiaotai_module_t *const modules[] = {&log, &module};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 2};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    const int64_t until = xiaotai_time_us() + WAIT_MS * 1000;
    for (;;) {
        xiaotai_media_status_t status;
        xiaotai_media_status(&status);
        if (status.ready) break;
        assert(xiaotai_time_us() < until);
        xiaotai_delay_ms(1);
    }
    const int64_t baseline = xiaotai_mem_host_outstanding();
    assert(decoders[0].opens == 1 && decoders[1].opens == 1 && !decoders[2].opens);
    assert(xiaotai_media_require_stream_format(XIAOTAI_CODEC_OPUS, UNDECLARED_RATE) == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_media_stream_t unsupported[] = {
        {.encoding = XIAOTAI_CODEC_G711U, .rate_hz = RATE},
        {.encoding = XIAOTAI_CODEC_OPUS, .rate_hz = NO_MATCH_RATE},
        {.encoding = XIAOTAI_CODEC_OPUS, .rate_hz = UNDECLARED_RATE},
    };
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        uint32_t unsupported_id = 0;
        assert(xiaotai_media_play_stream(&unsupported[i], &unsupported_id) == XIAOTAI_ERR_NOT_SUPPORTED);
        assert(!unsupported_id && xiaotai_mem_host_outstanding() == baseline);
    }
    raw();
    assert(xiaotai_mem_host_outstanding() == baseline);
    framed();
    assert(xiaotai_mem_host_outstanding() == baseline);
    alternate_rate();
    assert(xiaotai_mem_host_outstanding() == baseline);
    stopped();
    assert(xiaotai_mem_host_outstanding() == baseline);
    const uint32_t idle = start(XIAOTAI_CODEC_G711A, RATE, false);
    xiaotai_os_host_advance_us((int64_t)XIAOTAI_MEDIA_STREAM_IDLE_MS * 1000 + 1000);
    xiaotai_signal_give(&first_release);
    wait_done(idle, XIAOTAI_ERR_TIMEOUT);
    assert(atomic_load(&consumed) == BLOCK && xiaotai_mem_host_outstanding() == baseline);

    xiaotai_media_reset();
    assert(decoders[0].closes == 1 && decoders[1].closes == 1 && !decoders[2].closes);
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_sounds_reset();
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) xiaotai_signal_deinit(signals[i]);
    return 0;
}
