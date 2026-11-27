// SPDX-License-Identifier: MIT
#include "playout.h"

#include <string.h>
#include "diag.h"
#include "downlink_ring.h"
#include "downlink_rx.h"
#include "level.h"
#include "loudness.h"
#include "pacer.h"
#include "resample.h"
#include "trace.h"
#include "topology.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define FNV_OFFSET UINT32_C(2166136261)
#define FNV_PRIME UINT32_C(16777619)
#define FADE_RATE_DIVISOR 200U

_Static_assert(_Alignof(max_align_t) % sizeof(int16_t) == 0, "whole-sample PCM alignment");
_Static_assert(XIAOTAI_MEDIA_RATE_MAX_HZ / FADE_RATE_DIVISOR <= UINT8_MAX, "fade counter fits");

struct media_playout {
    const xiaotai_audio_output_t *part;
    xiaotai_downlink_t *ring;
    xiaotai_critical_t lock;
    xiaotai_media_stats_t counters, snapshot;
    uint32_t supported_ms, rate, latency_ms, generation, epoch;
    uint32_t blocks, resample_clips;
    xiaotai_media_meter_t meter;
    size_t block, packet_capacity, decode_capacity, resample_capacity, pending_capacity, pending, flush_remaining;
    uint8_t channels, format_slot;
    media_pacer_policy_t policy;
    uint16_t stream;
    bool have_stream, seen_drain;
    uint8_t fade_remaining;
    bool played_activation;
    bool heard_network, warned_no_stream;
    int64_t session_begin_us;
    int64_t next_us, buffered_until, sound_until, first_pending_us;
    int32_t speed_credit;
    media_pacer_t pacer;
    int16_t *decode, *pending_pcm, *mono, *wire;
    uint8_t *packet;
    /* Filling in for lost packets. held: a packet is popped but not decoded
     * yet, because the gap before it is filled first. */
    bool held;
    xiaotai_downlink_packet_t held_packet;
    uint16_t held_stream, held_samples;
    uint8_t held_slot;
    uint32_t next_stamp;            /* sender time right after the last sound */
    uint32_t last_samples;          /* of the last packet, at its own rate */
    uint16_t dry_ms;                /* made up since the last packet, nothing having come */
    size_t tail;                    /* of the pending audio, how much at its head is such a made-up tail */
    uint16_t conceal_gap_ms, conceal_dry_ms;
    media_resampler_storage_t resampler[];
};

static size_t min(size_t a, size_t b) { return a < b ? a : b; }

static void restart_fade(media_playout_t *p)
{
    p->fade_remaining = (uint8_t)(p->rate / FADE_RATE_DIVISOR);
}

