// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define WAIT_MS 8000U
#define BLOCK_FRAMES 160U
#define FLUSH_FRAMES 177U
#define OPUS_FRAMES 320U

static atomic_uint s_network_ring_allocations;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes)
{
    if (kind == XIAOTAI_MEM_BULK && bytes == XIAOTAI_DOWNLINK_BYTES(60U, 160U)) {
        atomic_fetch_add(&s_network_ring_allocations, 1);
    }
    return __real_xiaotai_mem_alloc(kind, bytes);
}

extern const xiaotai_sound_pack_t xiaotai_sound_pack_guide_zh_alaw8k;

/* Sixteen 20 ms silence packets, each with the SDK sound-container length. */
#define OPUS_SILENCE 3, 0, 0xf8, 0xff, 0xfe
static const uint8_t s_opus_silence[] = {
    OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE,
    OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE,
    OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE,
    OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE, OPUS_SILENCE,
};
#undef OPUS_SILENCE
static const xiaotai_sound_t s_opus_prompt = {
    .name = XIAOTAI_SOUND_GUIDE_WIFI_SETUP, .encoding = XIAOTAI_CODEC_OPUS,
    .container = XIAOTAI_SOUND_PACKETS, .rate_hz = 16000, .channels = 1, .packet_ms = 20,
    .data = s_opus_silence, .size = sizeof(s_opus_silence),
};
static const xiaotai_sound_pack_t s_opus_fixture = {"opus-fixture", &s_opus_prompt, 1};

#ifdef XIAOTAI_TEST_WITH_LIBOPUS
/* Test-only bindings to the small stable C ABI in an installed libopus. No
 * libopus headers or library are needed to build the SDK or the fallback. */
typedef struct OpusDecoder OpusDecoder;
extern OpusDecoder *opus_decoder_create(int32_t rate, int channels, int *error);
extern void opus_decoder_destroy(OpusDecoder *decoder);
extern int opus_packet_get_nb_samples(const unsigned char *data, int32_t bytes, int32_t rate);
extern int opus_decode(OpusDecoder *decoder, const unsigned char *data, int32_t bytes,
                       int16_t *pcm, int capacity, int decode_fec);
#else
static int16_t fixture_sample(const uint8_t *packet, size_t size, size_t index)
{
    return (int16_t)((int)packet[index % size] * 16 - 1792 + (int)(index % 19U));
}
#endif

static int decoder_match(const xiaotai_media_format_t *format)
{
    return format->encoding == XIAOTAI_CODEC_OPUS && format->rate_hz == 16000 &&
           format->channels == 1 && format->packet_ms == 20;
}

static xiaotai_err_t decoder_open(const xiaotai_media_format_t *format, void **state)
{
    assert(decoder_match(format));
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    int error = 0;
    *state = opus_decoder_create((int32_t)format->rate_hz, 1, &error);
    return *state && error == 0 ? XIAOTAI_OK : XIAOTAI_FAIL;
#else
    static int stateless;
    *state = &stateless;
    return XIAOTAI_OK;
#endif
}

static uint32_t packet_samples(const uint8_t *data, size_t size, uint32_t rate)
{
    if (!data || !size || size > 512 || rate != 16000) return 0;
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    const int samples = opus_packet_get_nb_samples(data, (int32_t)size, (int32_t)rate);
    return samples > 0 ? (uint32_t)samples : 0;
#else
    return OPUS_FRAMES;
#endif
}

static xiaotai_err_t decoder_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm,
                                   size_t capacity, size_t *samples)
{
    assert(state && data && size && size <= 512);
    if (capacity < OPUS_FRAMES) return XIAOTAI_ERR_INVALID_SIZE;
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    const int got = opus_decode(state, data, (int32_t)size, pcm, OPUS_FRAMES, 0);
    if (got <= 0) return XIAOTAI_FAIL;
    *samples = (size_t)got;
#else
    for (size_t i = 0; i < OPUS_FRAMES; ++i) pcm[i] = fixture_sample(data, size, i);
    *samples = OPUS_FRAMES;
#endif
    return XIAOTAI_OK;
}

static void decoder_close(void *state)
{
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    opus_decoder_destroy(state);
#else
    assert(state);
#endif
}

static const xiaotai_audio_decoder_t s_opus = {
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    .name = "host.libopus",
#else
    .name = "host.framed_fake",
#endif
    .encoding = XIAOTAI_CODEC_OPUS, .match = decoder_match, .open = decoder_open,
    .packet_samples = packet_samples, .decode = decoder_decode, .close = decoder_close,
};

typedef enum { PLAY_FULL, PLAY_REJECT_WRITE, PLAY_STOP_IN_WRITE } playback_case_t;

