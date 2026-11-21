// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_rollback.py
// Replaces legacy: tests/native/cores3/test_cores3_amplifier_clock.py
#include "audio_io_fake.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

static xiaotai_audio_codec_t codec;
static xt_audio_fake_record_t log_entries[80];
static int64_t line_rose_at;
static bool line_high;

static xiaotai_err_t amp_set(void *ctx, bool active)
{
    (void)ctx;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_AMP, active);
    if (err == XIAOTAI_OK) {
        line_high = active;
        if (active) line_rose_at = xiaotai_time_us();
    }
    return err;
}

static xiaotai_audio_device_config_t config(int port)
{
    return (xiaotai_audio_device_config_t){
        .i2s = {.port = port, .mclk_pin = -1, .bclk_pin = 1, .ws_pin = 2, .din_pin = 3, .dout_pin = 4,
            .rate_hz = 16000, .rx_slots = 2, .tx_slots = 2, .dma_descriptors = 6, .dma_frames = 240},
        .group = 1, .capture_codec = &codec, .capture_channels = 2,
        .capture_roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE}, .input_gain_db = {24, 11},
        .mic_inputs = 1, .gain_min_db = -6, .gain_max_db = 30, .gain_step_db = 3,
        .echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN, .capture_settle_ms = 32,
        .playback_codec = &codec, .playback_channels = 2, .max_write_frames = 240};
}

static size_t read_log(void) { return xt_audio_fake_log(log_entries, sizeof(log_entries) / sizeof(log_entries[0])); }

static void expect_log(const xt_audio_fake_call_t *calls, size_t n)
{
    assert(read_log() == n);
    for (size_t i = 0; i < n; ++i) assert(log_entries[i].call == calls[i]);
}

#define LOG(...) do { const xt_audio_fake_call_t calls[] = {__VA_ARGS__}; expect_log(calls, sizeof(calls)/sizeof(*calls)); } while (0)

static void capture_line(void *ctx, const char *line)
{
    unsigned *count = ctx;
    assert(line[0]);
    ++*count;
}

