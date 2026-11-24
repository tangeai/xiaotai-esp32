// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

enum { RATE = 8000, BLOCK = 80, GENERATION = 42, WAIT_MS = 3000 };
static xiaotai_signal_t owner, write_began, write_release, completed;
static atomic_bool hold_first;
static atomic_uint written, callbacks;
static int16_t wire[4096];
static xiaotai_err_t result;
static uint32_t mixed_id;
static bool enabled = true;

static xiaotai_err_t open_part(void *self) { (void)self; assert(!xiaotai_on_owner()); return XIAOTAI_OK; }
static xiaotai_err_t close_part(void *self) { (void)self; assert(!xiaotai_on_owner()); return XIAOTAI_OK; }
static size_t flush_part(void *self) { (void)self; return 19; }

static xiaotai_err_t write_part(void *self, void *data, size_t size)
{
    (void)self;
    assert(!xiaotai_on_owner() && size && size % sizeof(int16_t) == 0 && size <= BLOCK * sizeof(int16_t));
    const unsigned before = atomic_load(&written);
    const unsigned frames = (unsigned)(size / sizeof(int16_t));
    assert(before + frames <= sizeof(wire) / sizeof(wire[0]));
    memcpy(wire + before, data, size);
    atomic_store(&written, before + frames);
    if (atomic_exchange(&hold_first, false)) {
        xiaotai_signal_give(&write_began);
        assert(xiaotai_signal_take(&write_release, WAIT_MS));
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index) { (void)index; return NULL; }

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t out = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1},
        .info = {.max_write_frames = BLOCK, .flags = XIAOTAI_AUDIO_OUTPUT_PACED},
        .open = open_part, .write = write_part, .close = close_part, .flush_frames = flush_part,
    };
    return index == 0 ? &out : NULL;
}

static void on_done(uint32_t id, xiaotai_err_t status, void *ctx)
{
    assert(ctx == wire && xiaotai_on_owner());
    mixed_id = id;
    result = status;
    assert(atomic_fetch_add(&callbacks, 1) == 0);
    xiaotai_signal_give(&completed);
}

static xiaotai_err_t init(void)
{
    static const xiaotai_media_param_t on = {.name = "playout.mixer", .value = 1};
    static const xiaotai_media_param_t off = {.name = "playout.mixer", .value = 0};
    static const xiaotai_media_requirement_t req = {
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 1000, .latency = XIAOTAI_MEDIA_LATENCY_LOW,
    };
    assert(xiaotai_media_tune(enabled ? &on : &off, 1) == XIAOTAI_OK);
    assert(xiaotai_media_require(&req) == XIAOTAI_OK);
    return xiaotai_engine_media_init();
}

static void open_session(void *ctx)
{
    (void)ctx;
    const xiaotai_media_session_t session = {
        .generation = GENERATION, .flags = XIAOTAI_MEDIA_HOLD, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .latency = XIAOTAI_MEDIA_LATENCY_LOW,
    };
    assert(xiaotai_media_open(&session) == XIAOTAI_OK);
    xiaotai_signal_give(&owner);
}

static void activate(void *ctx) { (void)ctx; assert(xiaotai_media_activate(GENERATION) == XIAOTAI_OK); xiaotai_signal_give(&owner); }
static void close_session(void *ctx) { (void)ctx; xiaotai_media_close(GENERATION); xiaotai_signal_give(&owner); }

static void boot(void)
{
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t module = {.name = "mix.fixture", .init = init, .start = xiaotai_engine_media_start};
    static const xiaotai_module_t *const modules[] = {&log, &module};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 2};
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    const int64_t until = xiaotai_time_us() + WAIT_MS * 1000;
    for (;;) {
        xiaotai_media_status_t status;
        xiaotai_media_status(&status);
        if (status.ready) break;
        assert(xiaotai_time_us() < until);
        xiaotai_delay_ms(1);
    }
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.mixer == enabled);
}

static void finish(void)
{
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
}

int main(void)
{
    assert(xiaotai_signal_init(&owner) == XIAOTAI_OK && xiaotai_signal_init(&write_began) == XIAOTAI_OK &&
           xiaotai_signal_init(&write_release) == XIAOTAI_OK && xiaotai_signal_init(&completed) == XIAOTAI_OK);
    uint8_t overlay[640];
    for (size_t i = 0; i < sizeof(overlay) / 2U; ++i) {
        const uint16_t sample = i < 160 ? 16000U : (uint16_t)-16000;
        overlay[2U * i] = (uint8_t)sample;
        overlay[2U * i + 1U] = (uint8_t)(sample >> 8U);
    }
    const xiaotai_sound_t borrowed = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1,
                                      .data = overlay, .size = sizeof(overlay)};
    xiaotai_media_play_t request = {.clips = {{.sound = &borrowed}}, .clip_count = 1, .done = on_done, .ctx = wire,
                                    .priority = XIAOTAI_PLAY_NORMAL, .flags = XIAOTAI_PLAY_OVER_SESSION};

    enabled = false;
    boot();
    assert(xiaotai_media_play_clips(&request, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(!atomic_load(&written) && !atomic_load(&callbacks));
    finish();

    enabled = true;
    boot();
    assert(xiaotai_run(open_session, NULL) == XIAOTAI_OK && xiaotai_signal_take(&owner, WAIT_MS));
    uint8_t payload[160];
    for (unsigned i = 0; i < 4U; ++i) {
        memset(payload, i == 0 ? 0xaa : 0x2a, sizeof(payload));
        const xiaotai_media_packet_t packet = {.encoding = XIAOTAI_CODEC_G711A, .rate_hz = RATE,
            .timestamp_ms = 20U * i, .stream = 3, .data = payload, .size = sizeof(payload)};
        assert(xiaotai_media_push(GENERATION, &packet));
    }
    atomic_store(&hold_first, true);
    assert(xiaotai_run(activate, NULL) == XIAOTAI_OK && xiaotai_signal_take(&owner, WAIT_MS));
    assert(xiaotai_signal_take(&write_began, WAIT_MS));
    const unsigned unmodified = atomic_load(&written);
    assert(unmodified == BLOCK);
    request.generation = GENERATION;
    request.flags = 0;
    assert(xiaotai_media_play_clips(&request, NULL) == XIAOTAI_ERR_BUSY);
    request.flags = XIAOTAI_PLAY_OVER_SESSION;
    uint32_t id = 0;
    assert(xiaotai_media_play_clips(&request, &id) == XIAOTAI_OK && id);
    assert(!atomic_load(&callbacks) && atomic_load(&written) == unmodified);
    xiaotai_signal_give(&write_release);
    assert(xiaotai_signal_take(&completed, WAIT_MS));
    assert(id == mixed_id && result == XIAOTAI_OK && atomic_load(&callbacks) == 1);
    const unsigned count = atomic_load(&written);
    unsigned high = 0, low = 0;
    for (unsigned i = 0; i < count; ++i) {
        high += wire[i] == INT16_MAX;
        low += wire[i] == INT16_MIN;
        if (i < unmodified) assert(wire[i] != INT16_MAX && wire[i] != INT16_MIN);
    }
    assert(high > 0 && low > 0 && count >= unmodified + 320U + 19U);
    assert(xiaotai_run(close_session, NULL) == XIAOTAI_OK && xiaotai_signal_take(&owner, WAIT_MS));
    finish();
    xiaotai_signal_deinit(&completed);
    xiaotai_signal_deinit(&write_release);
    xiaotai_signal_deinit(&write_began);
    xiaotai_signal_deinit(&owner);
    return 0;
}
