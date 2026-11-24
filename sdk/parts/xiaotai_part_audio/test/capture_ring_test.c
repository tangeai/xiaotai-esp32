// SPDX-License-Identifier: MIT
#include "audio_io_fake.h"
#include "capture_ring.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

enum { FRAMES = 256, CHANNELS = 4, BLOCKS = 16, PORT = 0 };

static xiaotai_audio_device_config_t config(int port)
{
    return (xiaotai_audio_device_config_t){
        .i2s = {.port = port, .mclk_pin = 0, .bclk_pin = 34, .ws_pin = 33, .din_pin = 14, .dout_pin = 13,
            .rate_hz = 16000, .rx_slots = CHANNELS, .tx_slots = 2, .dma_descriptors = 6, .dma_frames = FRAMES},
        .group = 1, .capture_channels = CHANNELS,
        .capture_roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_MONITOR},
        .capture_mode = XIAOTAI_AUDIO_CAPTURE_RING, .ring_blocks = BLOCKS, .read_timeout_ms = 100,
        .playback_channels = 2};
}

static void receive(const int16_t first)
{
    int16_t block[FRAMES][CHANNELS];
    for (size_t f = 0; f < FRAMES; ++f) {
        block[f][0] = (int16_t)(first + (int16_t)f);
        block[f][1] = INT16_MIN;
        block[f][2] = 0;
        block[f][3] = -9;
    }
    xt_audio_fake_receive(PORT, block, sizeof(block));
}

static void *other_reader(void *arg)
{
    const xiaotai_audio_input_t *in = arg;
    int16_t pcm[CHANNELS];
    size_t n = 1;
    assert(in->read(in->self, pcm, sizeof(pcm), &n) == XIAOTAI_ERR_INVALID_STATE && n == 0);
    return NULL;
}

static void test_full_ring(void)
{
    const xiaotai_audio_device_config_t c = config(PORT);
    const xiaotai_audio_input_t *in;
    const xiaotai_audio_output_t *out;
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(2);
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_ERR_NO_MEM && !in && !out);
    assert(xiaotai_mem_host_outstanding() == 0);
    xiaotai_mem_failure_t allocation;
    xiaotai_mem_failure(&allocation);
    assert(allocation.has_first && allocation.requested_size == 32768);
    xiaotai_mem_host_clear();
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_OK);
    assert(in->info.block_frames == FRAMES && in->info.read_timeout_ms == 100);
    xiaotai_mem_host_clear();
    assert(in->open(in->self) == XIAOTAI_OK);
    int16_t malformed[CHANNELS] = {0};
    xt_audio_fake_receive(PORT, malformed, sizeof(malformed));
    xt_audio_fake_overflow(PORT); /* The blocking-mode callback was not installed. */

    for (int i = 0; i < BLOCKS; ++i) receive((int16_t)(1 + i * FRAMES));
    receive(10000);
    int16_t pcm[FRAMES + 1][CHANNELS];
    size_t n;
    assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_OK && n == sizeof(pcm[0]));
    assert(pcm[0][0] == 1 && pcm[0][1] == INT16_MIN && pcm[0][2] == 0 && pcm[0][3] == -9);

    pthread_t other;
    assert(pthread_create(&other, NULL, other_reader, (void *)in) == 0);
    assert(pthread_join(other, NULL) == 0);

    /* A partially borrowed slot still occupies the ring. The ISR must not
     * overwrite it while a task is consuming it frame by frame. */
    receive(11000);
    assert(in->read(in->self, pcm, sizeof(pcm), &n) == XIAOTAI_OK && n == sizeof(pcm));
    for (int i = 0; i <= FRAMES; ++i) assert(pcm[i][0] == i + 2);
    receive(12000);
    size_t next = FRAMES + 3;
    for (; next <= FRAMES * BLOCKS; ++next) {
        assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_OK && pcm[0][0] == (int16_t)next);
    }
    assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_OK && pcm[0][0] == 12000);

    xiaotai_audio_stats_t s;
    in->stats(in->self, &s);
    assert(s.overruns == 2 && s.lost_frames == 2 * FRAMES && s.errors == 1);
    xiaotai_audio_device_stats_t ds;
    xiaotai_audio_device_stats(in, &ds);
    assert(ds.ring_overflows == 2 && ds.receive_overflows == 0 && ds.bad_blocks == 1 && ds.read_timeouts == 0);
    assert(ds.peak[0] == 12000 && ds.peak[1] == 32768 && ds.peak[2] == 0 && ds.peak[3] == 9);
    assert(ds.nonzero[0] == FRAMES * BLOCKS + 1 && ds.nonzero[1] == ds.nonzero[0] && !ds.nonzero[2]);
    xiaotai_audio_device_stats(out, &ds);
    assert(ds.peak[0] == 0 && ds.peak[1] == 0 && ds.nonzero[0] == FRAMES * BLOCKS + 1);

    assert(out->open(out->self) == XIAOTAI_OK);
    assert(in->close(in->self) == XIAOTAI_OK);
    receive(20000); /* Playback is still on, so RX still supplies DMA blocks. */
    assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_ERR_INVALID_STATE && n == 0);
    assert(in->open(in->self) == XIAOTAI_OK);
    assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_ERR_TIMEOUT && n == 0);
    xiaotai_audio_device_stats(in, &ds);
    assert(ds.read_timeouts == 1 && ds.last_error == XIAOTAI_ERR_TIMEOUT);
    receive(21000);
    assert(in->read(in->self, pcm, sizeof(pcm[0]), &n) == XIAOTAI_OK && pcm[0][0] == 21000);
    assert(in->close(in->self) == XIAOTAI_OK && out->close(out->self) == XIAOTAI_OK);
    assert(xiaotai_mem_host_allocations() == 0);
}

