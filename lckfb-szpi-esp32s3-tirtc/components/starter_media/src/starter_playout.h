#ifndef STARTER_PLAYOUT_H
#define STARTER_PLAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Encoded frames stay in the media owner's PSRAM pool. This FIFO holds only
 * their slot IDs and timing. One playback task owns all fields; no allocation. */
#define STARTER_PLAYOUT_CAPACITY 32U
#define STARTER_PLAYOUT_INITIAL_MS 200U
#define STARTER_PLAYOUT_MIN_MS 60U
#define STARTER_PLAYOUT_MAX_MS 500U
#define STARTER_PLAYOUT_IDLE_MS 120U
#define STARTER_PLAYOUT_STABLE_MS 10000U
#define STARTER_PLAYOUT_DMA_ESTIMATE_US 90000U /* 6 x 240 / 16 kHz; not a DMA measurement */
#define STARTER_PLAYOUT_RATE_WARMUP_US 2000000U

_Static_assert(STARTER_PLAYOUT_MIN_MS <= STARTER_PLAYOUT_INITIAL_MS &&
               STARTER_PLAYOUT_INITIAL_MS <= STARTER_PLAYOUT_MAX_MS,
               "invalid playout target range");

typedef struct {
    uint64_t received_us;
    uint32_t duration_us;
    uint8_t slot;
} starter_playout_entry_t;

typedef struct {
    starter_playout_entry_t entries[STARTER_PLAYOUT_CAPACITY];
    unsigned head, count;
    uint32_t queued_us, target_ms, jitter_us;
    uint64_t last_received_us, last_write_us, last_adjust_us;
    uint64_t stable_since_us;
    uint32_t last_timestamp_ms, last_duration_us;
    uint32_t late_bursts, untimed_pairs, timestamp_breaks;
    uint32_t gap_max_us, residence_max_us, starts;
    uint8_t last_stream;
    bool buffering, have_arrival;
    uint64_t rate_start_us, output_updated_us;
    uint32_t output_estimate_us;
    int rate_mode; /* -1: consume 159, 0: 160, +1: 161 samples per 20 ms */
    uint8_t timed_run;
} starter_playout_t;

void starter_playout_init(starter_playout_t *q);
bool starter_playout_push(starter_playout_t *q, uint8_t slot, uint32_t duration_us,
                          uint32_t timestamp_ms, uint8_t stream, uint64_t received_us);
/* A successful pop transfers slot ownership back to the caller. force is only
 * for explicit session/mute teardown, not latency reduction by discarding audio. */
bool starter_playout_pop(starter_playout_t *q, uint64_t now_us, bool force, uint8_t *slot);
void starter_playout_written(starter_playout_t *q, uint64_t now_us);
void starter_playout_idle(starter_playout_t *q, uint64_t now_us);
/* Control uses app PCM plus a bounded output-clock estimate. No I2S clock,
 * peer timestamps, packet order or capture state is changed. */
uint16_t starter_playout_quantum(starter_playout_t *q, size_t pending_samples, uint64_t now_us);
uint32_t starter_playout_output_estimate(const starter_playout_t *q, uint64_t now_us);
void starter_playout_output_written(starter_playout_t *q, uint64_t now_us, size_t samples);

/* Packet boundaries are independent of the 20 ms output quantum. A residual
 * fragment is retained across packets; end markers preserve packet accounting
 * even when a packet spans several writes or several packets share one write. */
#define STARTER_PLAYOUT_PCM_SAMPLES 160U /* 8 kHz, 20 ms, mono */
#define STARTER_PLAYOUT_PCM_CAPACITY (STARTER_PLAYOUT_PCM_SAMPLES + 1U)
#define STARTER_PLAYOUT_FADE_SAMPLES 40U /* 5 ms, only at a playback restart */
#define STARTER_PLAYOUT_TAIL_WAIT_US 60000U
#define STARTER_PLAYOUT_HASH_INITIAL UINT32_C(2166136261)
typedef struct {
    int16_t samples[STARTER_PLAYOUT_PCM_CAPACITY];
    uint8_t packet_ends[STARTER_PLAYOUT_PCM_CAPACITY];
    uint16_t count, limit;
    bool failed_packet;
} starter_playout_pcm_t;

void starter_playout_pcm_init(starter_playout_pcm_t *pcm);
size_t starter_playout_pcm_append(starter_playout_pcm_t *pcm,
                                  const int16_t *samples, size_t count);
/* Returns whole source packets successfully submitted, not number of chunks.
 * Failed writes are NOT retried: the codec API cannot report partial progress. */
unsigned starter_playout_pcm_commit(starter_playout_pcm_t *pcm, bool written);
bool starter_playout_tail_ready(uint64_t first_sample_us, uint64_t now_us);
/* FNV-1a over little-endian PCM samples; stage-order diagnostic, not a network
 * checksum or proof that DMA/DAC physically played the data. Excludes padding. */
uint32_t starter_playout_pcm_hash(uint32_t hash, const int16_t *samples, size_t count);
/* Full quanta render exactly 20 ms. Short tails retain their actual length.
 * Endpoint-preserving interpolation changes duration slightly, not loudness;
 * this is not pitch-preserving time stretching. Input/packet markers stay intact. */
size_t starter_playout_pcm_render(const starter_playout_pcm_t *pcm, int16_t *output,
                                  size_t capacity, bool tail, uint16_t *fade_remaining);

#endif
