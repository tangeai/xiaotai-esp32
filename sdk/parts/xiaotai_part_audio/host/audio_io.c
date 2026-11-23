// SPDX-License-Identifier: MIT
#include "audio_io_fake.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct xt_audio_io {
    bool allocated, active;
    xt_audio_overflow_fn overflow;
    xt_audio_rx_block_fn receive;
    void *ctx;
};

static xt_audio_io_t ports[64];
static xt_audio_fake_record_t log_calls[128];
static size_t log_count;
static struct { xiaotai_err_t err; bool partial; } failures[FAKE_CALLS];
static uint8_t last_write[16];
static size_t last_bytes;
static bool short_read, short_write;

void xt_audio_fake_clear(void)
{
    log_count = last_bytes = 0;
    memset(failures, 0, sizeof(failures));
    short_read = short_write = false;
}

void xt_audio_fake_fail(xt_audio_fake_call_t call, xiaotai_err_t err, bool partial)
{
    failures[call].err = err;
    failures[call].partial = partial;
}

xiaotai_err_t xt_audio_fake_call(xt_audio_fake_call_t call, int32_t value)
{
    if (log_count < sizeof(log_calls) / sizeof(log_calls[0])) log_calls[log_count++] = (xt_audio_fake_record_t){call, value};
    const xiaotai_err_t err = failures[call].err;
    failures[call].err = XIAOTAI_OK;
    return err;
}

size_t xt_audio_fake_log(xt_audio_fake_record_t *out, size_t capacity)
{
    const size_t count = log_count < capacity ? log_count : capacity;
    memcpy(out, log_calls, count * sizeof(*out));
    return count;
}

xiaotai_err_t xt_audio_io_describe(xiaotai_i2s_config_t *i2s, uint8_t rx, uint8_t tx)
{
    if (i2s->port < 0 || (size_t)i2s->port >= sizeof(ports) / sizeof(ports[0])) return XIAOTAI_ERR_NOT_SUPPORTED;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_DESCRIBE, 256 * rx + tx);
    if (err != XIAOTAI_OK) return err;
    if (!i2s->dma_descriptors) i2s->dma_descriptors = 6;
    if (!i2s->dma_frames) i2s->dma_frames = 240;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_audio_io_start(const xiaotai_i2s_config_t *i2s, uint8_t rx, uint8_t tx,
                                xt_audio_overflow_fn overflow, xt_audio_rx_block_fn receive,
                                void *ctx, xt_audio_io_t **io)
{
    xt_audio_io_t *p = &ports[i2s->port];
    *io = NULL;
    if (p->allocated) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_START, 256 * rx + tx);
    if (err == XIAOTAI_OK || failures[FAKE_START].partial) {
        *p = (xt_audio_io_t){.allocated = true, .active = true, .overflow = overflow, .receive = receive, .ctx = ctx};
        *io = p;
    }
    failures[FAKE_START].partial = false;
    return err;
}

xiaotai_err_t xt_audio_io_pause(xt_audio_io_t *io)
{
    if (!io->active) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_PAUSE, 0);
    if (err == XIAOTAI_OK) io->active = false;
    return err;
}

xiaotai_err_t xt_audio_io_resume(xt_audio_io_t *io)
{
    if (!io->allocated || io->active) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_RESUME, 0);
    if (err == XIAOTAI_OK) io->active = true;
    return err;
}

xiaotai_err_t xt_audio_io_stop(xt_audio_io_t *io)
{
    if (!io->allocated) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_STOP, 0);
    if (err == XIAOTAI_OK) *io = (xt_audio_io_t){0};
    return err;
}

