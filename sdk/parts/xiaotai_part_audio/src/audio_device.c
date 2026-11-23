// SPDX-License-Identifier: MIT
#include "capture_ring.h"
#include "codec.h"
#include "xiaotai/parts/audio_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { INPUT_OPEN = 1U, OUTPUT_OPEN = 2U };

typedef struct audio_device {
    struct audio_device *next;
    xiaotai_audio_device_config_t cfg;
    xiaotai_audio_input_t input;
    xiaotai_audio_output_t output;
    xiaotai_mutex_t life;
    xt_audio_io_t *io;
    bool capture_started, playback_started, amp_on, gain_pending, volume_pending;
    uint8_t *silence;
    size_t pad_chunk;
    int8_t gain[XIAOTAI_AUDIO_CHANNELS_MAX];
    _Atomic uint32_t open_parts, volume;
    _Atomic bool running, latched, muted, enabled;
    _Atomic uint32_t opens, open_failures, receive_overflows, lost_frames, input_errors, read_timeouts, write_errors;
    _Atomic uint32_t peak[XIAOTAI_AUDIO_CHANNELS_MAX], nonzero[XIAOTAI_AUDIO_CHANNELS_MAX];
    _Atomic int last_error;
    xt_capture_ring_t ring[];  /* Only the RING device reserves the inline state. */
} audio_device_t;

/* Created by the board's single init task; published objects never move or
 * disappear. Queries can distinguish input and output without casting a part. */
static _Atomic(audio_device_t *) devices;

static bool has_amp(const audio_device_t *d) { return d->cfg.amplifier.set != NULL; }
static bool has_ring(const audio_device_t *d)
{
    return d->cfg.capture_channels && d->cfg.capture_mode == XIAOTAI_AUDIO_CAPTURE_RING;
}
static void note_error(audio_device_t *d, xiaotai_err_t err) { if (err != XIAOTAI_OK) atomic_store(&d->last_error, err); }

static void overflow(void *arg)
{
    audio_device_t *d = arg;
    atomic_fetch_add_explicit(&d->receive_overflows, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&d->lost_frames, d->cfg.i2s.dma_frames, memory_order_relaxed);
}

static uint8_t initial_gain_inputs(const audio_device_t *d)
{
    const xiaotai_audio_codec_t *c = d->cfg.capture_codec;
    return c && c->gain_inputs ? c->gain_inputs : d->cfg.mic_inputs;
}

