// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "g711.h"
#include "portable_audio.h"
#include "registry.h"
#include "resample.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

static void processor_packets(void)
{
    const xiaotai_audio_input_t part = {.desc = {.encoding = XIAOTAI_CODEC_G711A, .rate_hz = 8000,
        .channels = 1, .frame_ms = 20}};
    assert(media_select_processor(&part.desc, NULL) == &media_passthrough);
    xiaotai_processor_info_t info;
    void *state;
    assert(media_passthrough.open(&part, NULL, &info, &state) == XIAOTAI_OK);
    assert(info.flags == XIAOTAI_PROCESSOR_ENCODED && !info.feed_frames && info.rate_hz == 8000);
    uint8_t packet[160], saved[160];
    for (size_t i = 0; i < sizeof(packet); ++i) packet[i] = (uint8_t)i;
    memcpy(saved, packet, sizeof(saved));
    assert(media_passthrough.feed(state, packet, sizeof(packet), 10000, 2) == XIAOTAI_OK);
    assert(media_passthrough.feed(state, packet, sizeof(packet), 30000, 0) == XIAOTAI_ERR_NO_MEM);
    memset(packet, 0, sizeof(packet));
    xiaotai_processor_frame_t frame;
    assert(media_passthrough.fetch(state, &frame, 0) == XIAOTAI_OK);
    assert(frame.encoding == XIAOTAI_CODEC_G711A && frame.samples == sizeof(saved) && frame.size == sizeof(saved));
    assert(frame.captured_us == 10000 && frame.lost_samples == 320 && !memcmp(frame.data, saved, sizeof(saved)));
    assert(media_passthrough.fetch(state, &frame, 1) == XIAOTAI_ERR_TIMEOUT);
    assert(media_passthrough.feed(state, packet, 159, 50000, 0) == XIAOTAI_ERR_INVALID_SIZE);
    assert(media_passthrough.feed(state, packet, sizeof(packet), 70000, 0) == XIAOTAI_OK);
    assert(media_passthrough.fetch(state, &frame, 0) == XIAOTAI_OK && frame.lost_samples == 320);
    xiaotai_processor_stats_t stats;
    media_passthrough.stats(state, &stats);
    assert(stats.frames == 2 && stats.feeds == 2 && stats.errors == 1 && stats.input_full == 1 && stats.lost_samples == 640);
    media_passthrough.close(state);
}

enum { RAW_SAMPLES = 320, NETWORK_SAMPLES = 160, WAIT = 1200 };
static xiaotai_queue_t s_input, s_output;
static xiaotai_signal_t s_acted, s_input_waiting, s_allow_input, s_rendered;
static xiaotai_task_t s_uplink, s_playout;
static atomic_bool s_hold_input;
static atomic_uint s_written;

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return false; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    assert(strcmp(config->name, "xt_capture") && strcmp(config->name, "xt_clean"));
    const xiaotai_err_t ret = __real_xiaotai_task_create(config, task);
    if (ret == XIAOTAI_OK && !strcmp(config->name, "xt_uplink")) s_uplink = *task;
    if (ret == XIAOTAI_OK && !strcmp(config->name, "xt_playout")) s_playout = *task;
    return ret;
}

static xiaotai_err_t input_open(void *self) { (void)self; return XIAOTAI_OK; }
static xiaotai_err_t input_close(void *self) { (void)self; return XIAOTAI_OK; }

static xiaotai_err_t input_read(void *self, void *buffer, size_t capacity, size_t *size)
{
    (void)self;
    assert(xiaotai_task_is_current(&s_uplink) && !xiaotai_on_owner() && capacity >= RAW_SAMPLES);
    if (atomic_load(&s_hold_input)) {
        xiaotai_signal_give(&s_input_waiting);
        (void)xiaotai_signal_take(&s_allow_input, 100);
        return XIAOTAI_ERR_TIMEOUT;
    }
    if (!xiaotai_queue_receive(&s_input, buffer, 20)) return XIAOTAI_ERR_TIMEOUT;
    *size = RAW_SAMPLES;
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t input = {.desc = {.encoding = XIAOTAI_CODEC_G711A,
        .rate_hz = 16000, .channels = 1, .frame_ms = 20}, .info = {.read_timeout_ms = 100, .group = 1},
        .open = input_open, .close = input_close, .read = input_read};
    return index == 0 ? &input : NULL;
}

static xiaotai_err_t output_write(void *self, void *data, size_t size)
{
    (void)self;
    assert(data && size && size <= NETWORK_SAMPLES * sizeof(int16_t));
    assert(xiaotai_task_is_current(&s_playout) && !xiaotai_task_is_current(&s_uplink));
    if (atomic_load(&s_hold_input)) {
        atomic_fetch_add(&s_written, 1);
        xiaotai_signal_give(&s_rendered);
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t output = {.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1},
        .info = {.group = 1, .max_write_frames = NETWORK_SAMPLES}, .open = input_open, .close = input_close, .write = output_write};
    return index ? NULL : &output;
}

static const xiaotai_media_requirement_t s_requirement[] = {
    {.uplink = {XIAOTAI_CODEC_G711A, 16000, 1, 20}},
    {.uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K},
    {.uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
     .latency = XIAOTAI_MEDIA_LATENCY_LOW},
};