static void test_group(void)
{
    const xiaotai_audio_input_t *in;
    const xiaotai_audio_output_t *out;
    xiaotai_audio_device_config_t c = config(0);
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_OK);
    LOG(FAKE_DESCRIBE);
    c.input_gain_db[0] = 0;
    assert(in->desc.encoding == XIAOTAI_CODEC_PCM16 && in->desc.channels == 2 && in->desc.roles[1] == XIAOTAI_CHANNEL_REFERENCE);
    assert(in->info.group == out->info.group && in->info.gain_db == 24 && in->info.settle_ms == 32);
    assert(in->info.block_frames == 240 && in->info.read_timeout_ms == 0);
    assert(out->desc.roles[0] == XIAOTAI_CHANNEL_LEFT && out->desc.roles[1] == XIAOTAI_CHANNEL_RIGHT);
    assert(out->info.volume_span_cdb == 4950 && out->info.latency_ms == 90 && out->info.max_write_frames == 240);
    assert(out->info.flags & XIAOTAI_AUDIO_OUTPUT_PACED);
    assert(out->flush_frames(out->self) == 0);
    assert(in->set_gain_db(in->self, 31) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->set_volume(out->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    assert(in->set_gain_db(in->self, 16) == XIAOTAI_OK);
    assert(out->set_volume(out->self, 35) == XIAOTAI_OK);
    xt_audio_fake_clear();
    assert(out->open(out->self) == XIAOTAI_OK);
    LOG(FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CHECK_CLOCK, FAKE_GAIN, FAKE_GAIN, FAKE_VOLUME);
    assert(log_entries[4].value == 15 && log_entries[5].value == 256 + 11 && log_entries[6].value == 35);
    assert(out->flush_frames(out->self) == 7U * 240U);
    xt_audio_fake_clear();
    assert(in->open(in->self) == XIAOTAI_OK && out->open(out->self) == XIAOTAI_OK);
    LOG(FAKE_DROP_INPUT);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK);
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK);
    assert(out->set_muted(out->self, false) == XIAOTAI_OK);
    assert(out->set_enabled(out->self, false) == XIAOTAI_OK);
    LOG(FAKE_DROP_INPUT, FAKE_MUTE, FAKE_MUTE, FAKE_MUTE, FAKE_MUTE);
    assert(log_entries[1].value == 1 && log_entries[2].value == 1 && log_entries[3].value == 0 && log_entries[4].value == 1);

    xt_audio_fake_clear();
    assert(out->close(out->self) == XIAOTAI_OK);
    assert(out->flush_frames(out->self) == 0);
    assert(out->set_volume(out->self, 44) == XIAOTAI_OK);
    LOG(FAKE_MUTE);
    assert(out->open(out->self) == XIAOTAI_OK);
    LOG(FAKE_MUTE, FAKE_VOLUME);
    assert(log_entries[1].value == 44);
    assert(in->close(in->self) == XIAOTAI_OK);
    assert(in->set_gain_db(in->self, -1) == XIAOTAI_OK);
    assert(in->open(in->self) == XIAOTAI_OK);
    LOG(FAKE_MUTE, FAKE_VOLUME, FAKE_DROP_INPUT, FAKE_GAIN, FAKE_DROP_INPUT);
    assert(log_entries[3].value == -3);

    xt_audio_fake_clear();
    uint8_t pcm[961] = {0};
    size_t received = 9;
    assert(in->read(in->self, pcm, 0, &received) == XIAOTAI_ERR_INVALID_ARG && received == 0);
    assert(in->read(in->self, pcm, 7, &received) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->write(out->self, pcm, sizeof(pcm)) == XIAOTAI_ERR_INVALID_ARG);
    assert(read_log() == 0);
    assert(in->read(in->self, pcm + 1, 8, &received) == XIAOTAI_OK && received == 8);
    assert(out->write(out->self, pcm, 960) == XIAOTAI_OK);
    LOG(FAKE_READ, FAKE_WRITE);
    assert(log_entries[1].value == 960);
    xt_audio_fake_short_io(true, true);
    assert(in->read(in->self, pcm, 8, &received) == XIAOTAI_FAIL && received == 0);
    assert(out->write(out->self, pcm, 960) == XIAOTAI_FAIL);
    xt_audio_fake_fail(FAKE_READ, XIAOTAI_ERR_TIMEOUT, false);
    assert(in->read(in->self, pcm, 8, &received) == XIAOTAI_ERR_TIMEOUT && received == 0);
    LOG(FAKE_READ, FAKE_WRITE, FAKE_READ, FAKE_WRITE, FAKE_READ);
    xiaotai_mem_host_clear();
    xt_audio_fake_overflow(0);
    xt_audio_fake_overflow(0);
    assert(xiaotai_mem_host_allocations() == 0);
    xiaotai_audio_stats_t s;
    in->stats(in->self, &s);
    assert(s.overruns == 2 && s.lost_frames == 480 && s.errors == 1);
    out->stats(out->self, &s);
    assert(s.errors == 1);
    xiaotai_audio_device_stats_t stats;
    xiaotai_audio_device_stats(in, &stats);
    assert(stats.running && stats.opens == 1 && stats.read_timeouts == 1 && stats.peak[0] == 32768 && stats.peak[1] == 200);
    assert(stats.nonzero[0] == 1 && stats.nonzero[1] == 2 && stats.receive_overflows == 2);
    xiaotai_audio_device_stats(out, &stats);
    assert(stats.peak[0] == 0 && stats.nonzero[0] == 1);
    assert(out->close(out->self) == XIAOTAI_OK);
    assert(in->read(in->self, pcm, 8, &received) == XIAOTAI_FAIL); /* Other direction stayed alive. */
    xt_audio_fake_clear();
    assert(in->close(in->self) == XIAOTAI_OK && in->close(in->self) == XIAOTAI_OK);
    LOG(FAKE_CLOSE_INPUT, FAKE_CLOSE_OUTPUT, FAKE_STOP);
    unsigned lines = 0;
    xiaotai_audio_device_report(out, capture_line, &lines);
    assert(lines == 2); /* Shared codec reported once. */
    xiaotai_audio_device_stats(out, &stats);
    assert(!stats.running && !stats.latched);
}