typedef struct {
    const int16_t *samples;
    size_t source_frames, flush_frames;
    playback_case_t playback;
    atomic_size_t committed;
    atomic_uint callbacks, writes;
    atomic_int_fast64_t first_write_us;
    uint32_t done_id;
    xiaotai_err_t done_result;
} trace_t;

static const xiaotai_media_requirement_t s_need = {
    .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
    .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
    .downlink_burst_ms = 1000,
    .preroll_ms = 20000,
};

static xiaotai_audio_output_t s_output;
static xiaotai_task_t s_playout;
static unsigned s_playouts, s_link_audio_tasks;
static bool s_exclusive, s_unpaced_opus;
static atomic_bool s_opened, s_powered;
static trace_t s_trace;
static xiaotai_signal_t s_write_entered, s_write_release, s_done, s_owner_done;

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return s_exclusive; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    const xiaotai_err_t result = __real_xiaotai_task_create(config, task);
    if (result == XIAOTAI_OK && strcmp(config->name, "xt_playout") == 0) {
        s_playout = *task;
        ++s_playouts;
    }
    if (result == XIAOTAI_OK && (strcmp(config->name, "xt_uplink") == 0 ||
                               strcmp(config->name, "xt_capture") == 0 || strcmp(config->name, "xt_clean") == 0)) {
        ++s_link_audio_tasks;
    }
    return result;
}

static int16_t raw_reference(uint8_t value)
{
    value ^= 0x55;
    const unsigned segment = (value >> 4) & 7U;
    const int step = (value & 15) * 16;
    const int amplitude = segment ? (step + 264) * (1 << (segment - 1U)) : step + 8;
    return (int16_t)((value & 0x80) ? amplitude : -amplitude);
}

static int16_t *reference_prompt(const xiaotai_sound_t *asset, size_t *frames)
{
    const size_t input_frames = s_unpaced_opus ? 16U * OPUS_FRAMES : 40800U;
    assert(asset && asset->channels == 1 && asset->rate_hz == s_output.desc.rate_hz);
    int16_t *pcm = malloc(input_frames * sizeof(*pcm));
    assert(pcm);
    if (!s_unpaced_opus) {
        assert(asset->container == XIAOTAI_SOUND_RAW && asset->encoding == XIAOTAI_CODEC_G711A);
        assert(asset->size == input_frames);
        for (size_t i = 0; i < input_frames; ++i) pcm[i] = raw_reference(asset->data[i]);
    } else {
        assert(asset->container == XIAOTAI_SOUND_PACKETS && asset->encoding == XIAOTAI_CODEC_OPUS);
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
        int error = 0;
        OpusDecoder *reference = opus_decoder_create(16000, 1, &error);
        assert(reference && error == 0);
#endif
        size_t samples = 0;
        for (size_t offset = 0; offset < asset->size;) {
            assert(asset->size - offset >= 2);
            const size_t bytes = asset->data[offset] + (size_t)asset->data[offset + 1] * 256U;
            offset += 2;
            assert(bytes > 0 && bytes <= 512 && bytes <= asset->size - offset);
            assert(samples + OPUS_FRAMES <= input_frames);
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
            assert(opus_decode(reference, asset->data + offset, (int32_t)bytes, pcm + samples, OPUS_FRAMES, 0) ==
                   OPUS_FRAMES);
#else
            for (size_t i = 0; i < OPUS_FRAMES; ++i)
                pcm[samples + i] = fixture_sample(asset->data + offset, bytes, i);
#endif
            offset += bytes;
            samples += OPUS_FRAMES;
        }
        assert(samples == input_frames);
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
        opus_decoder_destroy(reference);
#endif
    }
    *frames = input_frames;
    return pcm;
}

static xiaotai_err_t output_open(void *self)
{
    assert(self == &s_output && !atomic_exchange(&s_opened, true));
    return XIAOTAI_OK;
}

static xiaotai_err_t output_enable(void *self, bool enabled)
{
    assert(self == &s_output && atomic_load(&s_opened) && !xiaotai_on_owner());
    atomic_store(&s_powered, enabled);
    return XIAOTAI_OK;
}

static xiaotai_err_t output_write(void *self, void *data, size_t bytes)
{
    assert(self == &s_output && atomic_load(&s_opened) && (s_unpaced_opus || atomic_load(&s_powered)));
    assert(!xiaotai_on_owner() && xiaotai_task_is_current(&s_playout));
    assert(bytes > 0 && bytes % sizeof(int16_t) == 0);
    const size_t frames = bytes / sizeof(int16_t);
    const size_t offset = atomic_load(&s_trace.committed);
    const size_t total = s_trace.source_frames + s_trace.flush_frames;
    const int16_t *samples = data;
    assert(frames <= BLOCK_FRAMES && offset + frames <= total);
    assert(!atomic_load(&s_trace.callbacks));
    for (size_t i = 0; i < frames; ++i) {
        const size_t index = offset + i;
        const int16_t expected = index < s_trace.source_frames ? s_trace.samples[index] : 0;
        assert(samples[i] == expected);
    }
    if (atomic_fetch_add(&s_trace.writes, 1) == 0) atomic_store(&s_trace.first_write_us, xiaotai_time_us());
    if (s_trace.playback == PLAY_REJECT_WRITE) return XIAOTAI_ERR_TIMEOUT;
    if ((s_trace.playback == PLAY_FULL && offset + frames == total) ||
        (s_trace.playback == PLAY_STOP_IN_WRITE && offset == 0)) {
        xiaotai_signal_give(&s_write_entered);
        assert(xiaotai_signal_take(&s_write_release, WAIT_MS));
    }
    atomic_store(&s_trace.committed, offset + frames);
    return XIAOTAI_OK;
}

