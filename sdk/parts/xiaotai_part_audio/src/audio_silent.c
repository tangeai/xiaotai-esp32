// SPDX-License-Identifier: MIT
/* An audio device without sound hardware (xiaotai/parts/audio.h,
 * xiaotai_audio_silent_create): the input delivers silence and the output
 * drops what it is given, both at the pace of the sample clock, so the media
 * engine, calls and the conversation run on a board that has a camera and a
 * key but no microphone or speaker yet. */
#include <string.h>
#include "xiaotai/parts/audio.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define DEFAULT_RATE_HZ 16000U
#define DEFAULT_BLOCK_FRAMES 320U   /* 20 ms at 16 kHz */

typedef struct {
    xiaotai_audio_input_t input;
    xiaotai_audio_output_t output;
    xiaotai_critical_t lock;
    uint32_t rate_hz;
    uint16_t block_frames;
    uint8_t in_channels, out_channels;
    bool in_open, out_open;
    int64_t in_next_us, out_next_us;
    xiaotai_audio_stats_t in_stats, out_stats;
} silent_t;

/* Waits until `due` and moves it on by `frames`. A caller that comes late
 * starts the clock again from now, as a device that ran dry would; more than
 * one block late counts as an overrun (input) or underrun (output). */
static void pace(int64_t *due, size_t frames, uint32_t rate_hz, uint16_t block_frames, xiaotai_audio_stats_t *stats)
{
    const int64_t now = xiaotai_time_us();
    if (now > *due) {
        if (now - *due > (int64_t)block_frames * 1000000 / rate_hz) ++stats->overruns;
        *due = now;
    } else {
        xiaotai_delay_ms((uint32_t)((*due - now + 999) / 1000));
    }
    *due += (int64_t)frames * 1000000 / rate_hz;
}

static xiaotai_err_t in_open(void *self)
{
    silent_t *s = self;
    xiaotai_critical_enter(&s->lock);
    s->in_open = true;
    s->in_next_us = xiaotai_time_us();
    xiaotai_critical_exit(&s->lock);
    return XIAOTAI_OK;
}

static xiaotai_err_t in_read(void *self, void *data, size_t capacity, size_t *size)
{
    silent_t *s = self;
    const size_t frame = (size_t)s->in_channels * sizeof(int16_t);
    if (!s->in_open) return XIAOTAI_ERR_INVALID_STATE;
    if (!data || !size || !capacity || capacity % frame) return XIAOTAI_ERR_INVALID_ARG;
    pace(&s->in_next_us, capacity / frame, s->rate_hz, s->block_frames, &s->in_stats);
    memset(data, 0, capacity);
    *size = capacity;
    return XIAOTAI_OK;
}

static xiaotai_err_t in_close(void *self)
{
    silent_t *s = self;
    s->in_open = false;
    return XIAOTAI_OK;
}

static xiaotai_err_t in_gain(void *self, int8_t gain_db)
{
    (void)self;
    return gain_db == 0 ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

static void in_stats(void *self, xiaotai_audio_stats_t *stats)
{
    silent_t *s = self;
    xiaotai_critical_enter(&s->lock);
    *stats = s->in_stats;
    xiaotai_critical_exit(&s->lock);
}

static xiaotai_err_t out_open(void *self)
{
    silent_t *s = self;
    xiaotai_critical_enter(&s->lock);
    s->out_open = true;
    s->out_next_us = xiaotai_time_us();
    xiaotai_critical_exit(&s->lock);
    return XIAOTAI_OK;
}

static xiaotai_err_t out_write(void *self, void *data, size_t size)
{
    silent_t *s = self;
    const size_t frame = (size_t)s->out_channels * sizeof(int16_t);
    if (!s->out_open) return XIAOTAI_ERR_INVALID_STATE;
    if (!data || !size || size % frame) return XIAOTAI_ERR_INVALID_ARG;
    pace(&s->out_next_us, size / frame, s->rate_hz, s->block_frames, &s->out_stats);
    return XIAOTAI_OK;
}

static xiaotai_err_t out_volume(void *self, uint8_t percent)
{
    (void)self;
    return percent <= 100 ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t out_flag(void *self, bool on)
{
    (void)self;
    (void)on;
    return XIAOTAI_OK;
}

static size_t out_flush(void *self)
{
    silent_t *s = self;
    return s->out_open ? s->block_frames : 0;
}

static xiaotai_err_t out_close(void *self)
{
    silent_t *s = self;
    s->out_open = false;
    return XIAOTAI_OK;
}

static void out_stats(void *self, xiaotai_audio_stats_t *stats)
{
    silent_t *s = self;
    xiaotai_critical_enter(&s->lock);
    *stats = s->out_stats;
    xiaotai_critical_exit(&s->lock);
}

xiaotai_err_t xiaotai_audio_silent_create(const xiaotai_audio_silent_config_t *config,
                                          const xiaotai_audio_input_t **input,
                                          const xiaotai_audio_output_t **output)
{
    if (!config || (!config->capture_channels && !config->playback_channels) || !config->group ||
        config->capture_channels > XIAOTAI_AUDIO_CHANNELS_MAX || config->playback_channels > 2U)
        return XIAOTAI_ERR_INVALID_ARG;
    silent_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_init(&s->lock);
    s->rate_hz = config->rate_hz ? config->rate_hz : DEFAULT_RATE_HZ;
    s->block_frames = config->block_frames ? config->block_frames : DEFAULT_BLOCK_FRAMES;
    s->in_channels = config->capture_channels;
    s->out_channels = config->playback_channels;
    if (s->in_channels) {
        s->input.desc.encoding = XIAOTAI_CODEC_PCM16;
        s->input.desc.rate_hz = s->rate_hz;
        s->input.desc.channels = s->in_channels;
        for (uint8_t i = 0; i < s->in_channels; ++i)
            s->input.desc.roles[i] = config->capture_roles[i] ? config->capture_roles[i] : XIAOTAI_CHANNEL_MIC;
        s->input.info.group = config->group;
        s->input.info.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        s->input.info.block_frames = s->block_frames;
        s->input.open = in_open;
        s->input.read = in_read;
        s->input.close = in_close;
        s->input.set_gain_db = in_gain;
        s->input.stats = in_stats;
        s->input.self = s;
    }
    if (s->out_channels) {
        s->output.desc.encoding = XIAOTAI_CODEC_PCM16;
        s->output.desc.rate_hz = s->rate_hz;
        s->output.desc.channels = s->out_channels;
        s->output.desc.roles[0] = s->out_channels == 1 ? XIAOTAI_CHANNEL_MONO : XIAOTAI_CHANNEL_LEFT;
        if (s->out_channels == 2) s->output.desc.roles[1] = XIAOTAI_CHANNEL_RIGHT;
        s->output.info.group = config->group;
        s->output.info.flags = XIAOTAI_AUDIO_OUTPUT_PACED;
        s->output.info.volume_span_cdb = 4950;
        s->output.open = out_open;
        s->output.write = out_write;
        s->output.set_volume = out_volume;
        s->output.set_muted = out_flag;
        s->output.set_enabled = out_flag;
        s->output.flush_frames = out_flush;
        s->output.close = out_close;
        s->output.stats = out_stats;
        s->output.self = s;
    }
    if (input) *input = s->in_channels ? &s->input : NULL;
    if (output) *output = s->out_channels ? &s->output : NULL;
    return XIAOTAI_OK;
}