static void wait_for_reader(xt_capture_ring_t *ring)
{
    const int64_t until = xiaotai_time_us() + 300000;
    do {
        xiaotai_critical_enter(&ring->lock);
        const bool reading = ring->reading;
        xiaotai_critical_exit(&ring->lock);
        if (reading) return;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < until);
    assert(0 && "reader did not start");
}

static void *wait_read(void *ctx)
{
    xt_capture_ring_t *ring = ctx;
    int16_t pcm;
    size_t moved;
    assert(xt_capture_ring_read(ring, &pcm, sizeof(pcm), &moved) == XIAOTAI_OK && pcm == 42);
    assert(xt_capture_ring_read(ring, &pcm, sizeof(pcm), &moved) == XIAOTAI_ERR_TIMEOUT && !moved);
    return NULL;
}

static void test_signal_and_close(void)
{
    xiaotai_audio_device_config_t c = config(1);
    c.i2s.dma_frames = 1;
    c.i2s.rx_slots = 2;
    c.capture_channels = 1;
    c.ring_blocks = 3;
    xt_capture_ring_t r = {0};
    assert(xt_capture_ring_init(&r, &c) == XIAOTAI_OK);
    assert(xt_capture_ring_set_readable(&r, true, false) == XIAOTAI_OK);

    pthread_t reader;
    assert(pthread_create(&reader, NULL, wait_read, &r) == 0);
    wait_for_reader(&r);
    assert(xt_capture_ring_set_readable(&r, false, true) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give_from_isr(&r.ready); /* A wake alone is not a published block. */
    const int16_t wire_frame[] = {42, -1};
    xt_capture_ring_receive(&r, wire_frame, sizeof(wire_frame));
    assert(pthread_join(reader, NULL) == 0);

    assert(xt_capture_ring_set_readable(&r, false, true) == XIAOTAI_OK);
    xiaotai_signal_give_from_isr(&r.ready); /* Late wake after timeout and close. */
    int16_t pcm;
    size_t n;
    assert(xt_capture_ring_read(&r, &pcm, sizeof(pcm), &n) == XIAOTAI_ERR_INVALID_STATE && !n);
    xt_capture_ring_deinit(&r);
}

static void *trickle_frames(void *ctx)
{
    xt_capture_ring_t *ring = ctx;
    wait_for_reader(ring);
    for (int16_t pcm = 1; pcm <= 8; ++pcm) {
        xiaotai_delay_ms(25);
        xt_capture_ring_receive(ring, &pcm, sizeof(pcm));
    }
    return NULL;
}

static void test_whole_read_deadline(void)
{
    xiaotai_audio_device_config_t c = config(2);
    c.i2s.dma_frames = 1;
    c.i2s.rx_slots = c.capture_channels = 1;
    c.ring_blocks = 3;
    xt_capture_ring_t r = {0};
    assert(xt_capture_ring_init(&r, &c) == XIAOTAI_OK);
    assert(xt_capture_ring_set_readable(&r, true, false) == XIAOTAI_OK);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, trickle_frames, &r) == 0);
    const int64_t t = xiaotai_time_us();
    int16_t pcm[10];
    size_t moved;
    assert(xt_capture_ring_read(&r, pcm, sizeof(pcm), &moved) == XIAOTAI_ERR_TIMEOUT);
    const int64_t elapsed = xiaotai_time_us() - t;
    assert(elapsed >= 90000 && elapsed < 190000 && moved < 8 * sizeof(int16_t));
    for (size_t f = 0; f < moved / sizeof(int16_t); ++f) assert(pcm[f] == (int16_t)(f + 1));
    assert(pthread_join(producer, NULL) == 0);
    xt_capture_ring_deinit(&r);
}

static void test_latched_start_failure(void)
{
    xiaotai_audio_device_config_t c = config(3);
    xiaotai_audio_codec_t codec;
    xt_audio_fake_codec(&codec, 0);
    c.capture_codec = &codec;
    const xiaotai_audio_input_t *in;
    const xiaotai_audio_output_t *out;
    assert(xiaotai_audio_device_create(&c, &in, &out) == XIAOTAI_OK);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_OPEN_INPUT, XIAOTAI_ERR_TIMEOUT, true);
    assert(in->open(in->self) == XIAOTAI_ERR_TIMEOUT);
    xt_audio_fake_record_t log[4];
    assert(xt_audio_fake_log(log, 4) == 4);
    assert(log[0].call == FAKE_START && log[1].call == FAKE_OPEN_INPUT &&
           log[2].call == FAKE_CLOSE_INPUT && log[3].call == FAKE_STOP);
    xt_audio_fake_clear();
    assert(in->open(in->self) == XIAOTAI_ERR_INVALID_STATE && out->open(out->self) == XIAOTAI_ERR_INVALID_STATE);
    assert(xt_audio_fake_log(log, 4) == 0);
    xiaotai_audio_device_stats_t ds;
    xiaotai_audio_device_stats(in, &ds);
    assert(ds.latched && !ds.running && !ds.opens && ds.open_failures == 1 && ds.last_error == XIAOTAI_ERR_TIMEOUT);
}

int main(void)
{
    test_full_ring();
    test_signal_and_close();
    test_whole_read_deadline();
    test_latched_start_failure();
    return 0;
}