xiaotai_err_t media_playout_create(const xiaotai_audio_output_t *part, xiaotai_downlink_t *ring,
                                  uint32_t downlink_ms, media_playout_t **out)
{
    if (!part || !out || !part->write || part->desc.encoding != XIAOTAI_CODEC_PCM16 ||
        !part->desc.channels || part->desc.channels > XIAOTAI_AUDIO_CHANNELS_MAX ||
        part->desc.rate_hz < XIAOTAI_MEDIA_RATE_MIN_HZ || part->desc.rate_hz > XIAOTAI_MEDIA_RATE_MAX_HZ) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const uint32_t rate = part->desc.rate_hz;
    size_t decoded = 0, converted = 0, resampled_input = 0;
    bool direct = false;
    if (ring) {
        for (uint8_t id = 1; id < 32U; ++id) {
            media_rx_format_t fmt;
            if (!media_rx_format_at(id, &fmt)) continue;
            const bool fixed = fmt.format.encoding == XIAOTAI_CODEC_PCM16 ||
                               fmt.format.encoding == XIAOTAI_CODEC_G711A || fmt.format.encoding == XIAOTAI_CODEC_G711U;
            const uint32_t ms = fixed ? fmt.format.packet_ms : XIAOTAI_MEDIA_PACKET_MS_MAX;
            const size_t n = (fmt.format.rate_hz * ms + 999U) / 1000U;
            const size_t m = (rate * ms + 999U) / 1000U;
            if (n > decoded) decoded = n;
            if (m > converted) converted = m;
            if (fmt.format.rate_hz == rate) direct = true;
            else if (n > resampled_input) resampled_input = n;
        }
    }
    const bool resampler = xiaotai_mem_has_external() || resampled_input != 0;
    const size_t bytes = sizeof(media_playout_t) + (resampler ? sizeof(media_resampler_storage_t) : 0);
    media_playout_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, bytes);
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->part = part;
    p->ring = ring;
    p->supported_ms = downlink_ms;
    p->rate = rate;
    p->channels = part->desc.channels;
    const int32_t policy = xiaotai_media_param("playout.pacer_policy", MEDIA_PACER_ADAPTIVE);
    p->policy = policy >= MEDIA_PACER_ADAPTIVE && policy <= MEDIA_PACER_FIXED ?
                (media_pacer_policy_t)policy : MEDIA_PACER_ADAPTIVE;
    const int32_t gap = xiaotai_media_param("playout.conceal_gap_ms", 120);
    const int32_t dry = xiaotai_media_param("playout.conceal_dry_ms", 40);
    p->conceal_gap_ms = (uint16_t)(gap < 0 ? 0 : gap > 500 ? 500 : gap);
    p->conceal_dry_ms = (uint16_t)(dry < 0 ? 0 : dry > 200 ? 200 : dry);
    p->latency_ms = part->info.latency_ms;
    if (!p->latency_ms && part->flush_frames) p->latency_ms = (uint32_t)(part->flush_frames(part->self) * 1000U / rate);
    p->block = media_output_block_frames(part);
    p->decode_capacity = decoded;
    p->resample_capacity = resampled_input;
    size_t tail_slack = 2U;
    const size_t max_pad = _Alignof(max_align_t) / sizeof(int16_t) - 1U;
    /* fill() runs below wanted, leaving at least one extra sample besides
     * tail_slack. Keep room to align a full packet after an odd paced tail. */
    if (direct && max_pad > tail_slack + 1U) tail_slack = max_pad - 1U;
    p->pending_capacity = converted + p->block + (p->block + 39U) / 40U + tail_slack;
    if (ring) {
        p->packet_capacity = media_downlink_packet_capacity(ring);
        p->packet = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->packet_capacity);
        if (resampled_input)
            p->decode = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, resampled_input * sizeof(*p->decode));
        p->pending_pcm = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->pending_capacity * sizeof(*p->pending_pcm));
    }
    p->mono = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->block * sizeof(*p->mono));
    p->wire = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->block * p->channels * sizeof(*p->wire));
    if (!p->mono || !p->wire ||
        (ring && (!p->packet || (resampled_input && !p->decode) || !p->pending_pcm))) {
        media_playout_destroy(p);
        return XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_critical_init(&p->lock);
    *out = p;
    return XIAOTAI_OK;
}

void media_playout_destroy(media_playout_t *p)
{
    if (!p) return;
    xiaotai_mem_free(p->wire);
    xiaotai_mem_free(p->mono);
    xiaotai_mem_free(p->pending_pcm);
    xiaotai_mem_free(p->decode);
    xiaotai_mem_free(p->packet);
    xiaotai_mem_free(p);
}

static void publish(media_playout_t *p)
{
    xiaotai_critical_enter(&p->lock);
    p->snapshot = p->counters;
    xiaotai_critical_exit(&p->lock);
}

void media_playout_reset(media_playout_t *p, uint32_t generation, uint32_t epoch, xiaotai_media_latency_t latency)
{
    if (!p || (generation == p->generation && epoch == p->epoch)) return;
    if (generation && generation != p->generation) {
        p->counters = (xiaotai_media_stats_t){0};
        p->heard_network = p->warned_no_stream = false;
        p->session_begin_us = 0;
    } else {
        p->counters.pcm_discarded += (uint32_t)p->pending;
        p->counters.pcm_pending = 0;
    }
    p->generation = generation;
    p->epoch = epoch;
    p->pending = p->tail = p->flush_remaining = 0;
    p->have_stream = p->seen_drain = false;
    p->held = false;
    p->dry_ms = 0;
    p->last_samples = 0;
    p->played_activation = false;
    restart_fade(p);
    p->next_us = p->buffered_until = p->sound_until = p->first_pending_us = 0;
    p->speed_credit = 0;
    xiaotai_media_meter_reset(&p->meter);
    p->resample_clips = p->blocks = 0;
    media_pacer_open(&p->pacer, latency, p->supported_ms, p->policy);
    media_pacer_set_prefill_packets(&p->pacer, xiaotai_media_param("playout.prefill_packets", 0));
    publish(p);
}

