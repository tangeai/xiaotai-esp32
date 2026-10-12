#!/usr/bin/env python3
"""Check the actual feed owner's idle-yield deadline without devices or IDF."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec.c").read_text()
start = source.index("static void afe_feed_yield(")
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
helper = source[start:end]
worker = source[source.index("static void afe_feed_worker("):source.index("static void starter_aec_release(")]
assert "afe_feed_yield(&last_yield);" in worker
assert "vTaskDelay(" not in worker
# 256 ms input depth: 8 x 512-sample MR feed blocks (D039: 4 x 1024).
assert "#define AFE_INPUT_DEPTH_SAMPLES 4096U" in source
assert "#define AFE_INPUT_SLOTS 8U" in source
assert "#define AFE_IDLE_YIELD_MS 250U" in source

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t TickType_t;
#define AFE_IDLE_YIELD_MS 250U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static TickType_t tick;
static unsigned delays;
static TickType_t xTaskGetTickCount(void) { return tick; }
static void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1);
    ++delays;
    tick += ticks;
}
''' + helper + r'''
static void run(TickType_t initial, TickType_t frame_ms)
{
    tick = initial;
    TickType_t last = tick, idle = tick;
    delays = 0;
    for (unsigned n = 0; n < 1000; ++n) {
        tick += frame_ms;
        unsigned before = delays;
        afe_feed_yield(&last);
        if (delays != before) {
            assert((TickType_t)(tick - idle) <= AFE_IDLE_YIELD_MS + frame_ms + 1);
            idle = tick;
        }
        assert((TickType_t)(tick - idle) <= AFE_IDLE_YIELD_MS + frame_ms + 1);
    }
    assert(delays > 0 && delays < 1000);
}
int main(void)
{
    for (unsigned ms = 32; ms <= 100; ++ms) {
        run(0, ms);
        run(UINT32_MAX - 3000, ms);
    }
    tick = 1000;
    TickType_t last = tick;
    delays = 0;
    tick += 249;
    afe_feed_yield(&last);
    assert(delays == 0 && last == 1000);
    ++tick;
    afe_feed_yield(&last);
    assert(delays == 1 && last == 1251);
    for (unsigned n = 0; n < 1000; ++n) afe_feed_yield(&last);
    assert(delays == 1);
    puts("PASS: actual feed-yield helper; 138 steady/wrap cases, deadline boundary, one tick, no per-frame delay or slot changes");
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-feed-yield-") as directory:
    test = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    test.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
