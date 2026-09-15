#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_playout_controller.h"

/* One audio-sink owner, no locks/allocations/I2S in this module. Store the
 * complete object in PSRAM. Fixed capacity is not the adaptive delay target. */
#define P4_PLAYOUT_RATE 8000U
#define P4_PLAYOUT_CHUNK 120U /* 15 ms: one complete 240-frame I2S DMA block. */
#define P4_PLAYOUT_CAPACITY 8000U
#define P4_PLAYOUT_TAIL_MS 120U
#define P4_PLAYOUT_DMA_MS 90U /* Six 240-frame descriptors at 16 kHz. */
#define P4_PLAYOUT_FADE_SAMPLES 40U /* 5 ms at 8 kHz, only on a restart */

typedef struct {
    size_t samples, consumed;
    unsigned next_phase;
    int16_t rate_permille;
    uint16_t next_fade_remaining;
    bool restart;
} p4_playout_block_t;

typedef struct {
    int16_t pcm[P4_PLAYOUT_CAPACITY];
    uint8_t packet_ends[(P4_PLAYOUT_CAPACITY + 7U) / 8U];
    size_t head, count;
    unsigned phase;
    bool started, have_packet, packet_failed;
    bool restart_pending;
    uint16_t fade_remaining;
    uint32_t last_arrival_ms, output_until_ms, last_write_ms;
    bool output_valid;
    audio_playout_controller_t controller;
    audio_playout_window_t window;
    uint32_t empty_events, overflow_samples, slow_blocks, fast_blocks;
} p4_audio_playout_t;

void p4_audio_playout_init(p4_audio_playout_t *q, audio_playout_profile_t profile);
bool p4_audio_playout_push(p4_audio_playout_t *q, const int16_t *pcm, size_t samples,
                           uint32_t source_ms, uint32_t arrival_ms);
bool p4_audio_playout_prepare(p4_audio_playout_t *q, uint32_t now_ms,
                              int16_t out[P4_PLAYOUT_CHUNK], p4_playout_block_t *block);
/* Commit only after attempting I2S write. An error has an unknown partial-write
 * extent: account for it, do not replay the chunk and duplicate audible words. */
/* Returns completed network packets, preserving the public played counter. */
uint32_t p4_audio_playout_commit(p4_audio_playout_t *q, const p4_playout_block_t *block,
                             uint32_t now_ms, bool written);
void p4_audio_playout_update_window(p4_audio_playout_t *q);
/* Bounded estimate, not a measurement of the physical DAC queue. */
uint32_t p4_audio_playout_pending_ms(const p4_audio_playout_t *q, uint32_t now_ms);