static void hash_pcm(media_playout_t *p, const int16_t *pcm, size_t decoded)
{
    uint32_t hash = p->counters.pcm_out_hash ? p->counters.pcm_out_hash : FNV_OFFSET;
    for (size_t i = 0; i < decoded; ++i) {
        const uint16_t sample = (uint16_t)pcm[i];
        hash = (hash ^ (uint8_t)sample) * FNV_PRIME;
        hash = (hash ^ (uint8_t)(sample >> 8)) * FNV_PRIME;
    }
    p->counters.pcm_out_hash = hash;
}

/* Where a codec may write n samples so that nothing is moved twice: straight
 * behind the pending audio when no rate conversion follows. */
static int16_t *decode_target(media_playout_t *p, xiaotai_media_resample_t plan, size_t *capacity)
{
    int16_t *out = p->pending_pcm + p->pending;
    const size_t space = p->pending_capacity - p->pending;
    int16_t *target = p->decode;
    *capacity = p->resample_capacity;
    if (plan == XIAOTAI_MEDIA_RESAMPLE_COPY) {
        /* Write past the committed tail so even a failed decode cannot
         * affect pending audio. Match the alignment of a decoder's scratch. */
        const size_t misalignment = (uintptr_t)out % _Alignof(max_align_t);
        const size_t pad = misalignment ? (_Alignof(max_align_t) - misalignment) / sizeof(*out) : 0;
        *capacity = 0;
        if (pad <= space) {
            target = out + pad;
            *capacity = space - pad;
        }
    }
    *capacity = min(*capacity, p->decode_capacity);
    return target;
}

/* Converts n decoded samples to the output rate behind the pending audio;
 * false when the conversion failed. */
static bool append(media_playout_t *p, uint32_t rate, xiaotai_media_resample_t plan, int16_t *decoded, size_t n,
                   bool restart)
{
    int16_t *out = p->pending_pcm + p->pending;
    const size_t space = p->pending_capacity - p->pending;
    size_t converted = 0;
    switch (plan) {
        case XIAOTAI_MEDIA_RESAMPLE_COPY:
            if (out != decoded) memmove(out, decoded, n * sizeof(*out));
            decoded = out;
            converted = n;
            break;
        case XIAOTAI_MEDIA_RESAMPLE_UP2: {
            xiaotai_media_up2_t *up = &p->resampler->up;
            if (restart) xiaotai_media_up2_reset(up, decoded[0]);
            const uint32_t before = up->clipped;
            converted = xiaotai_media_up2_process(up, decoded, n, out, space);
            p->resample_clips += up->clipped - before;
            break;
        }
        case XIAOTAI_MEDIA_RESAMPLE_DOWN2:
            if (restart) xiaotai_media_down2_reset(&p->resampler->down);
            converted = xiaotai_media_down2_process(&p->resampler->down, decoded, n, out, space);
            break;
        case XIAOTAI_MEDIA_RESAMPLE_RATE: {
            xiaotai_media_rate_t *state = &p->resampler->rate;
            if (restart && xiaotai_media_rate_open(state, rate, 1, p->rate) != XIAOTAI_OK) break;
            size_t consumed = 0;
            converted = xiaotai_media_rate_process(state, (const uint8_t *)decoded, n, &consumed, out, space);
            if (consumed != n) converted = 0;
            break;
        }
        default: break;
    }
    if (!converted) return false;
    hash_pcm(p, decoded, n);
    if (!p->pending) p->first_pending_us = xiaotai_time_us();
    p->pending += converted;
    p->counters.pcm_decoded += (uint32_t)converted;
    return true;
}

/* Makes up samples of sound continuing the current stream; next: the packet
 * that follows, NULL when none came. */
static bool conceal(media_playout_t *p, const media_rx_format_t *codec, const uint8_t *next, size_t next_size,
                    size_t samples)
{
    if (!codec->decoder->conceal || !samples) return false;
    const xiaotai_media_resample_t plan = xiaotai_media_resample_plan(codec->format.rate_hz, p->rate);
    size_t capacity;
    int16_t *target = decode_target(p, plan, &capacity);
    if (!target || samples > capacity ||
        codec->decoder->conceal(codec->state, next, next_size, target, samples) != XIAOTAI_OK ||
        !append(p, codec->format.rate_hz, plan, target, samples, false)) return false;
    p->counters.rx_concealed_ms += (uint32_t)(samples * 1000U / codec->format.rate_hz);
    return true;
}

