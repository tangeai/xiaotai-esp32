#include "p4_audio_playout.h"
#include <string.h>

_Static_assert(P4_PLAYOUT_CAPACITY > (560U + 320U) * (P4_PLAYOUT_RATE / 1000U),
               "PCM capacity must exceed the largest legacy emergency watermark");
_Static_assert(P4_PLAYOUT_CHUNK * 2U == 240U,
               "Network output quantum must fill one 16 kHz DMA descriptor");

static uint32_t duration_ms(size_t samples)
{
    return (uint32_t)((samples * 1000U + P4_PLAYOUT_RATE - 1U) / P4_PLAYOUT_RATE);
}

void p4_audio_playout_init(p4_audio_playout_t *q, audio_playout_profile_t profile)
{
    /* Do not clear 16 KB of PCM on each session: head/count own validity. */
    q->head = q->count = q->phase = 0;
    q->started = q->have_packet = q->packet_failed = false;
    q->restart_pending = false;
    q->fade_remaining = 0;
    q->last_arrival_ms = q->output_until_ms = q->last_write_ms = 0;
    q->output_valid = false;
    q->empty_events = q->overflow_samples = q->slow_blocks = q->fast_blocks = 0;
    q->window = (audio_playout_window_t){0};
    audio_playout_controller_init(&q->controller, profile);
}

bool p4_audio_playout_push(p4_audio_playout_t *q, const int16_t *pcm, size_t samples,
                           uint32_t source_ms, uint32_t arrival_ms)
{
    if (!q || !pcm || !samples || samples > P4_PLAYOUT_CAPACITY) return false;
    uint32_t ms = duration_ms(samples);
    audio_playout_controller_observe_packet(&q->controller, source_ms, arrival_ms, ms);
    q->last_arrival_ms = arrival_ms;
    q->have_packet = true;
    q->window.received_ms += ms;
    if (samples > P4_PLAYOUT_CAPACITY - q->count) {
        /* A real capacity failure, not a latency shortcut. Preserve the old
         * audio order, reject this packet and expose the lost duration. */
        q->overflow_samples += (uint32_t)samples;
        q->window.hard_trim_ms += ms;
        return false;
    }
    size_t tail = (q->head + q->count) % P4_PLAYOUT_CAPACITY;
    size_t first = P4_PLAYOUT_CAPACITY - tail;
    if (first > samples) first = samples;
    memcpy(q->pcm + tail, pcm, first * sizeof(*pcm));
    memcpy(q->pcm, pcm + first, (samples - first) * sizeof(*pcm));
    /* A bit per sample costs 1000 PSRAM bytes and handles variable-length
     * packets without an unbounded metadata queue or changing counter units. */
    for (size_t i = 0; i < samples; ++i) {
        size_t at = (tail + i) % P4_PLAYOUT_CAPACITY;
        uint8_t mask = (uint8_t)(1U << (at % 8U));
        q->packet_ends[at / 8U] &= (uint8_t)~mask;
        if (i + 1U == samples) q->packet_ends[at / 8U] |= mask;
    }
    q->count += samples;
    return true;
}

uint32_t p4_audio_playout_pending_ms(const p4_audio_playout_t *q, uint32_t now_ms)
{
    if (!q || !q->output_valid || now_ms - q->last_write_ms >= P4_PLAYOUT_DMA_MS) return 0;
    int32_t pending = (int32_t)(q->output_until_ms - now_ms);
    return pending > 0 ? (uint32_t)pending : 0;
}

