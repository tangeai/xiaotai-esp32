// SPDX-License-Identifier: MIT
#include <assert.h>

#include "dvp_bench_window.h"

static void burst(dvp_bench_window_t *w, int64_t now, uint8_t first, uint8_t next)
{
    assert(dvp_bench_step(w, now, false) == DVP_BENCH_SAMPLE);
    dvp_bench_burst_start(w, now);
    dvp_bench_sample(w, first, true);
    dvp_bench_sample(w, next, false);
    dvp_bench_burst_end(w, now, now + 100);
}

int main(void)
{
    const int64_t opened = 1000;
    const int64_t start = opened + DVP_BENCH_FIRST_US;
    dvp_bench_window_t w = {.opened_us = opened};
    assert(dvp_bench_step(&w, start - 1, false) == DVP_BENCH_WAIT && !w.done);
    assert(dvp_bench_step(&w, start - 1, true) == DVP_BENCH_WAIT && w.done);
    assert(dvp_bench_step(&w, start + 5000, false) == DVP_BENCH_WAIT && !w.samples);

    w = (dvp_bench_window_t){.opened_us = opened};
    burst(&w, start, 0, 3);
    /* Transitions between bursts are deliberately not represented as wire edges. */
    burst(&w, start + 5100, 4, 4);
    assert(w.changes[DVP_BENCH_VSYNC] == 1 && w.changes[DVP_BENCH_PCLK] == 1);
    assert(w.changes[DVP_BENCH_HREF] == 0 && w.highs == 7 && w.lows == 7);
    assert(w.samples == 4 && w.sampled_us == 200 && w.largest_gap_us == 5000);
    assert(dvp_bench_step(&w, start + 5300, true) == DVP_BENCH_REPORT && w.done);
    assert(dvp_bench_step(&w, start + 10000, true) == DVP_BENCH_WAIT);

    w = (dvp_bench_window_t){.opened_us = opened};
    burst(&w, start, 7, 7);
    assert(dvp_bench_step(&w, start + DVP_BENCH_WINDOW_US, false) == DVP_BENCH_REPORT);
    assert(w.bursts == 1U && w.changes[DVP_BENCH_VSYNC] == 0);

    w = (dvp_bench_window_t){.opened_us = opened};
    for (unsigned i = 0; i < DVP_BENCH_BURSTS; ++i) burst(&w, start + i * 200U, 0, 0);
    assert(dvp_bench_step(&w, start + DVP_BENCH_BURSTS * 200U, false) == DVP_BENCH_REPORT);
    assert(w.bursts == DVP_BENCH_BURSTS && w.done && w.samples == DVP_BENCH_BURSTS * 2U);
    return 0;
}