static bool fill(media_playout_t *p, const media_playout_request_t *r)
{
    xiaotai_downlink_packet_t packet;
    uint16_t stream, original_samples;
    uint8_t slot;
    const bool held = p->held;
    if (held) {
        packet = p->held_packet;
        stream = p->held_stream;
        slot = p->held_slot;
        original_samples = p->held_samples;
    } else if (!p->ring || !media_downlink_pop_tagged(p->ring, p->packet, p->packet_capacity,
                                                    &packet, &stream, &slot, &original_samples)) {
        return false;
    }
    p->held = false;
    if (packet.generation != r->generation || !r->current(r->generation, r->epoch)) return false;
    const uint8_t tagged = slot;
    const bool first_piece = !(slot & MEDIA_DOWNLINK_CONTINUATION);
    slot &= (uint8_t)~MEDIA_DOWNLINK_CONTINUATION;
    media_rx_format_t codec;
    if (!(slot ? media_rx_format_at(slot, &codec) : media_rx_format_select(&r->session->downlink, &slot, &codec))) {
        ++p->counters.rx_decode_failed;
        return true;
    }
    const bool restart = !p->have_stream || p->stream != stream || p->format_slot != slot;
    if (!held && (first_piece || restart)) {
        const uint32_t raw_us = original_samples ? (uint32_t)((uint64_t)original_samples * 1000000U / codec.format.rate_hz) :
                                                   packet.duration_us;
        media_pacer_arrival(&p->pacer, packet.timestamp_ms, packet.received_us, raw_us, restart);
    }
    if (restart) {
        p->have_stream = false;
        if (codec.decoder->reset && codec.decoder->reset(codec.state) != XIAOTAI_OK) {
            ++p->counters.rx_decode_failed;
            return true;
        }
    }
    const uint32_t expect = codec.decoder->packet_samples(p->packet, packet.size, codec.format.rate_hz);
    if (!restart && first_piece && expect && codec.decoder->conceal) {
        /* A short hole in the sender's time is a lost packet: fill it, one
         * packet's length at a time, the last piece with the help of this
         * packet. A long one is silence the sender never sent. */
        const int32_t gap_ms = (int32_t)(packet.timestamp_ms - p->next_stamp);
        if (gap_ms > 0 && gap_ms <= p->conceal_gap_ms) {
            const uint32_t rate = codec.format.rate_hz;
            const size_t missing = (size_t)((uint32_t)gap_ms * rate / 1000U) / (rate / 400U) * (rate / 400U);
            const size_t piece = min(missing, expect);
            const bool last = missing == piece;
            if (piece && conceal(p, &codec, last ? p->packet : NULL, last ? packet.size : 0, piece)) {
                p->next_stamp += (uint32_t)(piece * 1000U / rate);
                p->held = true;
                p->held_packet = packet;
                p->held_stream = stream;
                p->held_slot = tagged;
                p->held_samples = original_samples;
                return true;
            }
        }
    }
    const bool trace = media_trace_active() && !media_trace_rx_ingress();
    const xiaotai_media_resample_t plan = xiaotai_media_resample_plan(codec.format.rate_hz, p->rate);
    size_t capacity;
    int16_t *decoded = decode_target(p, plan, &capacity);
    size_t n = 0;
    if (!expect || !decoded || expect > capacity ||
        codec.decoder->decode(codec.state, p->packet, packet.size, decoded, capacity, &n) != XIAOTAI_OK ||
        n != expect) {
        ++p->counters.rx_decode_failed;
        if (trace) media_trace_frame(false, (uint32_t)(xiaotai_time_us() / 1000), p->packet, packet.size,
                                     0, 0, 11, packet.timestamp_ms, (uint16_t)(packet.duration_us / 1000));
        return true;
    }
    if (trace) {
        media_trace_frame(false, (uint32_t)(xiaotai_time_us() / 1000), p->packet, packet.size,
                          media_trace_db(decoded, n), 0, 3, packet.timestamp_ms, (uint16_t)(packet.duration_us / 1000));
    }
    if (!append(p, codec.format.rate_hz, plan, decoded, n, restart)) {
        ++p->counters.rx_decode_failed;
        p->have_stream = false;
        return true;
    }
    p->have_stream = true;
    p->stream = stream;
    p->format_slot = slot;
    p->last_samples = expect;
    p->next_stamp = packet.timestamp_ms + (uint32_t)(n * 1000U / codec.format.rate_hz);
    p->dry_ms = 0;
    ++p->counters.rx_played;
    return true;
}