bool p4_audio_playout_prepare(p4_audio_playout_t *q, uint32_t now_ms,
                              int16_t out[P4_PLAYOUT_CHUNK], p4_playout_block_t *block)
{
    if (!q || !out || !block) return false;
    *block = (p4_playout_block_t){0};
    if (!q->count) {
        /* Empty software PCM is not yet an empty DAC. Keep history until
         * only one output block remains, leaving time to write the FIR tail
         * before DMA drains. Waiting 120 ms here would append the tail after
         * audible silence. I2S, not a second timer, clocks continuous PCM. */
        if (q->started && q->output_valid &&
            (now_ms - q->last_write_ms < duration_ms(P4_PLAYOUT_CHUNK) ||
             p4_audio_playout_pending_ms(q, now_ms) > duration_ms(P4_PLAYOUT_CHUNK))) return false;
        if (q->started) {
            /* This is a PCM deadline gap, not proof of packet loss. Without an
             * end-of-speech marker, an intentional remote pause is ambiguous. */
            q->empty_events++;
            q->window.underflows++;
            audio_playout_controller_note_underflow(&q->controller);
        }
        q->started = false;
        q->output_valid = false;
        return false;
    }
    audio_playout_decision_t decision;
    uint32_t pending_ms = p4_audio_playout_pending_ms(q, now_ms);
    audio_playout_controller_decide(&q->controller, duration_ms(q->count) + pending_ms,
                                     duration_ms(P4_PLAYOUT_CHUNK),
                                     q->started, &decision);
    bool tail = q->have_packet && now_ms - q->last_arrival_ms >= P4_PLAYOUT_TAIL_MS;
    if (!q->started) {
        /* Flush a short utterance too; waiting forever for a full prebuffer
         * swallows the final words of AI speech. No samples are invented. */
        if (duration_ms(q->count) < decision.prebuffer_ms && !tail) return false;
        q->started = true;
        q->restart_pending = true;
        q->fade_remaining = P4_PLAYOUT_FADE_SAMPLES;
        audio_playout_controller_note_playback_started(&q->controller);
    }
    size_t samples = q->count < P4_PLAYOUT_CHUNK ? q->count : P4_PLAYOUT_CHUNK;
    /* Keep enough DMA lead to join a short tail to its preceding PCM.
     * Waiting the full network tail timeout after DMA emptied creates an
     * audible gap even when every source sample arrived and was consumed. */
    uint32_t short_wait_ms = q->controller.packet_duration_ms + duration_ms(P4_PLAYOUT_CHUNK);
    if (short_wait_ms > P4_PLAYOUT_TAIL_MS) short_wait_ms = P4_PLAYOUT_TAIL_MS;
    bool short_due = q->have_packet && now_ms - q->last_arrival_ms >= short_wait_ms;
    if (samples < P4_PLAYOUT_CHUNK && !tail &&
        (!short_due || pending_ms > 2U * duration_ms(P4_PLAYOUT_CHUNK))) return false;
    int rate = samples == P4_PLAYOUT_CHUNK ? decision.rate_adjust_permille : 0;
    unsigned phase = q->phase;
    unsigned step = (unsigned)(1000 + rate);
    size_t last = phase + (samples - 1U) * step;
    size_t consumed = (phase + samples * step) / 1000U;
    size_t needed = last / 1000U + 1U + (last % 1000U != 0);
    if (needed > q->count || consumed > q->count) {
        /* A final sub-sample phase must not strand a complete tail frame. */
        rate = 0;
        step = 1000;
        phase = 0;
        consumed = samples;
    }
    uint16_t fade_remaining = q->fade_remaining;
    for (size_t i = 0; i < samples; ++i) {
        unsigned position = phase + (unsigned)i * step;
        size_t index = (q->head + position / 1000U) % P4_PLAYOUT_CAPACITY;
        unsigned fraction = position % 1000U;
        int32_t a = q->pcm[index];
        int32_t b = fraction ? q->pcm[(index + 1U) % P4_PLAYOUT_CAPACITY] : a;
        int32_t value = (a * (int32_t)(1000U - fraction) + b * (int32_t)fraction) / 1000;
        /* S3's short restart fade avoids a step after silence. Do not fade
         * each packet, change PCM order, or advance history on a lock retry. */
        if (fade_remaining) {
            --fade_remaining;
            value = value * (int32_t)(P4_PLAYOUT_FADE_SAMPLES - fade_remaining) /
                    (int32_t)P4_PLAYOUT_FADE_SAMPLES;
        }
        out[i] = (int16_t)value;
    }
    *block = (p4_playout_block_t){.samples = samples, .consumed = consumed,
        .next_phase = (phase + (unsigned)samples * step) % 1000U,
        .rate_permille = (int16_t)rate,
        .next_fade_remaining = fade_remaining, .restart = q->restart_pending};
    return true;
}

uint32_t p4_audio_playout_commit(p4_audio_playout_t *q, const p4_playout_block_t *block,
                             uint32_t now_ms, bool written)
{
    if (!q || !block || !block->samples || block->consumed > q->count) return 0;
    uint32_t packets = 0;
    for (size_t i = 0; i < block->consumed; ++i) {
        size_t at = (q->head + i) % P4_PLAYOUT_CAPACITY;
        if (!written) q->packet_failed = true;
        if (q->packet_ends[at / 8U] & (1U << (at % 8U))) {
            if (!q->packet_failed) ++packets;
            q->packet_failed = false;
        }
    }
    q->head = (q->head + block->consumed) % P4_PLAYOUT_CAPACITY;
    q->count -= block->consumed;
    q->phase = q->count ? block->next_phase : 0;
    if (written) {
        uint32_t pending = p4_audio_playout_pending_ms(q, now_ms) + duration_ms(block->samples);
        if (pending > P4_PLAYOUT_DMA_MS) pending = P4_PLAYOUT_DMA_MS;
        q->output_until_ms = now_ms + pending;
        q->last_write_ms = now_ms;
        q->output_valid = true;
        q->fade_remaining = block->next_fade_remaining;
        q->restart_pending = false;
        q->window.played_ms += duration_ms(block->samples);
        if (block->rate_permille < 0) q->slow_blocks++;
        if (block->rate_permille > 0) q->fast_blocks++;
    } else {
        q->output_valid = false;
        q->window.local_write_drop_ms += duration_ms(block->samples);
        /* Partial I2S progress is unknown: discard once, restart cleanly on
         * the next block, and keep the write failure visible. */
        q->fade_remaining = P4_PLAYOUT_FADE_SAMPLES;
        q->restart_pending = true;
    }
    return packets;
}

void p4_audio_playout_update_window(p4_audio_playout_t *q)
{
    if (!q) return;
    audio_playout_controller_update_window(&q->controller, &q->window);
    q->window = (audio_playout_window_t){0};
}
