// SPDX-License-Identifier: MIT
/* Sample-rate converters (resample.h). Fixed point throughout: Q15 for the
 * decimator (bit-exact with the uplink decimator it replaces), Q20 for the
 * interpolation and the general converter, accumulated in 64 bits so no input
 * can overflow. Rounding is half away from zero and every output saturates at
 * the 16-bit limits. */
#include "resample.h"

#include <string.h>

/* Symmetric rounding of a fixed-point accumulator by shift bits, saturated. */
static int16_t round_shift(int64_t value, unsigned shift, uint32_t *clipped)
{
    const int64_t half = (int64_t)1 << (shift - 1U);
    const int64_t rounded = value >= 0 ? (value + half) >> shift : -((-value + half) >> shift);
    if (rounded > INT16_MAX) {
        if (clipped) ++*clipped;
        return INT16_MAX;
    }
    if (rounded < INT16_MIN) {
        if (clipped) ++*clipped;
        return INT16_MIN;
    }
    return (int16_t)rounded;
}

/* ------------------------------------------------------------------ down2 */

/* 31-tap Hamming low-pass in Q15. Keeping state across packets avoids a
 * periodic edge transient and rejects the upper half-band before decimation. */
static const int16_t s_down2_q15[XIAOTAI_MEDIA_DOWN2_TAPS] = {
    -9, 65, 33, -132, -112, 261, 305, -438,
    -691, 635, 1429, -813, -3041, 937, 10266, 15378,
    10266, 937, -3041, -813, 1429, 635, -691, -438,
    305, 261, -112, -132, 33, 65, -9,
};

void xiaotai_media_down2_reset(xiaotai_media_down2_t *down2)
{
    if (down2) memset(down2, 0, sizeof(*down2));
}

static int16_t down2_push(xiaotai_media_down2_t *down2, int16_t sample, bool emit)
{
    if (!down2->initialized) {
        for (size_t i = 0; i < XIAOTAI_MEDIA_DOWN2_TAPS; ++i) down2->delay[i] = sample;
        down2->initialized = true;
    }
    down2->delay[down2->write_index] = sample;
    size_t index = down2->write_index;
    down2->write_index = (uint8_t)((down2->write_index + 1U) % XIAOTAI_MEDIA_DOWN2_TAPS);
    if (!emit) return 0;
    int64_t accumulator = 0;
    for (size_t tap = 0; tap < XIAOTAI_MEDIA_DOWN2_TAPS; ++tap) {
        accumulator += (int64_t)down2->delay[index] * s_down2_q15[tap];
        index = index == 0U ? XIAOTAI_MEDIA_DOWN2_TAPS - 1U : index - 1U;
    }
    /* Round half up, as the decimator this replaces does (bit-exact). */
    accumulator = (accumulator + (1 << 14)) >> 15;
    if (accumulator > INT16_MAX) return INT16_MAX;
    if (accumulator < INT16_MIN) return INT16_MIN;
    return (int16_t)accumulator;
}

size_t xiaotai_media_down2_process(xiaotai_media_down2_t *down2, const int16_t *input, size_t count,
                                   int16_t *output, size_t capacity)
{
    if (!down2 || !input || !output || (count & 1U) != 0U || capacity < count / 2U) return 0;
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        const int16_t filtered = down2_push(down2, input[i], !down2->emit_phase);
        if (!down2->emit_phase) output[written++] = filtered;
        down2->emit_phase = !down2->emit_phase;
    }
    return written;
}

/* -------------------------------------------------------------------- up2 */

#define FIR_SHIFT 20U

/* Even phase of a 63-tap Blackman-windowed sinc, cutoff at a quarter of the
 * output rate, in Q20; the taps sum to exactly 1 << 20 (unity gain, no
 * equalization). The odd phase of that filter is a single tap of 1: every
 * second output sample is an input sample delayed by 15 inputs. Group delay
 * 31 output samples, independent of block boundaries. */