/* Nothing came in time and the speaker is about to run dry: one packet's
 * length of made-up sound, for a bounded time. */
static bool fill_dry(media_playout_t *p)
{
    media_rx_format_t codec;
    if (!p->have_stream || !p->last_samples || p->dry_ms >= p->conceal_dry_ms ||
        !media_rx_format_at(p->format_slot, &codec)) return false;
    const uint32_t ms = p->last_samples * 1000U / codec.format.rate_hz;
    if (!ms || p->dry_ms + ms > p->conceal_dry_ms || !conceal(p, &codec, NULL, 0, p->last_samples)) return false;
    p->dry_ms = (uint16_t)(p->dry_ms + ms);
    p->next_stamp += ms;
    p->tail = p->pending;           /* nothing was pending before it */
    return true;
}

/* Renders one block and says how loud the voice in it is. A hole in the middle of what is said is filled
 * with sound that counts as voice; the tail that is made up when nothing comes any more does not: it
 * fades what has been said, and nobody speaks in it. */
static uint32_t render(media_playout_t *p, size_t used, size_t frames)
{
    if (used == frames) {
        memcpy(p->mono, p->pending_pcm, used * sizeof(*p->mono));
    } else {
        /* One block consumes a slightly different number of input samples;
         * use endpoint-preserving interpolation instead of repeating chunks. */
        for (size_t i = 0; i < frames; ++i) {
            const size_t q = frames == 1U ? 0 : i * (used - 1U) * 256U / (frames - 1U);
            const size_t a = q >> 8;
            const size_t b = a + (a + 1U < used);
            const int32_t f = (int32_t)(q & 255U);
            p->mono[i] = (int16_t)((p->pending_pcm[a] * (256 - f) + p->pending_pcm[b] * f) / 256);
        }
    }
    const size_t ramp = p->rate / FADE_RATE_DIVISOR;
    const size_t remaining = p->fade_remaining;
    const size_t faded = min(frames, remaining);
    for (size_t i = 0; i < faded; ++i)
        p->mono[i] = (int16_t)((int32_t)p->mono[i] * (int32_t)(ramp - remaining + i + 1U) /
                              (int32_t)ramp);
    /* Timed playout can emit a short first block without ending the ramp. */
    p->fade_remaining = p->policy == MEDIA_PACER_TIMED ? (uint8_t)(remaining - faded) : 0;
    p->pending -= used;
    if (!p->pending) p->first_pending_us = 0;
    memmove(p->pending_pcm, p->pending_pcm + used, p->pending * sizeof(*p->pending_pcm));
    const size_t tail = min(p->tail, used), from = tail * frames / used;
    p->tail -= tail;
    return media_loudness_peak(p->mono + from, frames - from);
}

