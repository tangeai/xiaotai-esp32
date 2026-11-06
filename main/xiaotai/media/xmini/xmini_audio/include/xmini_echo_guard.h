// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#define XMINI_ECHO_GUARD_UNITY 32768
#define XMINI_ECHO_GUARD_FAR_FRAMES 12

// Integer uplink echo suppressor for frames of at most 65536 samples.
// The caller owns the state and serialises all calls.
typedef struct {
    uint32_t far[XMINI_ECHO_GUARD_FAR_FRAMES]; // AC power of frames handed to the DAC
    uint32_t noise, erl_q8;
    uint16_t gain_q15, closed_q15, dt_q15;
    uint8_t far_next, hang, startup, mic_db;
    uint32_t closed_frames, dt_frames, open_frames;
} xmini_echo_guard_t;

void xmini_echo_guard_reset(xmini_echo_guard_t *g, int volume, int gain_db);
void xmini_echo_guard_levels(xmini_echo_guard_t *g, int volume, int gain_db);
void xmini_echo_guard_downlink(xmini_echo_guard_t *g, const int16_t *pcm, size_t n);
void xmini_echo_guard_uplink(xmini_echo_guard_t *g, int16_t *pcm, size_t n);