static size_t output_flush(void *self)
{
    assert(self == &s_output);
    return FLUSH_FRAMES;
}

static xiaotai_err_t output_close(void *self)
{
    assert(self == &s_output && atomic_exchange(&s_opened, false));
    atomic_store(&s_powered, false);
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    (void)index;
    return NULL;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    return index == 0 ? &s_output : NULL;
}

static xiaotai_err_t fixture_init(void)
{
    const xiaotai_sound_pack_t *pack = s_unpaced_opus ? &s_opus_fixture : &xiaotai_sound_pack_guide_zh_alaw8k;
    xiaotai_err_t err = xiaotai_sounds_register(pack);
    if (err == XIAOTAI_OK && s_unpaced_opus) err = xiaotai_media_decoder_register(&s_opus);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&s_need);
    return err;
}

static xiaotai_media_status_t status(void)
{
    xiaotai_media_status_t snapshot;
    xiaotai_media_status(&snapshot);
    return snapshot;
}

static void check_exclusive_open(void *ctx)
{
    xiaotai_err_t *result = ctx;
    const xiaotai_media_session_t session = {
        .generation = 1, .downlink = s_need.downlink, .downlink_burst_ms = s_need.downlink_burst_ms,
    };
    assert(xiaotai_on_owner());
    *result = xiaotai_media_open(&session);
    const xiaotai_media_preroll_t preroll = {.wake_us = xiaotai_time_us(), .ttl_ms = 1000};
    uint32_t token = 0;
    assert(xiaotai_media_preroll_hold(&preroll, &token) == XIAOTAI_ERR_NOT_SUPPORTED && token == 0);
    xiaotai_signal_give(&s_owner_done);
}

static void boot(bool exclusive, bool unpaced_opus)
{
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t engine = {
        .name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start,
    };
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = deps, .init = fixture_init};
    static const xiaotai_module_t *const modules[] = {&engine, &fixture};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 2, .queue_depth = 8};
    s_exclusive = exclusive;
    s_unpaced_opus = unpaced_opus;
    s_playouts = s_link_audio_tasks = 0;
    atomic_store(&s_network_ring_allocations, 0);
    s_output = (xiaotai_audio_output_t){
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = unpaced_opus ? 16000U : 8000U, .channels = 1},
        .info = {.max_write_frames = BLOCK_FRAMES, .flags = unpaced_opus ? 0 : XIAOTAI_AUDIO_OUTPUT_PACED},
        .open = output_open, .write = output_write, .set_enabled = unpaced_opus ? NULL : output_enable,
        .flush_frames = unpaced_opus ? NULL : output_flush, .close = output_close, .self = &s_output,
    };
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    const int64_t until = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
    while (!status().ready && xiaotai_time_us() < until) xiaotai_delay_ms(1);
    assert(status().ready && atomic_load(&s_opened) && !atomic_load(&s_powered) && s_playouts == 1);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.playback && info.playback_rate_hz == s_output.desc.rate_hz);
    if (exclusive) {
        assert(info.downlink_ms == 0 && info.preroll_ms == 0 && s_link_audio_tasks == 0);
        assert(atomic_load(&s_network_ring_allocations) == 0);
        assert(!xiaotai_media_downlink() && !xiaotai_media_can_receive(&s_need.downlink));
        assert(!xiaotai_media_can_send(&s_need.uplink));
        xiaotai_err_t result = XIAOTAI_OK;
        assert(xiaotai_run(check_exclusive_open, &result) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&s_owner_done, WAIT_MS) && result == XIAOTAI_ERR_INVALID_STATE);
    } else {
        assert(info.downlink_ms == 1200 && xiaotai_media_can_receive(&s_need.downlink));
        assert(atomic_load(&s_network_ring_allocations) == 1);
        assert(xiaotai_downlink_capacity(xiaotai_media_downlink()) == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    }
}

