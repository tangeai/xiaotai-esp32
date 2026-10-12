#!/usr/bin/env python3
"""Run the real WHIP worker against the IDF WithCaps deletion contract.

Mocks distinguish ordinary deletion (retains a WithCaps stack/TCB) from
WithCaps deletion. This tests API pairing and all worker exit paths, not RTOS
scheduling or the SDK handshake on hardware.
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          "components/starter_runtime/src/starter_runtime.c").read_text()
start = source.index("static void voip_connect_task(void *argument)\n{")
worker = source[start:source.index("\n}", start) + 2]
assert 'xTaskCreateWithCaps(voip_connect_task, "voip_whip"' in source
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define EVENT_VOIP_CONNECT_RESULT 1
#define ESP_ERR_INVALID_ARG -2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct { char peer_id[4], token[4]; uint32_t generation; } voip_connect_request_t;
typedef struct { int type; uint32_t generation; int error; } runtime_event_t;
static int result, connects, frees, deleted, outstanding;
static bool queue_ok;
static runtime_event_t captured;
static int starter_tirtc_voip_connect(const char *peer, const char *token, uint32_t generation) {
    assert(peer && token && generation==7); ++connects; return result;
}
static bool queue_event(const runtime_event_t *event) { captured=*event; return queue_ok; }
static void request_free(void *ptr) { assert(ptr); ++frees; free(ptr); }
void vTaskDelete(void *task) { assert(task==NULL); ++deleted; }
void vTaskDeleteWithCaps(void *task) { assert(task==NULL); ++deleted; --outstanding; }
#define free request_free
'''
code += worker
code += r'''
#undef free
int main(void) {
    for (int i=0;i<100;i++) {
        result=(i%2) ? -1 : 0; queue_ok=(i%3)!=0;
        voip_connect_request_t *r=calloc(1,sizeof(*r)); assert(r); r->generation=7;
        ++outstanding;
        voip_connect_task(r);
        assert(outstanding==0); /* WithCaps stack/TCB must be reclaimed. */
        assert(captured.type==EVENT_VOIP_CONNECT_RESULT && captured.generation==7);
        assert(captured.error==result && frees==i+1 && connects==i+1 && deleted==i+1);
    }
    ++outstanding; voip_connect_task(NULL);
    assert(outstanding==0 && captured.error==ESP_ERR_INVALID_ARG && captured.generation==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="voip-cleanup-") as directory:
    path=Path(directory)
    (path/"test.c").write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path/"test.c"), "-o", str(path/"test")], check=True)
    subprocess.run([str(path/"test")], check=True)
print("PASS: WHIP WithCaps cleanup on success/failure/full queue/null request, 100 iterations")