static xiaotai_err_t require_formats(void)
{
    assert(xiaotai_media_require(&s_requirement[0]) == XIAOTAI_OK);
    assert(xiaotai_media_require(&s_requirement[1]) == XIAOTAI_OK);
    return xiaotai_media_require(&s_requirement[2]);
}

typedef struct { uint32_t generation; size_t size; uint8_t data[RAW_SAMPLES]; } sent_t;

static xiaotai_err_t send_packet(void *ctx, uint32_t generation, uint32_t stamp, const uint8_t *data, size_t size)
{
    (void)ctx;
    (void)stamp;
    assert(xiaotai_task_is_current(&s_uplink) && !xiaotai_on_owner() && (size == RAW_SAMPLES || size == NETWORK_SAMPLES));
    sent_t packet = {.generation = generation, .size = size};
    memcpy(packet.data, data, size);
    return xiaotai_queue_send(&s_output, &packet, 0) ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

static void exercise_session(void *opaque)
{
    static const xiaotai_media_sink_t sink = {.send = send_packet};
    const unsigned *op = opaque;
    const uint32_t id = (uint32_t)op[0] + 1U;
    if (op[1]) {
        const xiaotai_err_t err = xiaotai_media_open(&(xiaotai_media_session_t){.generation = id,
            .uplink = s_requirement[op[0]].uplink, .downlink = s_requirement[op[0]].downlink,
            .latency = s_requirement[op[0]].latency, .duplex = true, .sink = &sink});
        if (err != XIAOTAI_OK) fprintf(stderr, "open encoded uplink (generation %u): %d\n", (unsigned)id, err);
        assert(err == XIAOTAI_OK);
    } else {
        xiaotai_media_close(id);
    }
    xiaotai_signal_give(&s_acted);
}

static void act(unsigned which, bool open)
{
    const unsigned context[] = {which, open ? 1U : 0U};
    assert(xiaotai_run(exercise_session, (void *)context) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_acted, WAIT));
}

static void real_uplink(void)
{
    assert(xiaotai_queue_init(&s_input, 2, RAW_SAMPLES, XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&s_output, 2, sizeof(sent_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_acted) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_input_waiting) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_allow_input) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_rendered) == XIAOTAI_OK);
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t engine = {.name = "media", .init = xiaotai_engine_media_init,
        .start = xiaotai_engine_media_start};
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = deps, .init = require_formats};
    static const xiaotai_module_t *const modules[] = {&engine, &fixture};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 2,
        .queue_depth = 8}) == XIAOTAI_OK);
    const int64_t startup_deadline = xiaotai_time_us() + WAIT * 1000;
    xiaotai_media_status_t status;
    for (;;) {
        xiaotai_media_status(&status);
        if (status.ready) break;
        assert(xiaotai_time_us() < startup_deadline);
        xiaotai_delay_ms(1);
    }

    uint8_t input[RAW_SAMPLES];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(0x80U ^ (i * 13U));
    sent_t actual;
    act(0, true);
    assert(xiaotai_queue_send(&s_input, input, 0));
    assert(xiaotai_queue_receive(&s_output, &actual, WAIT));
    assert(actual.size == RAW_SAMPLES && actual.generation == 1 && !memcmp(input, actual.data, RAW_SAMPLES));
    act(0, false);

    act(1, true);
    assert(xiaotai_queue_send(&s_input, input, 0));
    assert(xiaotai_queue_receive(&s_output, &actual, WAIT));
    assert(actual.size == NETWORK_SAMPLES && actual.generation == 2);
    int16_t input_pcm[RAW_SAMPLES], output_pcm[NETWORK_SAMPLES];
    uint8_t expected[NETWORK_SAMPLES];
    xiaotai_media_down2_t converter = {0};
    xiaotai_media_alaw_decode(input, RAW_SAMPLES, input_pcm);
    assert(xiaotai_media_down2_process(&converter, input_pcm, RAW_SAMPLES, output_pcm, NETWORK_SAMPLES) == NETWORK_SAMPLES);
    xiaotai_media_alaw_encode(output_pcm, NETWORK_SAMPLES, expected);
    assert(memcmp(actual.data, expected, NETWORK_SAMPLES) == 0);
    act(1, false);

    /* A voice chip can wait for a packet longer than a 10 ms speaker block.
     * Both rendered blocks must reach HAL before the input read is released. */
    atomic_store(&s_hold_input, true);
    act(2, true);
    assert(xiaotai_signal_take(&s_input_waiting, WAIT));
    assert(xiaotai_media_push(3, &(xiaotai_media_packet_t){.encoding = XIAOTAI_CODEC_G711A,
        .rate_hz = 8000, .data = expected, .size = sizeof(expected)}));
    const int64_t rendered_deadline = xiaotai_time_us() + 80000;
    while (atomic_load(&s_written) < 2U && xiaotai_time_us() < rendered_deadline)
        (void)xiaotai_signal_take(&s_rendered, 20);
    assert(atomic_load(&s_written) == 2U);
    act(2, false);
    atomic_store(&s_hold_input, false);
    xiaotai_signal_give(&s_allow_input);
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&s_acted);
    xiaotai_signal_deinit(&s_input_waiting);
    xiaotai_signal_deinit(&s_allow_input);
    xiaotai_signal_deinit(&s_rendered);
    xiaotai_queue_deinit(&s_input);
    xiaotai_queue_deinit(&s_output);
}

int main(void)
{
    processor_packets();
    real_uplink();
    return 0;
}
