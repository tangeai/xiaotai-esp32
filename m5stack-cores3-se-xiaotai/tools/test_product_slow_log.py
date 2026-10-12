"""Actual UI warning mailbox and worker drain, without hardware or serial IO."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_voice_reliability import function

root = Path(__file__).resolve().parents[1]
product = "components/starter_product/src/starter_product.c"
source = (root / product).read_text(encoding="utf-8")
face = (root / "components/starter_product/src/starter_product_s3_face.inc").read_text(encoding="utf-8")
assert 'ESP_LOGW(TAG, "face draw slow' not in face
assert '"UI refresh slow:' not in function(product, "product_tick")
ring = function(product, "call_ring_task")
assert "product_log_slow_pending();" in ring
assert ring.index("product_log_slow_pending();") < ring.index("vTaskDelay(pdMS_TO_TICKS(80))")
assert "static EXT_RAM_BSS_ATTR struct {\n    product_slow_log_t face, refresh;" in source

code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "product_slow_log.h"
static struct {
    product_slow_log_t face, refresh;
    uint32_t last_ms, face_reported, refresh_reported;
} s_slow_logs;
_Static_assert(sizeof(s_slow_logs)==108, "budget");
#define PRODUCT_SLOW_LOG_INTERVAL_MS 5000U
static uint32_t clock_ms;
static int64_t monotonic_ms(void) { return clock_ms; }
static unsigned log_calls;
static bool publish_during_log;
static char last_log[512];
static void logger(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    ++log_calls;
    if (publish_during_log) {
        publish_during_log=false;
        clock_ms+=17;
        uint32_t d[7]={2};
        product_slow_log_record(&s_slow_logs.face, clock_ms, 10001, d);
        product_slow_log_record(&s_slow_logs.refresh, clock_ms, 30001, d);
    }
}
#define ESP_LOGW(tag, ...) logger(__VA_ARGS__)
'''
code += function(product, "product_log_slow_pending")
code += r'''
static product_slow_log_t concurrent;
static atomic_bool done;
static void *writer(void *unused) {
    (void)unused;
    for (uint32_t n=1;n<=200000;++n) {
        uint32_t d[7];
        for (unsigned i=0;i<7;++i) d[i]=n+i;
        product_slow_log_record(&concurrent,n,n+100,d);
    }
    atomic_store(&done,true);
    return NULL;
}
int main(void) {
    product_slow_log_snapshot_t r;
    assert(product_slow_log_read(&s_slow_logs.face,&r) && r.count==0);
    clock_ms=5000; product_log_slow_pending(); assert(log_calls==0);
    uint32_t d[7]={1,2,3,4,5,6,7};
    for (unsigned i=0;i<50;++i)
        product_slow_log_record(&s_slow_logs.face,6000+i,9000+i,d);
    product_slow_log_record(&s_slow_logs.refresh,6070,30000,d);
    assert(log_calls==0); /* Publishing cannot call the output sink. */
    clock_ms=9999; product_log_slow_pending(); assert(log_calls==0);
    clock_ms=10000; publish_during_log=true; product_log_slow_pending();
    assert(log_calls==2 && s_slow_logs.face_reported==50);
    assert(strstr(last_log,"age_ms=0") != NULL);
    assert(product_slow_log_read(&s_slow_logs.face,&r) && r.count==51);
    assert(r.max_us==10001 && r.detail[0]==2);
    clock_ms=15000; product_log_slow_pending();
    assert(log_calls==3 && s_slow_logs.face_reported==51);
    clock_ms=20000; product_log_slow_pending(); assert(log_calls==3);
    atomic_store(&s_slow_logs.face.sequence,3);
    assert(!product_slow_log_read(&s_slow_logs.face,&r));
    atomic_store(&s_slow_logs.face.sequence,4);
    s_slow_logs.last_ms=UINT32_MAX-1000;
    product_slow_log_record(&s_slow_logs.face,100,8001,d);
    clock_ms=3998; product_log_slow_pending(); assert(log_calls==3);
    clock_ms=3999; product_log_slow_pending(); assert(log_calls==4);
    atomic_store(&s_slow_logs.face.count,UINT32_MAX);
    s_slow_logs.face_reported=UINT32_MAX;
    product_slow_log_record(&s_slow_logs.face,5000,8002,d);
    clock_ms=10000; product_log_slow_pending(); assert(log_calls==5);
    assert(s_slow_logs.face_reported==0);
    pthread_t t; assert(pthread_create(&t,NULL,writer,NULL)==0);
    unsigned reads=0;
    do {
        if (product_slow_log_read(&concurrent,&r) && r.count) {
            assert(r.count==r.at_ms && r.elapsed_us==r.at_ms+100);
            assert(r.max_us==r.elapsed_us);
            for (unsigned i=0;i<7;++i) assert(r.detail[i]==r.at_ms+i);
            ++reads;
        }
    } while (!atomic_load(&done));
    assert(pthread_join(t,NULL)==0);
    assert(product_slow_log_read(&concurrent,&r) && r.count==200000);
    printf("PASS: UI warnings deferred; coalescing, reentrant publish, wrap, 200000 concurrent writes (%u reads)\n",reads);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    base = Path(folder)
    c = base / "test.c"
    exe = base / "test"
    c.write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-pthread", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-I", str(root / "components/starter_product/src"), str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