static const int32_t s_up2_even_q20[XIAOTAI_MEDIA_UP2_HISTORY] = {
    0, 86, -387, 999, -2074, 3824, -6523, 10522,
    -16276, 24411, -35879, 52365, -77387, 120090, -214222, 664739,
    664739, -214222, 120090, -77387, 52365, -35879, 24411, -16276,
    10522, -6523, 3824, -2074, 999, -387, 86, 0,
};
#define UP2_ODD_TAP 15U

void xiaotai_media_up2_reset(xiaotai_media_up2_t *up2, int16_t first)
{
    if (!up2) return;
    for (size_t i = 0; i < XIAOTAI_MEDIA_UP2_HISTORY; ++i) up2->history[i] = first;
    up2->newest = 0;
    up2->initialized = true;
    up2->pending_tail = false;
    up2->clipped = 0;
}

/* input NULL feeds zeros (the flush). */
static size_t up2_convert(xiaotai_media_up2_t *up2, const int16_t *input, size_t count, int16_t *output)
{
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        up2->newest = (uint8_t)((up2->newest + 1U) % XIAOTAI_MEDIA_UP2_HISTORY);
        up2->history[up2->newest] = input ? input[i] : 0;
        int64_t accumulator = 0;
        size_t index = up2->newest; /* age k = 0, 1, ... */
        for (size_t k = 0; k < XIAOTAI_MEDIA_UP2_HISTORY; ++k) {
            accumulator += (int64_t)up2->history[index] * s_up2_even_q20[k];
            index = index == 0U ? XIAOTAI_MEDIA_UP2_HISTORY - 1U : index - 1U;
        }
        /* Reconstruction can overshoot near full scale: saturate at the 16-bit
         * boundary and count it, never wrap and never lower the gain. */
        output[written++] = round_shift(accumulator, FIR_SHIFT, &up2->clipped);
        const size_t odd = (up2->newest + XIAOTAI_MEDIA_UP2_HISTORY - UP2_ODD_TAP) % XIAOTAI_MEDIA_UP2_HISTORY;
        output[written++] = up2->history[odd];
    }
    return written;
}

size_t xiaotai_media_up2_process(xiaotai_media_up2_t *up2, const int16_t *input, size_t count, int16_t *output,
                                 size_t capacity)
{
    if (!up2 || !up2->initialized || !input || !output || count == 0 || count > capacity / 2U) return 0;
    const size_t written = up2_convert(up2, input, count, output);
    up2->pending_tail = true;
    return written;
}

size_t xiaotai_media_up2_finish(xiaotai_media_up2_t *up2, int16_t *output, size_t capacity)
{
    if (!up2 || !up2->initialized || !up2->pending_tail || !output || capacity < XIAOTAI_MEDIA_UP2_TAIL_SAMPLES)
        return 0;
    const size_t written = up2_convert(up2, NULL, XIAOTAI_MEDIA_UP2_TAIL_INPUT, output);
    up2->pending_tail = false;
    return written;
}

/* ------------------------------------------------------------------- rate */

/* Hamming-windowed sinc low-passes at 48 kHz, normalized to unity DC gain
 * before rounding to Q20: tap i of N is
 *   sin(2 pi (fc / 48000) (i - (N - 1) / 2)) / (pi (i - (N - 1) / 2))
 *   * (0.54 - 0.46 cos(2 pi i / (N - 1)))
 * divided by the sum of all taps. fc is 7/16 of the output rate, so each
 * table spans 32 output samples and keeps the same transition band relative
 * to its output rate. Test media_resample regenerates both from the formula. */