static void play_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    assert(ctx == &s_trace && xiaotai_on_owner() && id != 0);
    const size_t committed = atomic_load(&s_trace.committed);
    assert(atomic_load(&s_trace.callbacks) == 0);
    if (s_trace.playback == PLAY_FULL) {
        assert(result == XIAOTAI_OK && committed == s_trace.source_frames + s_trace.flush_frames);
        if (s_unpaced_opus) {
            /* The sink has no pacing or flush callback. */
            const int64_t elapsed_us = xiaotai_time_us() - atomic_load(&s_trace.first_write_us);
            const int64_t duration_us = ((int64_t)committed * 1000000) / s_output.desc.rate_hz;
            assert(elapsed_us >= duration_us - 40000);
        }
    } else if (s_trace.playback == PLAY_REJECT_WRITE) {
        assert(result == XIAOTAI_FAIL && committed == 0);
    } else {
        assert(result == XIAOTAI_ERR_INVALID_STATE && committed > 0 && committed <= BLOCK_FRAMES);
    }
    s_trace.done_id = id;
    s_trace.done_result = result;
    atomic_store(&s_trace.callbacks, 1);
    xiaotai_signal_give(&s_done);
}

static void play_prompt(playback_case_t playback)
{
    const xiaotai_sound_t *asset = xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_WIFI_SETUP);
    size_t frames;
    int16_t *reference = reference_prompt(asset, &frames);
    s_trace.samples = reference;
    s_trace.source_frames = frames;
    s_trace.flush_frames = s_output.flush_frames ? FLUSH_FRAMES : 0;
    s_trace.playback = playback;
    s_trace.done_id = 0;
    s_trace.done_result = XIAOTAI_OK;
    atomic_store(&s_trace.committed, 0);
    atomic_store(&s_trace.writes, 0);
    atomic_store(&s_trace.callbacks, 0);
    atomic_store(&s_trace.first_write_us, 0);
    const xiaotai_media_play_t request = {
        .clips = {{.name = XIAOTAI_SOUND_GUIDE_WIFI_SETUP}}, .clip_count = 1,
        .priority = XIAOTAI_PLAY_NORMAL, .done = play_done, .ctx = &s_trace,
    };
    uint32_t id = 0;
    assert(xiaotai_media_play_clips(&request, &id) == XIAOTAI_OK && id != 0);
    if (playback != PLAY_REJECT_WRITE) {
        assert(xiaotai_signal_take(&s_write_entered, WAIT_MS));
        assert(atomic_load(&s_trace.callbacks) == 0);
        if (playback == PLAY_FULL) {
            /* The last write is in flight: paced flush or unpaced payload. */
            const size_t committed = atomic_load(&s_trace.committed);
            if (s_trace.flush_frames) assert(committed > frames && committed < frames + s_trace.flush_frames);
            else assert(committed == frames - BLOCK_FRAMES);
        } else {
            assert(atomic_load(&s_trace.committed) == 0);
            xiaotai_media_stop(id);
            assert(atomic_load(&s_trace.callbacks) == 0);
        }
        xiaotai_signal_give(&s_write_release);
    }
    assert(xiaotai_signal_take(&s_done, WAIT_MS));
    const xiaotai_err_t expected = playback == PLAY_FULL ? XIAOTAI_OK : playback == PLAY_REJECT_WRITE ?
                                                     XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_STATE;
    assert(atomic_load(&s_trace.callbacks) == 1 && s_trace.done_id == id && s_trace.done_result == expected);
    assert(atomic_load(&s_trace.writes) > 0);
    if (playback != PLAY_FULL) assert(atomic_load(&s_trace.writes) == 1);
    free(reference);
}

static void reset(void)
{
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_sounds_reset();
    assert(!atomic_load(&s_opened) && !atomic_load(&s_powered));
}

int main(void)
{
    xiaotai_signal_t *const barriers[] = {&s_write_entered, &s_write_release, &s_done, &s_owner_done};
    for (size_t i = 0; i < sizeof(barriers) / sizeof(*barriers); ++i)
        assert(xiaotai_signal_init(barriers[i]) == XIAOTAI_OK);
    for (unsigned opus = 0; opus < 2; ++opus) {
        boot(false, opus != 0);
        play_prompt(PLAY_FULL);
        reset();
        boot(true, opus != 0);
        if (opus == 0) {
            play_prompt(PLAY_REJECT_WRITE);
            play_prompt(PLAY_STOP_IN_WRITE);
        }
        play_prompt(PLAY_FULL);
        reset();
    }
    for (size_t i = 0; i < sizeof(barriers) / sizeof(*barriers); ++i) xiaotai_signal_deinit(barriers[i]);
#ifdef XIAOTAI_TEST_WITH_LIBOPUS
    puts("Bundled A-law and synthetic libopus prompts reached the fake HAL; no physical audio tested.");
#else
    puts("Bundled A-law and synthetic framed prompts reached the fake HAL; no host libopus.");
#endif
    return 0;
}