static void test_plain_and_amp(void)
{
    const xiaotai_audio_input_t *in;
    const xiaotai_audio_output_t *out;
    xiaotai_audio_device_config_t c = config(1);
    c.capture_channels = 0;
    c.playback_codec = NULL;
    c.playback_channels = 1;
    c.i2s.rate_hz = 8000;
    c.i2s.dma_frames = 160;
    c.i2s.dma_descriptors = 4;
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_OK && !in && out);
    assert(out->desc.roles[0] == XIAOTAI_CHANNEL_MONO && out->info.latency_ms == 80);
    assert(out->set_enabled(out->self, true) == XIAOTAI_ERR_INVALID_STATE);
    assert(out->open(out->self) == XIAOTAI_OK);
    assert(out->set_volume(out->self, 25) == XIAOTAI_OK);
    int16_t sample = 400, received = -1;
    assert(out->write(out->self, &sample, sizeof(sample)) == XIAOTAI_OK && sample == 0);
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK);
    sample = -400;
    assert(out->write(out->self, &sample, sizeof(sample)) == XIAOTAI_OK && sample == -100);
    assert(xt_audio_fake_last_write(&received, sizeof(received)) == sizeof(sample) && received == -100);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK);
    sample = 400;
    assert(out->write(out->self, &sample, sizeof(sample)) == XIAOTAI_OK && sample == 0);
    assert(out->close(out->self) == XIAOTAI_OK);

    c.i2s.port = 2;
    c.amplifier = (xiaotai_line_t){.set = amp_set};
    c.amplifier_enable_ms = 4;
    c.amplifier_pad_frames = 160;
    c.max_write_frames = 160;
    assert(xiaotai_audio_device_create(&c, NULL, &out) == XIAOTAI_OK);
    assert(out->info.enable_ms == 4);
    assert(out->open(out->self) == XIAOTAI_OK && !line_high);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_WRITE, XIAOTAI_FAIL, false);
    assert(out->set_enabled(out->self, true) == XIAOTAI_FAIL && !line_high);
    LOG(FAKE_WRITE);
    xt_audio_fake_clear();
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK && line_high);
    assert(xiaotai_time_us() - line_rose_at >= 4000);
    LOG(FAKE_WRITE, FAKE_AMP);
    assert(log_entries[0].value == 160 * 2 && log_entries[1].value == 1);
    assert(xt_audio_fake_last_write(&received, sizeof(received)) == 320 && received == 0);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK && !line_high);
    assert(out->set_muted(out->self, false) == XIAOTAI_OK && line_high);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_AMP, XIAOTAI_FAIL, false);
    assert(out->close(out->self) == XIAOTAI_FAIL && line_high);
    LOG(FAKE_AMP); /* Failed safety gate means the port is not stopped. */
    xt_audio_fake_clear();
    assert(out->close(out->self) == XIAOTAI_OK && !line_high);
    LOG(FAKE_AMP, FAKE_STOP);

    c.i2s.port = 3;
    c.capture_codec = NULL;
    c.capture_channels = 1;
    c.playback_channels = 0;
    c.i2s.rx_mode = XIAOTAI_I2S_PDM;
    c.i2s.rx_slots = 1;
    c.i2s.bclk_pin = -1;
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_OK && !out && in);
    assert(!in->set_gain_db && in->info.gain_min_db == in->info.gain_max_db);
    assert(in->open(in->self) == XIAOTAI_OK && in->close(in->self) == XIAOTAI_OK);
}