/* 16 kHz output: 96 taps, fc 7000 Hz, decimation 3. */
static const int32_t s_rate_16k_q20[96] = {
    -249, -571, -464, 44, 613, 781, 300, -589,
    -1206, -911, 308, 1613, 1836, 453, -1733, -2959,
    -1853, 1202, 3961, 3879, 339, -4331, -6258, -3120,
    3444, 8419, 7107, -680, -9512, -11914, -4449, 8464,
    16753, 12220, -4001, -20417, -22757, -5547, 21155, 36452,
    23381, -15833, -55801, -60495, -6168, 99864, 218000, 295518,
    295518, 218000, 99864, -6168, -60495, -55801, -15833, 23381,
    36452, 21155, -5547, -22757, -20417, -4001, 12220, 16753,
    8464, -4449, -11914, -9512, -680, 7107, 8419, 3444,
    -3120, -6258, -4331, 339, 3879, 3961, 1202, -1853,
    -2959, -1733, 453, 1836, 1613, 308, -911, -1206,
    -589, 300, 781, 613, 44, -464, -571, -249,
};

/* 8 kHz output: 192 taps, fc 3500 Hz, decimation 6. */
static const int32_t s_rate_8k_q20[192] = {
    -64, -180, -264, -297, -270, -185, -54, 101,
    250, 362, 407, 369, 244, 48, -185, -409,
    -572, -630, -555, -344, -25, 343, 686, 922,
    983, 832, 473, -40, -611, -1119, -1442, -1483,
    -1199, -613, 181, 1029, 1748, 2164, 2148, 1655,
    741, -438, -1650, -2629, -3135, -3004, -2199, -827,
    869, 2548, 3840, 4421, 4093, 2835, 831, -1554,
    -3836, -5506, -6138, -5492, -3581, -697, 2628, 5712,
    7861, 8510, 7365, 4489, 328, -4351, -8584, -11403,
    -12037, -10098, -5707, 480, 7339, 13469, 17443, 18098,
    14814, 7701, -2318, -13575, -23872, -30810, -32186, -26393,
    -12753, 8286, 35123, 65158, 95130, 121557, 141243, 151746,
    151746, 141243, 121557, 95130, 65158, 35123, 8286, -12753,
    -26393, -32186, -30810, -23872, -13575, -2318, 7701, 14814,
    18098, 17443, 13469, 7339, 480, -5707, -10098, -12037,
    -11403, -8584, -4351, 328, 4489, 7365, 8510, 7861,
    5712, 2628, -697, -3581, -5492, -6138, -5506, -3836,
    -1554, 831, 2835, 4093, 4421, 3840, 2548, 869,
    -827, -2199, -3004, -3135, -2629, -1650, -438, 741,
    1655, 2148, 2164, 1748, 1029, 181, -613, -1199,
    -1483, -1442, -1119, -611, -40, 473, 832, 983,
    922, 686, 343, -25, -344, -555, -630, -572,
    -409, -185, 48, 244, 369, 407, 362, 250,
    101, -54, -185, -270, -297, -264, -180, -64,
};

typedef struct {
    uint32_t out_hz;
    const int32_t *taps;
    uint16_t count;
} rate_table_t;

static const rate_table_t s_rate_tables[] = {
    {16000U, s_rate_16k_q20, (uint16_t)(sizeof(s_rate_16k_q20) / sizeof(s_rate_16k_q20[0]))},
    {8000U, s_rate_8k_q20, (uint16_t)(sizeof(s_rate_8k_q20) / sizeof(s_rate_8k_q20[0]))},
};

/* Interpolation flush: the source is followed by zeros until the instant
 * after the third has been passed (the converter it replaces fed four zero
 * samples and stopped as the fourth entered), then the filter is flushed with
 * one table length less one of zero 48 kHz samples. */
#define RATE_TAIL_INPUTS 4U

enum { STAGE_CONVERT, STAGE_FLUSH_INPUT, STAGE_FLUSH_FILTER, STAGE_DONE };

static const rate_table_t *rate_table(uint32_t out_hz)
{
    for (size_t i = 0; i < sizeof(s_rate_tables) / sizeof(s_rate_tables[0]); ++i) {
        if (s_rate_tables[i].out_hz == out_hz) return &s_rate_tables[i];
    }
    return NULL;
}

bool xiaotai_media_rate_supported(uint32_t in_hz, uint32_t out_hz)
{
    if (in_hz < XIAOTAI_MEDIA_RATE_MIN_HZ || in_hz > XIAOTAI_MEDIA_RATE_MAX_HZ) return false;
    if (in_hz == out_hz) return true;
    return rate_table(out_hz) != NULL;
}

