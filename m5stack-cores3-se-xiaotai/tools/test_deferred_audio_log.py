"""Real mailbox code: bounded publishers, slow output, concurrency and wrapping."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_voice_reliability import function

root = Path(__file__).resolve().parents[1]
component = root / "components/starter_log"
for path, name in [
    ("components/starter_media/src/starter_aec.c", "starter_aec_submit_capture"),
    ("components/starter_tirtc/src/starter_tirtc.c", "on_audio"),
]:
    body = function(path, name)
    assert "starter_log_record(" in body and "ESP_LOG" not in body
sink = function("components/starter_media/src/starter_media.c", "audio_sink_task")
assert '"JB target=' not in sink and "STARTER_LOG_PLAYOUT" in sink
ring = function("components/starter_product/src/starter_product.c", "call_ring_task")
assert "starter_log_flush_pending();" in ring
assert ring.index("starter_log_flush_pending();") < ring.index("vTaskDelay(pdMS_TO_TICKS(80))")

code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include "starter_log.c"
static atomic_uint clock_ms;
static atomic_bool block_output, output_entered, release_output;
static unsigned log_calls;
static char last_log[768];
int64_t esp_timer_get_time(void) { return (int64_t)atomic_load(&clock_ms)*1000; }
void logger(const char *tag, const char *format, ...) {
    (void)tag;
    va_list args; va_start(args,format);
    vsnprintf(last_log,sizeof(last_log),format,args); va_end(args);
    ++log_calls;
    if (atomic_load(&block_output)) {
        atomic_store(&output_entered,true);
        while (!atomic_load(&release_output)) sched_yield();
    }
}
static void publish(unsigned n) {
    uint32_t v[STARTER_LOG_VALUES];
    for (unsigned i=0;i<STARTER_LOG_VALUES;++i) v[i]=n+i;
    starter_log_record(STARTER_LOG_PLAYOUT,v);
}
static void *drain(void *unused) {
    (void)unused; starter_log_flush_pending(); return NULL;
}
static atomic_uint finished;
static void *writer(void *arg) {
    uint32_t offset=(uint32_t)(uintptr_t)arg;
    for (uint32_t i=1;i<=100000;++i) publish(offset+i);
    atomic_fetch_add(&finished,1); return NULL;
}
int main(void) {
    starter_log_flush_pending(); assert(log_calls==0);
    starter_log_record((starter_log_topic_t)-1,NULL);
    starter_log_record(STARTER_LOG_TOPIC_COUNT,NULL);
    starter_log_record(STARTER_LOG_PLAYOUT,NULL);
    atomic_store(&clock_ms,100);
    publish(1); assert(log_calls==0);
    starter_log_flush_pending(); assert(log_calls==1);
    assert(strstr(last_log,"gen=16 epoch=17 deferred=1 missed=0 age_ms=0"));
    publish(2); atomic_store(&clock_ms,5099);
    starter_log_flush_pending(); assert(log_calls==1);
    atomic_store(&clock_ms,5100); starter_log_flush_pending(); assert(log_calls==2);

    /* Freeze the output sink, then publish 10000 updates. No audio producer
     * acquires the output lock or waits for the reader; newest details survive. */
    publish(3); atomic_store(&clock_ms,10100);
    atomic_store(&block_output,true);
    pthread_t reader; assert(!pthread_create(&reader,NULL,drain,NULL));
    while (!atomic_load(&output_entered)) sched_yield();
    for (unsigned n=4;n<10004;++n) publish(n);
    atomic_store(&release_output,true); assert(!pthread_join(reader,NULL));
    atomic_store(&block_output,false);
    atomic_store(&clock_ms,15100); starter_log_flush_pending();
    assert(strstr(last_log,"target=10003 q=10004"));
    log_snapshot_t s;
    assert(read_pending(&s_pending[STARTER_LOG_PLAYOUT],&s) && s.count==10003);

    /* A concurrent writer does not spin or silently discard contention. */
    atomic_store(&s_pending[STARTER_LOG_RX_REJECT].sequence,1);
    starter_log_record(STARTER_LOG_RX_REJECT,(uint32_t[STARTER_LOG_VALUES]){9});
    assert(atomic_load(&s_pending[STARTER_LOG_RX_REJECT].missed)==1);
    assert(!read_pending(&s_pending[STARTER_LOG_RX_REJECT],&s));
    atomic_store(&s_pending[STARTER_LOG_RX_REJECT].sequence,2);
    starter_log_flush_pending(); assert(strstr(last_log,"missed=1"));

    log_reader_t *r=&s_readers[STARTER_LOG_PLAYOUT];
    r->last_ms=UINT32_MAX-1000;
    publish(20000); unsigned before=log_calls;
    atomic_store(&clock_ms,3998); starter_log_flush_pending(); assert(log_calls==before);
    atomic_store(&clock_ms,3999); starter_log_flush_pending(); assert(log_calls==before+1);
    atomic_store(&s_pending[STARTER_LOG_PLAYOUT].sequence,UINT32_MAX-1);
    r->sequence=UINT32_MAX-1;
    publish(30000); atomic_store(&clock_ms,8999); starter_log_flush_pending();
    assert(r->sequence==0 && strstr(last_log,"target=30000"));

    pthread_t a,b;
    assert(!pthread_create(&a,NULL,writer,(void *)(uintptr_t)40000));
    assert(!pthread_create(&b,NULL,writer,(void *)(uintptr_t)200000));
    unsigned reads=0;
    while (atomic_load(&finished)!=2) {
        if (read_pending(&s_pending[STARTER_LOG_PLAYOUT],&s)) {
            for (unsigned i=0;i<STARTER_LOG_VALUES;++i) assert(s.values[i]==s.values[0]+i);
            ++reads;
        }
    }
    assert(!pthread_join(a,NULL)); assert(!pthread_join(b,NULL));
    assert(read_pending(&s_pending[STARTER_LOG_PLAYOUT],&s));
    assert(s.count+s.missed==210005);
    printf("PASS: blocked sink, 210005 publishes, multiwriter snapshots (%u reads), wraps, 384-byte PSRAM budget\n",reads);
}
'''
with tempfile.TemporaryDirectory() as folder:
    base = Path(folder)
    (base / "esp_attr.h").write_text("#define EXT_RAM_BSS_ATTR\n")
    (base / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
    (base / "esp_log.h").write_text("void logger(const char *, const char *, ...);\n" +
        "\n".join(f"#define ESP_LOG{x}(tag, ...) logger(tag, __VA_ARGS__)" for x in "IEW") + "\n")
    (base / "test.c").write_text(code)
    exe = base / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-pthread", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(base),
        "-I", str(component), "-I", str(component / "include"), str(base / "test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