static xiaotai_err_t apply_gain(audio_device_t *d, const int8_t *gain, uint8_t mask)
{
    xiaotai_audio_codec_t *c = d->cfg.capture_codec;
    if (!d->cfg.capture_channels || !c || !c->ops->gain) return XIAOTAI_OK;
    for (uint8_t i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i) {
        if (!(mask & (1U << i))) continue;
        const xiaotai_err_t err = c->ops->gain(c->self, i, gain[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

/* Volume 1..100 % scaled into the board's linear range 1..volume_max_percent. */
static uint32_t played_volume(const audio_device_t *d, uint32_t volume)
{
    const uint32_t max = d->cfg.volume_max_percent;
    if (!volume || !max || max >= 100U) return volume;
    const uint32_t scaled = (volume * max + 50U) / 100U;
    return scaled ? scaled : 1U;
}

static xiaotai_err_t apply_volume(audio_device_t *d, uint8_t volume)
{
    xiaotai_audio_codec_t *c = d->cfg.playback_codec;
    return c && c->ops->volume ? c->ops->volume(c->self, (uint8_t)played_volume(d, volume)) : XIAOTAI_OK;
}

static xiaotai_err_t apply_mute(audio_device_t *d, bool mute)
{
    xiaotai_audio_codec_t *c = d->cfg.playback_codec;
    return c && c->ops->mute ? c->ops->mute(c->self, mute) : XIAOTAI_OK;
}

static xiaotai_err_t close_codec(xiaotai_audio_codec_t *c, xt_audio_direction_t direction, bool *started)
{
    if (!*started) return XIAOTAI_OK;
    const xiaotai_err_t err = c->ops->close(c->self, direction);
    if (err == XIAOTAI_OK) *started = false;
    return err;
}

static xiaotai_err_t stop_device(audio_device_t *d, bool retain)
{
    xiaotai_err_t first = close_codec(d->cfg.capture_codec, XT_AUDIO_INPUT, &d->capture_started);
    xiaotai_err_t err = close_codec(d->cfg.playback_codec, XT_AUDIO_OUTPUT, &d->playback_started);
    if (first == XIAOTAI_OK) first = err;
    /* A chip whose close failed can still refer to the port. Retain that
     * ownership and the callback context rather than tearing it out. */
    if (d->io && !d->capture_started && !d->playback_started) {
        err = retain ? xt_audio_io_pause(d->io) : xt_audio_io_stop(d->io);
        if (err == XIAOTAI_OK && !retain) d->io = NULL;
        if (first == XIAOTAI_OK) first = err;
    }
    if (first != XIAOTAI_OK) atomic_store(&d->latched, true);
    atomic_store(&d->running, d->io && first != XIAOTAI_OK);
    return first;
}

static xiaotai_err_t open_codec(xiaotai_audio_codec_t *c, xt_audio_io_t *io, xt_audio_direction_t direction,
                                const xiaotai_audio_desc_t *description, bool *started)
{
    if (!c) return XIAOTAI_OK;
    const xiaotai_err_t err = c->ops->open(c->self, io, direction, description, started);
    return err == XIAOTAI_OK && !*started ? XIAOTAI_FAIL : err;
}

static xiaotai_err_t start_device(audio_device_t *d, bool input_opening)
{
    const xiaotai_audio_device_config_t *c = &d->cfg;
    const bool ring = has_ring(d);
    const bool reuse = XIAOTAI_AUDIO_RETAIN_I2S_DMA && d->io != NULL;
    xiaotai_err_t err = ring ? xt_capture_ring_set_readable(d->ring, input_opening, true) : XIAOTAI_OK;
    bool resume_failed = false;
    if (err == XIAOTAI_OK && reuse) {
        err = xt_audio_io_resume(d->io);
        resume_failed = err != XIAOTAI_OK;
    } else if (err == XIAOTAI_OK)
        err = xt_audio_io_start(&c->i2s, c->capture_channels, c->playback_channels,
                                c->capture_channels && !ring ? overflow : NULL,
                                ring ? xt_capture_ring_receive : NULL,
                                ring ? (void *)d->ring : d, &d->io);
    if (err == XIAOTAI_OK && !d->io) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK && has_amp(d)) err = xiaotai_line_set(&c->amplifier, false);
    if (err == XIAOTAI_OK) err = open_codec(c->playback_codec, d->io, XT_AUDIO_OUTPUT, &d->output.desc, &d->playback_started);
    if (err == XIAOTAI_OK) err = open_codec(c->capture_codec, d->io, XT_AUDIO_INPUT, &d->input.desc, &d->capture_started);
    if (err == XIAOTAI_OK && c->playback_codec && c->playback_codec->ops->check_clock)
        err = c->playback_codec->ops->check_clock(c->playback_codec->self, d->io);
    if (err == XIAOTAI_OK && c->capture_codec && c->capture_codec != c->playback_codec && c->capture_codec->ops->check_clock)
        err = c->capture_codec->ops->check_clock(c->capture_codec->self, d->io);
    if (err == XIAOTAI_OK) err = apply_gain(d, d->gain, initial_gain_inputs(d));
    if (err == XIAOTAI_OK) err = apply_volume(d, (uint8_t)atomic_load(&d->volume));
    if (err == XIAOTAI_OK) {
        d->amp_on = false;
        d->gain_pending = d->volume_pending = false;
        atomic_store(&d->enabled, false);
        atomic_store(&d->running, true);
        atomic_fetch_add(&d->opens, 1);
        return XIAOTAI_OK;
    }
    atomic_fetch_add(&d->open_failures, 1);
    const xiaotai_err_t rollback = stop_device(d, reuse && !resume_failed);
    if (ring || resume_failed || err == XIAOTAI_ERR_INVALID_STATE) atomic_store(&d->latched, true);
    note_error(d, rollback != XIAOTAI_OK ? rollback : err);
    return err;
}

static xiaotai_err_t enter_lifecycle(audio_device_t *d)
{
    return xiaotai_mutex_lock(&d->life, XIAOTAI_WAIT_FOREVER) ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
}

static xiaotai_err_t open_part(audio_device_t *d, uint32_t part)
{
    xiaotai_err_t err = enter_lifecycle(d);
    if (err != XIAOTAI_OK) return err;
    const uint32_t opened = atomic_load(&d->open_parts);
    if (atomic_load(&d->latched)) err = XIAOTAI_ERR_INVALID_STATE;
    else if (!(opened & part)) {
        if (!opened) err = start_device(d, part == INPUT_OPEN);
        else if (part == INPUT_OPEN) {
            if (d->gain_pending) err = apply_gain(d, d->gain, d->cfg.mic_inputs);
            if (err == XIAOTAI_OK) d->gain_pending = false;
            if (err == XIAOTAI_OK) err = has_ring(d) ? xt_capture_ring_set_readable(d->ring, true, true) :
                xt_audio_io_discard_input(d->io);
        } else if (d->volume_pending) {
            err = apply_volume(d, (uint8_t)atomic_load(&d->volume));
            if (err == XIAOTAI_OK) d->volume_pending = false;
        }
        if (err == XIAOTAI_OK) atomic_store(&d->open_parts, opened | part);
        else if (opened) {
            note_error(d, err);
            if (err == XIAOTAI_ERR_INVALID_STATE) err = XIAOTAI_FAIL; /* The other part still runs. */
        }
    }
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static xiaotai_err_t input_open(void *self) { return open_part(self, INPUT_OPEN); }
static xiaotai_err_t output_open(void *self) { return open_part(self, OUTPUT_OPEN); }

static size_t frame_bytes(uint8_t channels) { return (size_t)channels * sizeof(int16_t); }

static xiaotai_err_t write_block(audio_device_t *d, const void *data, size_t bytes)
{
    size_t moved = 0;
    xiaotai_err_t err = xt_audio_io_write(d->io, data, bytes, &moved);
    if (err == XIAOTAI_OK && moved != bytes) err = XIAOTAI_FAIL;
    if (err != XIAOTAI_OK) {
        atomic_fetch_add(&d->write_errors, 1);
        note_error(d, err);
    }
    return err;
}

static xiaotai_err_t drive_amp(audio_device_t *d, bool on)
{
    if (!has_amp(d) || on == d->amp_on) return XIAOTAI_OK;
    for (size_t frames = on ? d->cfg.amplifier_pad_frames : 0; frames;) {
        const size_t chunk = frames < d->pad_chunk ? frames : d->pad_chunk;
        const xiaotai_err_t err = write_block(d, d->silence, chunk * frame_bytes(d->cfg.playback_channels));
        if (err != XIAOTAI_OK) return err;
        frames -= chunk;
    }
    const xiaotai_err_t err = xiaotai_line_set(&d->cfg.amplifier, on);
    if (err == XIAOTAI_OK) {
        d->amp_on = on;
        if (on && d->cfg.amplifier_enable_ms) xiaotai_delay_ms(d->cfg.amplifier_enable_ms);
    }
    return err;
}

static xiaotai_err_t apply_output_gate(audio_device_t *d, bool enabled, bool muted)
{
    /* A chip can mute while retaining the clock. Without a control chip,
     * use the line for the same gate when there is one. */
    const bool amp_on = enabled && (d->cfg.playback_codec || !muted);
    xiaotai_err_t err = amp_on ? XIAOTAI_OK : drive_amp(d, false);
    if (err == XIAOTAI_OK) err = apply_mute(d, !enabled || muted);
    if (err == XIAOTAI_OK && amp_on) err = drive_amp(d, true);
    return err;
}

static xiaotai_err_t close_part(audio_device_t *d, uint32_t part)
{
    xiaotai_err_t err = enter_lifecycle(d);
    if (err != XIAOTAI_OK) return err;
    const uint32_t opened = atomic_load(&d->open_parts);
    if (atomic_load(&d->latched)) {
        xiaotai_mutex_unlock(&d->life);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (opened & part) {
        if (part == OUTPUT_OPEN) err = apply_output_gate(d, false, atomic_load(&d->muted));
        else if (has_ring(d)) err = xt_capture_ring_set_readable(d->ring, false, true);
        else if (opened & OUTPUT_OPEN) err = xt_audio_io_discard_input(d->io);
        if (err == XIAOTAI_OK) {
            if (part == OUTPUT_OPEN) atomic_store(&d->enabled, false);
            atomic_store(&d->open_parts, opened & ~part);
            if (opened == part) err = stop_device(d, XIAOTAI_AUDIO_RETAIN_I2S_DMA);
        }
    }
    note_error(d, err);
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static xiaotai_err_t input_close(void *self) { return close_part(self, INPUT_OPEN); }
static xiaotai_err_t output_close(void *self) { return close_part(self, OUTPUT_OPEN); }

static xiaotai_err_t set_gain(void *self, int8_t db)
{
    audio_device_t *d = self;
    const int min = d->input.info.gain_min_db;
    const int max = d->input.info.gain_max_db;
    if (db < min || db > max) return XIAOTAI_ERR_INVALID_ARG;
    const int step = d->input.info.gain_step_db;
    int rounded = db;
    if (step) {
        const int rem = db % step;
        rounded -= rem < 0 ? step + rem : rem;
        if (rounded < min) rounded = min;
    }
    const xiaotai_err_t lock = enter_lifecycle(d);
    if (lock != XIAOTAI_OK) return lock;
    int8_t gain[XIAOTAI_AUDIO_CHANNELS_MAX];
    memcpy(gain, d->gain, sizeof(gain));
    for (uint8_t i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i)
        if (d->cfg.mic_inputs & (1U << i)) gain[i] = (int8_t)rounded;

    const bool opened = (atomic_load(&d->open_parts) & INPUT_OPEN) != 0;
    const xiaotai_err_t err = opened ? apply_gain(d, gain, d->cfg.mic_inputs) : XIAOTAI_OK;
    if (err == XIAOTAI_OK) {
        memcpy(d->gain, gain, sizeof(gain));
        d->gain_pending = !opened;
    }
    note_error(d, err);
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static xiaotai_err_t set_volume(void *self, uint8_t percent)
{
    audio_device_t *d = self;
    if (percent > 100) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t lock = enter_lifecycle(d);
    if (lock != XIAOTAI_OK) return lock;
    const bool opened = (atomic_load(&d->open_parts) & OUTPUT_OPEN) != 0;
    const xiaotai_err_t err = opened ? apply_volume(d, percent) : XIAOTAI_OK;
    if (err == XIAOTAI_OK) {
        atomic_store(&d->volume, percent);
        d->volume_pending = !opened;
    }
    note_error(d, err);
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static xiaotai_err_t set_muted(void *self, bool muted)
{
    audio_device_t *d = self;
    const xiaotai_err_t lock = enter_lifecycle(d);
    if (lock != XIAOTAI_OK) return lock;
    const xiaotai_err_t err = atomic_load(&d->open_parts) & OUTPUT_OPEN ?
        apply_output_gate(d, atomic_load(&d->enabled), muted) : XIAOTAI_OK;
    if (err == XIAOTAI_OK) atomic_store(&d->muted, muted);
    note_error(d, err);
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static xiaotai_err_t set_enabled(void *self, bool enabled)
{
    audio_device_t *d = self;
    const xiaotai_err_t lock = enter_lifecycle(d);
    if (lock != XIAOTAI_OK) return lock;
    const xiaotai_err_t err = atomic_load(&d->open_parts) & OUTPUT_OPEN ?
        apply_output_gate(d, enabled, atomic_load(&d->muted)) : XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) atomic_store(&d->enabled, enabled);
    note_error(d, err);
    xiaotai_mutex_unlock(&d->life);
    return err;
}

static void note_pcm(audio_device_t *d, const uint8_t *bytes, size_t count)
{
    const size_t channels = d->cfg.capture_channels;
    uint32_t peaks[XIAOTAI_AUDIO_CHANNELS_MAX] = {0}, samples[XIAOTAI_AUDIO_CHANNELS_MAX] = {0};
    for (size_t i = 0; i < count / sizeof(int16_t); i += channels) {
        for (size_t j = 0; j < channels; ++j) {
            int16_t s;
            memcpy(&s, bytes + (i + j) * sizeof(s), sizeof(s));
            const uint32_t magnitude = s < 0 ? (uint32_t)-(int32_t)s : (uint32_t)s;
            if (magnitude > peaks[j]) peaks[j] = magnitude;
            if (s) ++samples[j];
        }
    }
    for (size_t i = 0; i < channels; ++i) {
        uint32_t previous = atomic_load(&d->peak[i]);
        while (peaks[i] > previous && !atomic_compare_exchange_weak(&d->peak[i], &previous, peaks[i])) {}
        atomic_fetch_add(&d->nonzero[i], samples[i]);
    }
}

static xiaotai_err_t read_pcm(void *self, void *data, size_t capacity, size_t *size)
{
    audio_device_t *d = self;
    if (size) *size = 0;
    if (!data || !size || !capacity || capacity > INT_MAX || capacity % frame_bytes(d->cfg.capture_channels))
        return XIAOTAI_ERR_INVALID_ARG;
    if (!(atomic_load(&d->open_parts) & INPUT_OPEN)) return XIAOTAI_ERR_INVALID_STATE;
    size_t moved = 0;
    const bool ring = has_ring(d);
    xiaotai_err_t err = ring ? xt_capture_ring_read(d->ring, data, capacity, &moved) :
        xt_audio_io_read(d->io, data, capacity, &moved);
    if (ring && err == XIAOTAI_ERR_INVALID_STATE) return err;
    if (err == XIAOTAI_OK && moved != capacity) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK || (ring && moved)) note_pcm(d, data, moved);
    if (err == XIAOTAI_OK) {
        *size = capacity;
    } else {
        if (err == XIAOTAI_ERR_TIMEOUT) atomic_fetch_add(&d->read_timeouts, 1);
        else atomic_fetch_add(&d->input_errors, 1);
        note_error(d, err);
    }
    return err;
}

static xiaotai_err_t write_pcm(void *self, void *data, size_t bytes)
{
    audio_device_t *d = self;
    const size_t frame = frame_bytes(d->cfg.playback_channels);
    if (!data || !bytes || bytes > INT_MAX || bytes % frame ||
        (d->cfg.max_write_frames && bytes / frame > d->cfg.max_write_frames)) return XIAOTAI_ERR_INVALID_ARG;
    if (!(atomic_load(&d->open_parts) & OUTPUT_OPEN)) return XIAOTAI_ERR_INVALID_STATE;
    if (!d->cfg.playback_codec) {
        const bool quiet = !has_amp(d) && (!atomic_load(&d->enabled) || atomic_load(&d->muted));
        const uint32_t volume = played_volume(d, atomic_load(&d->volume));
        if (quiet || !volume) memset(data, 0, bytes);
        else if (volume != 100U) {
            uint8_t *p = data;
            for (size_t i = 0; i < bytes; i += sizeof(int16_t)) {
                int16_t s;
                memcpy(&s, p + i, sizeof(s));
                s = (int16_t)((int32_t)s * (int32_t)volume / 100);
                memcpy(p + i, &s, sizeof(s));
            }
        }
    }
    return write_block(d, data, bytes);
}

static size_t flush(void *self)
{
    audio_device_t *d = self;
    return atomic_load(&d->open_parts) & OUTPUT_OPEN ?
        ((size_t)d->cfg.i2s.dma_descriptors + 1U) * d->cfg.i2s.dma_frames : 0;
}

static void input_stats(void *self, xiaotai_audio_stats_t *stats)
{
    audio_device_t *d = self;
    if (!stats) return;
    uint32_t overruns = 0, bad = 0;
    if (has_ring(d)) xt_capture_ring_stats(d->ring, &overruns, &bad);
    *stats = (xiaotai_audio_stats_t){.overruns = overruns + atomic_load(&d->receive_overflows),
        .lost_frames = overruns * d->cfg.i2s.dma_frames + atomic_load(&d->lost_frames),
        .errors = bad + atomic_load(&d->input_errors)};
}

static void output_stats(void *self, xiaotai_audio_stats_t *stats)
{
    audio_device_t *d = self;
    if (stats) *stats = (xiaotai_audio_stats_t){.errors = atomic_load(&d->write_errors)};
}

static bool codec_valid(const xiaotai_audio_codec_t *c)
{
    return !c || (c->ops && c->ops->open && c->ops->close);
}

static xiaotai_err_t validate(const xiaotai_audio_device_config_t *c)
{
    if (!c || !c->group || (!c->capture_channels && !c->playback_channels) || c->i2s.port < 0 ||
        !c->i2s.rate_hz || c->i2s.ws_pin < 0 || c->i2s.mclk_pin < -1 || c->playback_channels > 2U ||
        c->capture_channels > XIAOTAI_AUDIO_CHANNELS_MAX) return XIAOTAI_ERR_INVALID_ARG;

    const xiaotai_i2s_config_t *i = &c->i2s;
    if (c->capture_channels) {
        if (i->din_pin < 0 || c->capture_channels > i->rx_slots || i->rx_slots > XIAOTAI_AUDIO_CHANNELS_MAX ||
            (i->rx_mode == XIAOTAI_I2S_SLOTS && (i->rx_slots < 2 || i->bclk_pin < 0)) ||
            (i->rx_mode == XIAOTAI_I2S_PDM && i->rx_slots > 2) ||
            (i->rx_mode != XIAOTAI_I2S_SLOTS && i->rx_mode != XIAOTAI_I2S_PDM) ||
            !codec_valid(c->capture_codec) ||
            (c->capture_codec && c->gain_min_db > c->gain_max_db)) return XIAOTAI_ERR_INVALID_ARG;
        if (c->capture_mode == XIAOTAI_AUDIO_CAPTURE_RING) {
            if (!c->ring_blocks || !c->read_timeout_ms) return XIAOTAI_ERR_INVALID_ARG;
        } else if (c->capture_mode != XIAOTAI_AUDIO_CAPTURE_BLOCKING) return XIAOTAI_ERR_INVALID_ARG;
        if (c->capture_codec && c->gain_min_db != c->gain_max_db && (!c->mic_inputs || !c->capture_codec->ops->gain))
            return XIAOTAI_ERR_INVALID_ARG;
        if (c->capture_codec && c->capture_codec->gain_inputs &&
            (c->mic_inputs & ~c->capture_codec->gain_inputs)) return XIAOTAI_ERR_INVALID_ARG;
    }
    if (c->playback_channels) {
        if (i->dout_pin < 0 || (i->tx_mode == XIAOTAI_I2S_SLOTS && (i->tx_slots != 2 || i->bclk_pin < 0)) ||
            (i->tx_mode == XIAOTAI_I2S_PDM && (i->tx_slots < c->playback_channels || i->tx_slots > 2)) ||
            (i->tx_mode != XIAOTAI_I2S_SLOTS && i->tx_mode != XIAOTAI_I2S_PDM) ||
            !codec_valid(c->playback_codec) || (c->amplifier.get && !c->amplifier.set) ||
            c->volume_max_percent > 100U) return XIAOTAI_ERR_INVALID_ARG;
        if (c->playback_codec && (!c->playback_codec->ops->volume || !c->playback_codec->ops->mute))
            return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    return XIAOTAI_OK;
}

static audio_device_t *lookup(const void *part)
{
    if (part) for (audio_device_t *d = atomic_load(&devices); d; d = d->next) {
        if ((d->cfg.capture_channels && part == &d->input) || (d->cfg.playback_channels && part == &d->output)) return d;
    }
    return NULL;
}

static int8_t initial_gain(const audio_device_t *d)
{
    if (d->cfg.capture_codec) {
        for (uint8_t i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i)
            if (d->cfg.mic_inputs & (1U << i)) return d->gain[i];
    }
    return 0;
}

static void prepare_parts(audio_device_t *d)
{
    const xiaotai_audio_device_config_t *c = &d->cfg;
    const bool adjustable = c->capture_codec && c->gain_min_db != c->gain_max_db;
    const int8_t gain = initial_gain(d);
    const uint64_t latency = ((uint64_t)c->i2s.dma_frames * c->i2s.dma_descriptors * 1000U + c->i2s.rate_hz - 1U) /
                             c->i2s.rate_hz;
    d->input = (xiaotai_audio_input_t){.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = c->i2s.rate_hz,
        .channels = c->capture_channels}, .info = {.gain_db = gain,
        .gain_min_db = adjustable ? c->gain_min_db : gain, .gain_max_db = adjustable ? c->gain_max_db : gain,
        .gain_step_db = adjustable ? c->gain_step_db : 0, .settle_ms = c->capture_settle_ms,
        .echo_coupling_db = c->echo_coupling_db, .group = c->group, .block_frames = c->i2s.dma_frames,
        .read_timeout_ms = has_ring(d) ? c->read_timeout_ms : 0},
        .open = input_open, .read = read_pcm, .close = input_close, .set_gain_db = adjustable ? set_gain : NULL,
        .stats = input_stats, .self = d};
    memcpy(d->input.desc.roles, c->capture_roles, sizeof(d->input.desc.roles));
    d->output = (xiaotai_audio_output_t){.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = c->i2s.rate_hz,
        .channels = c->playback_channels, .roles = {c->playback_channels == 1 ? XIAOTAI_CHANNEL_MONO : XIAOTAI_CHANNEL_LEFT,
            c->playback_channels == 2 ? XIAOTAI_CHANNEL_RIGHT : XIAOTAI_CHANNEL_UNUSED}},
        .info = {.latency_ms = latency > UINT16_MAX ? UINT16_MAX : (uint16_t)latency,
        .enable_ms = has_amp(d) ? c->amplifier_enable_ms : 0, .max_write_frames = c->max_write_frames,
        .volume_span_cdb = c->playback_codec ? (uint16_t)(c->playback_codec->ops->volume_span_cdb *
            (c->volume_max_percent && c->volume_max_percent < 100U ? c->volume_max_percent : 100U) / 100U) : 0,
        .group = c->group, .flags = XIAOTAI_AUDIO_OUTPUT_PACED,
        .volume_max_percent = c->volume_max_percent && c->volume_max_percent < 100U ? c->volume_max_percent : 100U},
        .open = output_open, .write = write_pcm,
        .set_volume = set_volume, .set_muted = set_muted, .set_enabled = set_enabled, .flush_frames = flush,
        .close = output_close, .stats = output_stats, .self = d};
}

xiaotai_err_t xiaotai_audio_device_create(const xiaotai_audio_device_config_t *config,
                                          const xiaotai_audio_input_t **input, const xiaotai_audio_output_t **output)
{
    if (input) *input = NULL;
    if (output) *output = NULL;
    xiaotai_err_t err = validate(config);
    if (err != XIAOTAI_OK) return err;
    if ((!input || !config->capture_channels) && (!output || !config->playback_channels)) return XIAOTAI_ERR_INVALID_ARG;
    for (audio_device_t *prior = atomic_load(&devices); prior; prior = prior->next)
        if (prior->cfg.i2s.port == config->i2s.port) return XIAOTAI_ERR_INVALID_STATE;

    xiaotai_i2s_config_t geometry = config->i2s;
    err = xt_audio_io_describe(&geometry, config->capture_channels, config->playback_channels);
    if (err != XIAOTAI_OK) return err;
    if (!geometry.dma_frames || !geometry.dma_descriptors) return XIAOTAI_FAIL;

    const bool ring = config->capture_channels && config->capture_mode == XIAOTAI_AUDIO_CAPTURE_RING;
    audio_device_t *d = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*d) + (ring ? sizeof(*d->ring) : 0));
    if (!d) return XIAOTAI_ERR_NO_MEM;
    err = xiaotai_mutex_init(&d->life);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(d);
        return err;
    }
    d->cfg = *config;
    d->cfg.i2s = geometry;
    if (!d->cfg.capture_channels) d->cfg.capture_codec = NULL;
    if (!d->cfg.playback_channels) {
        d->cfg.playback_codec = NULL;
        d->cfg.amplifier = (xiaotai_line_t){0};
    }
    memcpy(d->gain, config->input_gain_db, sizeof(d->gain));
    atomic_init(&d->volume, 100);
    atomic_init(&d->open_parts, 0);
    atomic_init(&d->running, false);
    atomic_init(&d->latched, false);
    atomic_init(&d->muted, false);
    atomic_init(&d->enabled, false);
    atomic_init(&d->opens, 0);
    atomic_init(&d->open_failures, 0);
    atomic_init(&d->receive_overflows, 0);
    atomic_init(&d->lost_frames, 0);
    atomic_init(&d->input_errors, 0);
    atomic_init(&d->read_timeouts, 0);
    atomic_init(&d->write_errors, 0);
    atomic_init(&d->last_error, XIAOTAI_OK);
    for (uint8_t i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i) {
        atomic_init(&d->peak[i], 0);
        atomic_init(&d->nonzero[i], 0);
    }
    prepare_parts(d);
    if (has_amp(d) && config->amplifier_pad_frames) {
        d->pad_chunk = config->max_write_frames ? config->max_write_frames : geometry.dma_frames;
        if (d->pad_chunk > config->amplifier_pad_frames) d->pad_chunk = config->amplifier_pad_frames;
        d->silence = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, d->pad_chunk, frame_bytes(config->playback_channels));
        if (!d->silence) err = XIAOTAI_ERR_NO_MEM;
    }
    if (err == XIAOTAI_OK && ring) err = xt_capture_ring_init(d->ring, &d->cfg);
    if (err == XIAOTAI_OK && (config->verify_at_create || XIAOTAI_AUDIO_RETAIN_I2S_DMA)) {
        err = start_device(d, false);
        if (err == XIAOTAI_OK) err = stop_device(d, XIAOTAI_AUDIO_RETAIN_I2S_DMA);
    }
    if (err != XIAOTAI_OK && !atomic_load(&d->latched)) {
        if (ring) xt_capture_ring_deinit(d->ring);
        xiaotai_mem_free(d->silence);
        xiaotai_mutex_deinit(&d->life);
        xiaotai_mem_free(d);
        return err;
    }
    /* A failed verification may leave callbacks or handles live. Keep their
     * context but do not publish parts; the port cannot be recreated. */
    d->next = atomic_load(&devices);
    atomic_store(&devices, d);
    if (err != XIAOTAI_OK) return err;
    if (input && config->capture_channels) *input = &d->input;
    if (output && config->playback_channels) *output = &d->output;
    return XIAOTAI_OK;
}

void xiaotai_audio_device_stats(const void *part, xiaotai_audio_device_stats_t *stats)
{
    if (!stats) return;
    *stats = (xiaotai_audio_device_stats_t){0};
    audio_device_t *d = lookup(part);
    if (!d) return;
    stats->opens = atomic_load(&d->opens);
    stats->open_failures = atomic_load(&d->open_failures);
    if (has_ring(d)) xt_capture_ring_stats(d->ring, &stats->ring_overflows, &stats->bad_blocks);
    stats->receive_overflows = atomic_load(&d->receive_overflows);
    stats->read_timeouts = atomic_load(&d->read_timeouts);
    stats->write_errors = atomic_load(&d->write_errors);
    stats->running = atomic_load(&d->running);
    stats->latched = atomic_load(&d->latched);
    stats->last_error = atomic_load(&d->last_error);
    for (uint8_t i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i) {
        stats->peak[i] = (uint16_t)atomic_exchange(&d->peak[i], 0);
        stats->nonzero[i] = atomic_load(&d->nonzero[i]);
    }
}

void xiaotai_audio_device_report(const void *part, xiaotai_hal_write_fn_t write, void *ctx)
{
    audio_device_t *d = lookup(part);
    if (!d || !write || enter_lifecycle(d) != XIAOTAI_OK) return;
    xiaotai_audio_codec_t *playback = d->cfg.playback_codec, *capture = d->cfg.capture_codec;
    if (playback && playback->ops->report) playback->ops->report(playback->self, write, ctx);
    if (capture && capture != playback && capture->ops->report) capture->ops->report(capture->self, write, ctx);
    xt_audio_io_report(&d->cfg.i2s, atomic_load(&d->running), write, ctx);
    if (d->cfg.playback_channels) {
        /* The output gate as driven, and the amplifier pin as read back
         * (-1: no amplifier line, -2: the line cannot be read). */
        bool level = false;
        const int pin = !has_amp(d) ? -1 : !d->cfg.amplifier.get ? -2 :
            xiaotai_line_get(&d->cfg.amplifier, &level) == XIAOTAI_OK ? (int)level : -2;
        char line[112];
        (void)snprintf(line, sizeof(line), "output enabled=%u muted=%u volume=%u played=%u amp=%u amp_line=%d",
                       (unsigned)atomic_load(&d->enabled), (unsigned)atomic_load(&d->muted),
                       (unsigned)atomic_load(&d->volume), (unsigned)played_volume(d, atomic_load(&d->volume)),
                       (unsigned)d->amp_on, pin);
        write(ctx, line);
    }
    xiaotai_mutex_unlock(&d->life);
}