xiaotai_err_t xiaotai_media_rate_open(xiaotai_media_rate_t *rate, uint32_t in_hz, uint8_t channels,
                                      uint32_t out_hz)
{
    if (!rate) return XIAOTAI_ERR_INVALID_ARG;
    memset(rate, 0, sizeof(*rate));
    rate->stage = STAGE_DONE; /* refuses conversion until opened successfully */
    if (channels < 1 || channels > 2) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_media_rate_supported(in_hz, out_hz)) return XIAOTAI_ERR_NOT_SUPPORTED;
    rate->channels = channels;
    rate->in_hz = in_hz;
    rate->copy = in_hz == out_hz;
    if (!rate->copy) {
        const rate_table_t *table = rate_table(out_hz);
        rate->taps = table->taps;
        rate->tap_count = table->count;
        rate->decimation = (uint8_t)(XIAOTAI_MEDIA_RATE_MID_HZ / out_hz);
        rate->tail_inputs = RATE_TAIL_INPUTS;
        rate->tail_zeros = (uint16_t)(table->count - 1U);
    }
    rate->stage = STAGE_CONVERT;
    return XIAOTAI_OK;
}

static int32_t le16(const uint8_t *p) { return (int16_t)(uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }

/* One source frame at twice the sample value: 2 * mono, or left + right. */
static int32_t frame_twice(const xiaotai_media_rate_t *rate, const uint8_t *frame)
{
    return rate->channels == 2 ? le16(frame) + le16(frame + 2) : 2 * le16(frame);
}

static void window_push(xiaotai_media_rate_t *rate, int32_t twice)
{
    rate->window[0] = rate->window[1];
    rate->window[1] = rate->window[2];
    rate->window[2] = rate->window[3];
    rate->window[3] = twice;
    rate->phase -= XIAOTAI_MEDIA_RATE_MID_HZ;
}

/* Cubic (Catmull-Rom) value between window[1] and window[2] at the current
 * phase, at four times the sample value. With w at twice the sample value
 * and the polynomial value = k0 + p (k1 + p (k2 + p k3)):
 * 2 k0 = 2 w1, 2 k1 = w2 - w0, 2 k2 = 2 w0 - 5 w1 + 4 w2 - w3,
 * 2 k3 = (w3 - w0) + 3 (w1 - w2). Horner from the highest power, rounded. */
static int32_t cubic(const xiaotai_media_rate_t *rate)
{
    const int64_t w0 = rate->window[0], w1 = rate->window[1], w2 = rate->window[2], w3 = rate->window[3];
    const int64_t p = (int64_t)(((uint64_t)rate->phase << 16) / XIAOTAI_MEDIA_RATE_MID_HZ); /* Q16 */
    int64_t value = (w3 - w0) + 3 * (w1 - w2);
    value = (2 * w0 - 5 * w1 + 4 * w2 - w3) + ((value * p + 32768) >> 16);
    value = (w2 - w0) + ((value * p + 32768) >> 16);
    value = 2 * w1 + ((value * p + 32768) >> 16);
    return (int32_t)value;
}

/* Enters one 48 kHz sample; true with *out set when a group is complete. */
static bool filter_push(xiaotai_media_rate_t *rate, int32_t sample, int16_t *out)
{
    rate->delay[rate->delay_pos] = sample;
    rate->delay_pos = (uint16_t)((rate->delay_pos + 1U) % rate->tap_count);
    if (++rate->group < rate->decimation) return false;
    rate->group = 0;
    /* Oldest sample against the first tap: the table is symmetric. */
    int64_t accumulator = 0;
    size_t index = rate->delay_pos;
    for (size_t tap = 0; tap < rate->tap_count; ++tap) {
        accumulator += (int64_t)rate->delay[index] * rate->taps[tap];
        if (++index == rate->tap_count) index = 0;
    }
    *out = round_shift(accumulator, FIR_SHIFT + 2U, NULL); /* Q20 taps, four times the sample value */
    return true;
}

static int16_t copy_frame(const xiaotai_media_rate_t *rate, const uint8_t *frame)
{
    if (rate->channels == 1) return (int16_t)le16(frame);
    const int32_t sum = frame_twice(rate, frame); /* average, half away from zero */
    return (int16_t)(sum >= 0 ? (sum + 1) >> 1 : -((-sum + 1) >> 1));
}

size_t xiaotai_media_rate_process(xiaotai_media_rate_t *rate, const uint8_t *frames, size_t count,
                                  size_t *consumed, int16_t *output, size_t capacity)
{
    if (consumed) *consumed = 0;
    if (!rate || !consumed || !output || (!frames && count) || rate->stage != STAGE_CONVERT) return 0;
    const size_t frame_bytes = 2U * rate->channels;
    size_t used = 0, written = 0;
    if (rate->copy) {
        written = count < capacity ? count : capacity;
        for (size_t i = 0; i < written; ++i) output[i] = copy_frame(rate, frames + i * frame_bytes);
        *consumed = written;
        return written;
    }
    while (written < capacity) {
        if (rate->phase >= XIAOTAI_MEDIA_RATE_MID_HZ) {
            if (used == count) break; /* the next instant needs the next frame */
            window_push(rate, frame_twice(rate, frames + used * frame_bytes));
            ++used;
            rate->started = true;
            continue;
        }
        const int32_t sample = cubic(rate);
        rate->phase += rate->in_hz;
        if (filter_push(rate, sample, &output[written])) ++written;
    }
    *consumed = used;
    return written;
}

size_t xiaotai_media_rate_finish(xiaotai_media_rate_t *rate, int16_t *output, size_t capacity)
{
    if (!rate || !output || rate->stage == STAGE_DONE) return 0;
    if (rate->copy || !rate->started) {
        rate->stage = STAGE_DONE;
        return 0;
    }
    if (rate->stage == STAGE_CONVERT) rate->stage = STAGE_FLUSH_INPUT;
    size_t written = 0;
    while (written < capacity && rate->stage != STAGE_DONE) {
        if (rate->stage == STAGE_FLUSH_INPUT) {
            if (rate->phase >= XIAOTAI_MEDIA_RATE_MID_HZ) {
                if (rate->tail_inputs > 1U) {
                    --rate->tail_inputs;
                    window_push(rate, 0);
                } else {
                    rate->stage = STAGE_FLUSH_FILTER;
                }
                continue;
            }
            const int32_t sample = cubic(rate);
            rate->phase += rate->in_hz;
            if (filter_push(rate, sample, &output[written])) ++written;
        } else if (rate->tail_zeros) {
            --rate->tail_zeros;
            if (filter_push(rate, 0, &output[written])) ++written;
        } else {
            /* The last partial group is completed with zeros: one more sample. */
            if (rate->group) {
                while (!filter_push(rate, 0, &output[written])) {
                }
                ++written;
            }
            rate->stage = STAGE_DONE;
        }
    }
    return written;
}

/* ------------------------------------------------------------------- plan */

xiaotai_media_resample_t xiaotai_media_resample_plan(uint32_t in_hz, uint32_t out_hz)
{
    if (in_hz == 0 || out_hz == 0) return XIAOTAI_MEDIA_RESAMPLE_NONE;
    if (in_hz == out_hz) return XIAOTAI_MEDIA_RESAMPLE_COPY;
    if (out_hz == 2U * in_hz) return XIAOTAI_MEDIA_RESAMPLE_UP2;
    if (in_hz == 2U * out_hz) return XIAOTAI_MEDIA_RESAMPLE_DOWN2;
    if (in_hz != out_hz && xiaotai_media_rate_supported(in_hz, out_hz)) return XIAOTAI_MEDIA_RESAMPLE_RATE;
    return XIAOTAI_MEDIA_RESAMPLE_NONE;
}