uint32_t media_playout_pass(media_playout_t *p, const media_playout_request_t *r, bool *drained)
{
    *drained = !p;
    if (!p) return XIAOTAI_WAIT_FOREVER;
    const int64_t now = xiaotai_time_us();
    if (!p->session_begin_us) p->session_begin_us = now;
    if (!p->heard_network && !p->warned_no_stream && !r->draining &&
        now - p->session_begin_us >= INT64_C(5000000)) {
        p->warned_no_stream = true;
        xiaotai_eventlog_warn("media", "playout.no_stream", r->generation, 5000);
    }
    if (p->next_us > now) return (uint32_t)((p->next_us - now + 999) / 1000);
    const uint32_t output_ms = p->buffered_until > now ? (uint32_t)((p->buffered_until - now) / 1000) : 0;
    const uint32_t block_ms = (uint32_t)((p->block * 1000U + p->rate - 1U) / p->rate);
    const size_t wanted = p->block + (p->block + 39U) / 40U;
    if (p->ring && p->pending >= wanted && xiaotai_downlink_count(p->ring)) ++p->counters.rx_pcm_waits;
    for (unsigned i = 0; i < 8 && p->pending < wanted && fill(p, r); ++i) {}
    if (p->pacer.started && !p->pending && !p->held && !output_ms && !r->draining && p->ring &&
        !xiaotai_downlink_count(p->ring)) (void)fill_dry(p);
    const uint32_t encoded_ms = p->ring ? xiaotai_downlink_queued_us(p->ring) / 1000U : 0;
    const uint32_t packets = p->ring ? (uint32_t)xiaotai_downlink_count(p->ring) : 0;
    const bool ring_empty = packets == 0;
    const uint32_t queued_ms = encoded_ms + (uint32_t)(p->pending * 1000U / p->rate) + output_ms;
    media_pacer_tick(&p->pacer, now, queued_ms >= block_ms, false);
    p->counters.target_ms = (uint16_t)p->pacer.target_ms;
    p->counters.queued_ms = queued_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)queued_ms;
    p->counters.jitter_ms = (uint16_t)(p->pacer.jitter_q8 / 256U);
    const bool started = p->pacer.started;
    const bool play = p->pending && media_pacer_start(&p->pacer, queued_ms ? queued_ms : 1U,
                      packets + p->pacer.prefill_packets, now, r->armed_audio && !p->played_activation, r->draining);
    if (play && !started) {
        ++p->counters.playout_starts;
        p->played_activation = true;
    }
    size_t used = 0, frames = 0;
    bool flush = false;
    if (play) {
        if (p->pending >= p->block) {
            int32_t adjustment;
            if (p->policy == MEDIA_PACER_TIMED) {
                adjustment = media_pacer_timed_delta(&p->pacer, queued_ms, now, p->pending, p->block);
            } else {
                const int speed = media_pacer_speed(&p->pacer, queued_ms, p->latency_ms, block_ms);
                p->speed_credit += (int32_t)p->block * speed;
                adjustment = p->speed_credit / 1000;
                p->speed_credit %= 1000;
            }
            const int32_t n = (int32_t)p->block + adjustment;
            used = min(p->pending, n > 0 ? (size_t)n : 1U);
            frames = p->block;
            p->counters.fast_blocks += used > frames;
            p->counters.slow_blocks += used < frames;
        } else {
            const uint32_t nominal = r->session->downlink.packet_ms;
            if (r->draining || media_pacer_short_tail(&p->pacer, now, p->first_pending_us,
                                                       media_rx_last_us(), output_ms, block_ms, nominal)) {
                used = frames = p->pending;
            }
        }
    } else if (started && !p->pending && ring_empty && !output_ms && !r->draining && !r->mixing &&
               media_pacer_on_empty(&p->pacer, now)) {
        if (p->policy == MEDIA_PACER_ADAPTIVE) ++p->counters.rx_underruns;
        if (media_trace_active() && !media_trace_rx_ingress())
            media_trace_frame(false, (uint32_t)(now / 1000), NULL, 0, 0, 0, 7, 0, 0);
        restart_fade(p);
    }
    /* Drain has closed admission. A writer publishes to the ring before dropping
     * its writer count, so sample the ring only after confirming no writers remain. */
    if (!frames && r->draining && !p->pending && !media_rx_writers() &&
        !p->held && (!p->ring || !xiaotai_downlink_count(p->ring))) {
        if (!p->seen_drain) {
            p->flush_remaining = p->part->flush_frames ? p->part->flush_frames(p->part->self) : 0;
            p->seen_drain = true;
        }
        flush = p->flush_remaining != 0;
        frames = min(p->block, p->flush_remaining);
        *drained = !flush && (p->part->flush_frames || !output_ms);
    }
    if (!frames && r->mixing) frames = p->block;
    if (!frames && !r->draining && p->policy == MEDIA_PACER_FIXED) frames = p->block;
    if (frames) {
        /* The voice alone, before a sound is mixed in: whoever moves a mouth with it follows the voice. */
        uint32_t voice = 0;
        if (used) voice = render(p, used, frames);
        else memset(p->mono, 0, frames * sizeof(*p->mono));
        const uint32_t mixed = r->mixing && r->mix && !flush ? r->mix(p->mono, frames) : 0;
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < p->channels; ++ch) {
                const bool copy = p->channels == 1 || p->part->desc.roles[ch] != XIAOTAI_CHANNEL_UNUSED;
                p->wire[i * p->channels + ch] = copy ? p->mono[i] : 0;
            }
        }
        xiaotai_err_t written = XIAOTAI_ERR_INVALID_STATE;
        const int64_t began = xiaotai_time_us();
        if (r->current(r->generation, r->epoch)) {
            written = p->part->write(p->part->self, p->wire, frames * p->channels * sizeof(*p->wire));
        }
        if (mixed && r->mixed_written) r->mixed_written(mixed, written);
        const int64_t duration = (int64_t)frames * 1000000 / p->rate;
        const int64_t finished = xiaotai_time_us();
        if (finished - began > duration) ++p->counters.slow_writes;
        if (written == XIAOTAI_OK) {
            if (used) {
                media_pacer_written(&p->pacer, finished);
                p->heard_network = true;
            }
            if (!flush) xiaotai_media_meter_add(&p->meter, p->mono, frames, 1);
            if (r->far_end) r->far_end(p->mono, frames, p->rate);
            p->counters.pcm_written += (uint32_t)used;
            const int64_t previous = p->buffered_until > finished ? p->buffered_until : finished;
            p->buffered_until = finished + (int64_t)min((size_t)(previous - finished + duration), p->latency_ms * 1000U);
            if (used) p->sound_until = p->buffered_until;
            media_loudness_playout(voice, p->buffered_until - duration, duration, finished);
            if (flush) {
                p->flush_remaining -= frames;
                *drained = !p->flush_remaining;
            }
        } else {
            ++p->counters.write_failed;
            p->counters.pcm_discarded += (uint32_t)used;
            p->buffered_until = p->sound_until = finished;
            restart_fade(p);
        }
        p->next_us = r->capture_clock || (p->part->info.flags & XIAOTAI_AUDIO_OUTPUT_PACED) ? finished : began + duration;
    }
    p->counters.pcm_pending = (uint32_t)p->pending;
    publish(p);
    if (frames && ++p->blocks % 50U == 0) {
        media_diag_playout_window(&p->counters, p->meter.peak, p->meter.near_full, p->meter.step_max, p->resample_clips);
        xiaotai_media_meter_clear_window(&p->meter);
        p->resample_clips = 0;
    }
    if (*drained) return XIAOTAI_WAIT_FOREVER;
    const int64_t after = xiaotai_time_us();
    if (p->next_us > after) return (uint32_t)((p->next_us - after + 999) / 1000);
    if (frames) return 0;
    if (p->pending || !ring_empty || output_ms || r->draining) return block_ms;
    const uint32_t idle = media_pacer_empty_wait(&p->pacer, after);
    if (!p->heard_network && !p->warned_no_stream && !r->draining) {
        const int64_t remains = INT64_C(5000000) - (after - p->session_begin_us);
        return (uint32_t)min(idle, remains > 0 ? (size_t)((remains + 999) / 1000) : 0);
    }
    return idle;
}

