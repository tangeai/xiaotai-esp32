"""Execute P4 snapshot/backpressure code with a deliberately slow log sink."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = root / "components/starter_media/src"
media = (src / "starter_media.c").read_text(encoding="utf-8")


def typedef(name):
    end = media.index("} " + name + ";") + len(name) + 3
    return media[media.rfind("typedef struct {", 0, end):end]


types = "\n".join(typedef(n) for n in (
    "capture_post_diagnostics_t", "uplink_level_diagnostics_t",
    "playout_diagnostic_snapshot_t"))
state_end = media.index("} s_playout;") + len("} s_playout;")
state = media[media.rfind("static struct {", 0, state_end):state_end]
functions = media[media.index("static void playout_diagnostics("):
                  media.index("static void audio_diagnostic_task(")]
producer = functions[:functions.index("static void print_playout_diagnostics(")]
assert "ESP_LOG" not in producer and "printf(" not in producer
assert "xQueueSend(s_playout_log_queue, &snapshot, 0)" in producer
assert producer.index("if (!queued) return;") < producer.index("s_playout.log_ms = now_ms;")
consumer = functions[functions.index("static void print_playout_diagnostics("):]
assert "s_playout." not in consumer  # no races with the PCM owner
assert '"audio_diag",\n                            AUDIO_DIAGNOSTIC_STACK_BYTES, NULL, 2' in media
assert "EXT_RAM_BSS_ATTR static uint8_t s_playout_log_storage" in media

prefix = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "p4_audio_playout.h"
#include "p4_playback_meter.h"
typedef int starter_tirtc_mode_t;
#define P4_CAPTURE_HIGHPASS_ENABLED 1
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
static int s_audio_diag_lock;
static void *s_playout_log_queue;
static uint32_t s_playout_publish_max_us;
static atomic_uint s_audio_dropped, s_audio_write_failed, s_audio_rx_pool_full;
static atomic_uint s_aec_max_process_us, s_aec_deadline_misses, s_capture_queue_overflows;
static int64_t clock_us;
static unsigned prints;
static bool queue_full;
static const char *TAG = "test";
static int64_t esp_timer_get_time(void) { return clock_us; }
static unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 3000; }
static void log_slow(const char *tag, const char *fmt, ...) {
    (void)tag;
    va_list args; va_start(args, fmt);
    char line[512]; int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    assert(len > 0 && (size_t)len < sizeof(line));
    clock_us += 20000; ++prints; /* UART/another logger can be slow. */
}
#define ESP_LOGI log_slow
'''
stub = r'''
static playout_diagnostic_snapshot_t queued;
static capture_post_diagnostics_t s_capture_post_diag;
static uplink_level_diagnostics_t s_uplink_level;
static int xQueueSend(void *q, const void *item, unsigned wait) {
    (void)q; assert(wait == 0); clock_us += 15;
    if (queue_full) return 0;
    memcpy(&queued, item, sizeof(queued)); queue_full = true; return pdTRUE;
}
'''
test = r'''
int main(void) {
    s_playout.have_stream = true; s_playout.generation = 7;
    p4_audio_playout_init(&s_playout.queue, AUDIO_PLAYOUT_PROFILE_JITTER_SAFE);
    s_playout.received_samples = 80000; s_playout.consumed_samples = 79200;
    s_playout.queue.count = 800; s_playout.late_max_ms = 35;
    int16_t levels[]={-32768,32767,0,1000};
    p4_playback_meter_add(&s_playout.levels[0],levels,4,1);
    s_capture_post_diag.frames = 32; s_capture_post_diag.at_ms = 9990;
    s_uplink_level.frames = 50; s_uplink_level.at_ms = 9995;
    clock_us = 10000000;
    playout_diagnostics(10000);
    assert(prints == 0 && clock_us == 10000015);
    assert(queued.received == 80000 && queued.consumed == 79200);
    assert(queued.buffered_ms == 100 && queued.late_max_ms == 35);
    assert(queued.levels[0].samples==4 && queued.levels[0].peak==32768);
    assert(queued.levels[0].near_full==2 && queued.levels[0].step_max==65535);
    assert(s_playout.levels[0].samples==0 && s_playout.levels[0].last==1000);
    assert(s_playout.received_samples == 0 && s_playout.log_ms == 10000);
    assert(s_playout.queue.count == 800); /* diagnostics cannot consume PCM */

    /* Congested logging keeps the full interval, including error deltas. */
    s_audio_dropped = 3; s_audio_write_failed = 1;
    s_playout.received_samples = 16000; s_playout.late_max_ms = 23;
    clock_us = 12000000; playout_diagnostics(12000);
    assert(prints == 0 && s_playout.log_ms == 10000);
    assert(s_playout.received_samples == 16000 && s_playout.late_max_ms == 23);
    assert(s_playout.rx_dropped_at_log == 0 && s_playout.write_failed_at_log == 0);
    assert(queued.received == 80000); /* unread snapshot is not overwritten */
    /* Formatting only reads the immutable copy, not the next live session. */
    s_playout.generation = 8;
    print_playout_diagnostics(&queued);
    assert(prints == 5 && clock_us == 12100015 && queued.generation == 7);
    queue_full = false;
    clock_us = 12090000; playout_diagnostics(12090);
    assert(queued.dt_ms == 2090 && queued.received == 16000);
    assert(queued.dropped == 3 && queued.write_failures == 1 && queued.late_max_ms == 23);
    assert(queued.generation == 8 && queued.publish_max_us == 15);
    assert(s_playout.received_samples == 0 && s_playout.log_ms == 12090);
    p4_playback_meter_t m={0};
    int16_t stereo[]={1,-30000,2,-30000,3,-30000};
    p4_playback_meter_add(&m,stereo,3,2);
    assert(m.samples==3 && m.peak==3 && m.step_max==1 && !m.near_full);
    p4_playback_meter_clear_window(&m);
    p4_playback_meter_add(&m,levels+2,1,1);
    assert(m.samples==1 && m.step_max==3); /* cross-window adjacency */
    printf("snapshot=%zu bytes; producer=15 us; simulated UART=100000 us\n", sizeof(queued));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-audio-diag-") as tmp:
    p = Path(tmp)
    (p / "test.c").write_text(prefix + types + state + stub + functions + test, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(src), str(p / "test.c"),
                    str(src / "p4_audio_playout.c"), str(src / "audio_playout_controller.c"),
                    "-o", str(p / "test")], check=True)
    subprocess.run([str(p / "test")], check=True)
print("PASS: nonblocking diagnostic publication, backpressure, immutable snapshot and error retention")