static void test_rollback(void)
{
    const xt_audio_fake_call_t forward[] = {FAKE_START, FAKE_AMP, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT,
        FAKE_CHECK_CLOCK, FAKE_GAIN, FAKE_VOLUME};
    for (size_t i = 0; i < sizeof(forward) / sizeof(*forward); ++i) {
        xiaotai_audio_device_config_t c = config(10 + (int)i);
        c.amplifier = (xiaotai_line_t){.set = amp_set};
        const xiaotai_audio_input_t *in;
        assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_OK);
        xt_audio_fake_clear();
        xt_audio_fake_fail(forward[i], XIAOTAI_FAIL, true);
        assert(in->open(in->self) == XIAOTAI_FAIL);
        size_t n = read_log();
        assert(n >= 2 && log_entries[n - 1U].call == FAKE_STOP);
        if (i >= 2U) assert(log_entries[n - 2U].call == FAKE_CLOSE_OUTPUT);
        if (i >= 3U) assert(log_entries[n - 3U].call == FAKE_CLOSE_INPUT);
        xiaotai_audio_device_stats_t s;
        xiaotai_audio_device_stats(in, &s);
        assert(!s.running && !s.latched && s.open_failures == 1 && s.last_error == XIAOTAI_FAIL);
        assert(in->open(in->self) == XIAOTAI_OK && in->close(in->self) == XIAOTAI_OK);
    }
    const xiaotai_audio_output_t *out;
    xiaotai_audio_device_config_t c = config(20);
    assert(xiaotai_audio_device_create(&c, NULL, &out) == XIAOTAI_OK);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_CHECK_CLOCK, XIAOTAI_FAIL, false);
    xt_audio_fake_fail(FAKE_CLOSE_INPUT, XIAOTAI_ERR_TIMEOUT, false);
    assert(out->open(out->self) == XIAOTAI_FAIL);
    LOG(FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CHECK_CLOCK, FAKE_CLOSE_INPUT, FAKE_CLOSE_OUTPUT);
    assert(out->open(out->self) == XIAOTAI_ERR_INVALID_STATE);
    assert(out->close(out->self) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_audio_device_stats_t s;
    xiaotai_audio_device_stats(out, &s);
    assert(s.latched && s.running && s.last_error == XIAOTAI_ERR_TIMEOUT);

    c.i2s.port = 21;
    c.verify_at_create = true;
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_STOP, XIAOTAI_ERR_TIMEOUT, false);
    assert(xiaotai_audio_device_create(&c, NULL, &out) == XIAOTAI_ERR_TIMEOUT && !out);
    assert(xiaotai_audio_device_create(&c, NULL, &out) == XIAOTAI_ERR_INVALID_STATE && !out);
    c.i2s.port = 22;
    xt_audio_fake_clear();
    c.i2s.dma_descriptors = 0;
    c.i2s.dma_frames = 256;
    assert(xiaotai_audio_device_create(&c, NULL, &out) == XIAOTAI_OK && out->info.latency_ms == 96);
    LOG(FAKE_DESCRIBE, FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CHECK_CLOCK, FAKE_GAIN, FAKE_GAIN,
        FAKE_VOLUME, FAKE_CLOSE_INPUT, FAKE_CLOSE_OUTPUT, FAKE_STOP);
    assert(out->flush_frames(out->self) == 0);
}

static void test_config(void)
{
    const xiaotai_audio_input_t *in;
    xiaotai_audio_device_config_t c = config(30);
    c.capture_channels = c.playback_channels = 0;
    assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_ERR_INVALID_ARG);
    c = config(30);
    c.capture_mode = XIAOTAI_AUDIO_CAPTURE_RING;
    assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_ERR_INVALID_ARG);
    c.ring_blocks = 16;
    assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_ERR_INVALID_ARG);
    c = config(30);
    xt_audio_fake_fail(FAKE_DESCRIBE, XIAOTAI_ERR_NOT_SUPPORTED, false);
    assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_ERR_NOT_SUPPORTED && !in);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_audio_device_create(&c, &in, NULL) == XIAOTAI_ERR_NO_MEM && !in);
}

int main(void)
{
    xt_audio_fake_codec(&codec, 3);
    test_group();
    test_plain_and_amp();
    test_rollback();
    test_config();
    return 0;
}