xiaotai_err_t xt_audio_io_discard_input(xt_audio_io_t *io)
{
    return io->active && io->overflow ? xt_audio_fake_call(FAKE_DROP_INPUT, 0) : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xt_audio_io_read(xt_audio_io_t *io, void *data, size_t capacity, size_t *size)
{
    if (!io->active) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_READ, (int32_t)capacity);
    *size = 0;
    if (err != XIAOTAI_OK) return err;
    const int16_t sample[] = {INT16_MIN, 17, 0, -200};
    for (size_t n = 0; n < capacity / sizeof(int16_t); ++n)
        memcpy((uint8_t *)data + n * sizeof(int16_t), &sample[n % 4], sizeof(int16_t));
    *size = short_read ? capacity - 1U : capacity;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_audio_io_write(xt_audio_io_t *io, const void *data, size_t size, size_t *written)
{
    if (!io->active) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xt_audio_fake_call(FAKE_WRITE, (int32_t)size);
    last_bytes = size;
    memcpy(last_write, data, size < sizeof(last_write) ? size : sizeof(last_write));
    *written = err != XIAOTAI_OK ? 0 : short_write ? size - 1U : size;
    return err;
}

void xt_audio_io_report(const xiaotai_i2s_config_t *i2s, bool running, xiaotai_hal_write_fn_t write, void *ctx)
{
    char line[52];
    (void)snprintf(line, sizeof(line), "port=%d %s", i2s->port, running ? "running" : "stopped");
    write(ctx, line);
}

void xt_audio_fake_overflow(int port)
{
    if (ports[port].active && ports[port].overflow) ports[port].overflow(ports[port].ctx);
}

void xt_audio_fake_receive(int port, const void *dma, size_t bytes)
{
    if (ports[port].active && ports[port].receive) ports[port].receive(ports[port].ctx, dma, bytes);
}

uintptr_t xt_audio_io_reader_id(void)
{
    static _Atomic uintptr_t next = 1;
    static _Thread_local uintptr_t reader;
    if (!reader) reader = atomic_fetch_add(&next, 1);
    return reader;
}

void xt_audio_fake_short_io(bool read, bool write) { short_read = read; short_write = write; }

size_t xt_audio_fake_last_write(void *out, size_t capacity)
{
    size_t copy = last_bytes < sizeof(last_write) ? last_bytes : sizeof(last_write);
    if (capacity < copy) copy = capacity;
    memcpy(out, last_write, copy);
    return last_bytes;
}

static xiaotai_err_t codec_open(void *self, xt_audio_io_t *io, xt_audio_direction_t direction,
                               const xiaotai_audio_desc_t *desc, bool *opened)
{
    (void)self;
    if (!io->active || desc->encoding != XIAOTAI_CODEC_PCM16 || !desc->channels) return XIAOTAI_ERR_INVALID_STATE;
    const xt_audio_fake_call_t call = direction == XT_AUDIO_INPUT ? FAKE_OPEN_INPUT : FAKE_OPEN_OUTPUT;
    const xiaotai_err_t err = xt_audio_fake_call(call, desc->channels);
    *opened = err == XIAOTAI_OK || failures[call].partial;
    failures[call].partial = false;
    return err;
}

static xiaotai_err_t codec_close(void *self, xt_audio_direction_t direction)
{
    (void)self;
    return xt_audio_fake_call(direction == XT_AUDIO_INPUT ? FAKE_CLOSE_INPUT : FAKE_CLOSE_OUTPUT, 0);
}

static xiaotai_err_t codec_clock(void *self, xt_audio_io_t *io)
{
    (void)self;
    return io->active ? xt_audio_fake_call(FAKE_CHECK_CLOCK, 0) : XIAOTAI_ERR_INVALID_STATE;
}

static xiaotai_err_t codec_gain(void *self, uint8_t input, int8_t db)
{
    (void)self;
    return xt_audio_fake_call(FAKE_GAIN, (int32_t)input * 256 + db);
}

static xiaotai_err_t codec_volume(void *self, uint8_t percent)
{
    (void)self;
    return xt_audio_fake_call(FAKE_VOLUME, percent);
}

static xiaotai_err_t codec_mute(void *self, bool muted)
{
    (void)self;
    return xt_audio_fake_call(FAKE_MUTE, muted);
}

static void codec_report(void *self, xiaotai_hal_write_fn_t write, void *ctx)
{
    (void)self;
    write(ctx, "codec registers");
}

void xt_audio_fake_codec(xiaotai_audio_codec_t *codec, uint8_t inputs)
{
    static const xt_audio_codec_ops_t ops = {
        .open = codec_open, .close = codec_close, .check_clock = codec_clock, .gain = codec_gain,
        .volume = codec_volume, .mute = codec_mute, .report = codec_report, .volume_span_cdb = 4950};
    *codec = (xiaotai_audio_codec_t){.ops = &ops, .gain_inputs = inputs};
}