int64_t media_playout_heard_us(const media_playout_t *p)
{
    if (!p) return 0;
    const bool held = p->pending || p->held || (p->ring && xiaotai_downlink_count(p->ring));
    return held ? INT64_MAX : p->sound_until;
}

void media_playout_stats(media_playout_t *p, xiaotai_media_stats_t *s)
{
    if (!p) return;
    xiaotai_critical_enter(&p->lock);
    const xiaotai_media_stats_t ready = p->snapshot;
    xiaotai_critical_exit(&p->lock);
    s->rx_played = ready.rx_played;
    s->rx_decode_failed = ready.rx_decode_failed;
    s->rx_pcm_waits = ready.rx_pcm_waits;
    s->rx_underruns = ready.rx_underruns;
    s->rx_concealed_ms = ready.rx_concealed_ms;
    s->target_ms = ready.target_ms;
    s->queued_ms = ready.queued_ms;
    s->jitter_ms = ready.jitter_ms;
    s->playout_starts = ready.playout_starts;
    s->fast_blocks = ready.fast_blocks;
    s->slow_blocks = ready.slow_blocks;
    s->write_failed = ready.write_failed;
    s->slow_writes = ready.slow_writes;
    s->pcm_decoded = ready.pcm_decoded;
    s->pcm_written = ready.pcm_written;
    s->pcm_discarded = ready.pcm_discarded;
    s->pcm_pending = ready.pcm_pending;
    s->pcm_out_hash = ready.pcm_out_hash;
}
