// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { DVP_BENCH_VSYNC, DVP_BENCH_PCLK, DVP_BENCH_HREF, DVP_BENCH_SIGNALS };
enum { DVP_BENCH_FIRST_US = 500000, DVP_BENCH_WINDOW_US = 200000,
       DVP_BENCH_BURST_US = 1000, DVP_BENCH_BURSTS = 16, DVP_BENCH_BURST_SAMPLES = 4096 };
typedef enum { DVP_BENCH_WAIT, DVP_BENCH_SAMPLE, DVP_BENCH_REPORT } dvp_bench_step_t;

typedef struct {
    int64_t opened_us, began_us, ended_us;
    uint32_t samples, sampled_us, largest_gap_us, changes[DVP_BENCH_SIGNALS];
    uint8_t bursts, highs, lows, last;
    bool done;
} dvp_bench_window_t;

static inline dvp_bench_step_t dvp_bench_step(dvp_bench_window_t *window, int64_t now, bool first_frame)
{
    if (window->done) return DVP_BENCH_WAIT;
    if (first_frame || (window->bursts && (window->bursts == DVP_BENCH_BURSTS ||
                        now - window->began_us >= DVP_BENCH_WINDOW_US))) {
        window->done = true;
        return window->bursts ? DVP_BENCH_REPORT : DVP_BENCH_WAIT;
    }
    return now - window->opened_us >= DVP_BENCH_FIRST_US ? DVP_BENCH_SAMPLE : DVP_BENCH_WAIT;
}

static inline void dvp_bench_burst_start(dvp_bench_window_t *window, int64_t now)
{
    if (!window->bursts) window->began_us = now;
    else {
        const uint32_t gap = (uint32_t)(now - window->ended_us);
        if (gap > window->largest_gap_us) window->largest_gap_us = gap;
    }
    ++window->bursts;
}

static inline void dvp_bench_sample(dvp_bench_window_t *window, uint8_t levels, bool first_sample)
{
    const uint8_t changed = first_sample ? 0 : (uint8_t)(levels ^ window->last);
    for (unsigned i = 0; i < DVP_BENCH_SIGNALS; ++i)
        window->changes[i] += (changed >> i) & 1U;
    window->last = levels;
    window->highs |= levels;
    window->lows |= (uint8_t)(~levels & ((1U << DVP_BENCH_SIGNALS) - 1U));
    ++window->samples;
}

static inline void dvp_bench_burst_end(dvp_bench_window_t *window, int64_t began, int64_t ended)
{
    window->sampled_us += (uint32_t)(ended - began);
    window->ended_us = ended;
}
