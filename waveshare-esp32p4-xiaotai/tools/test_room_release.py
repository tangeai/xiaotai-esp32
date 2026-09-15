"""Exercise release ownership and nonblocking UI reads from the actual C code."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
room = (root / 'components/starter_runtime/src/starter_room.inc').read_text(encoding='utf-8')
runtime = (root / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
product = (root / 'components/starter_product/src/starter_product.c').read_text(encoding='utf-8')


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:bool|void|esp_err_t) ' + name + r'\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


tick = function(room, 'room_tick')
assert tick.index('if (s_room.release.id[0])') < tick.index('snprintf(s_room.session')
assert 'return;' in tick[tick.index('if (s_room.release.id[0])'):tick.index('STARTER_RUNTIME_ROOM_ACTIVE')]
result = function(room, 'room_http_result')
assert result.index('event->request_tag != s_room.pending_ticket') < result.index('room_complete_release')
assert 'else room_remember_release(false);' in result  # token completing after AI preemption
assert 'room_remember_release(error != 0);' in function(room, 'room_before_finish')
assert 'room_presence(' not in function(room, 'room_before_finish')
assert 'op == ROOM_RELEASE' in tick  # lost / timed out callback retains cleanup ownership
assert 'memset(&s_room, 0' in function(room, 'room_reset')  # binding epoch discards old authority
ui = function(product, 'product_tick')
assert 'if (!starter_runtime_try_product_snapshot(&product)) return;' in ui
# Snapshot contention may defer business content, never Wi-Fi/binding setup.
assert ui.index('s3_update_setup(') < ui.index('starter_runtime_try_product_snapshot(')
assert 'starter_runtime_product_snapshot()' not in ui

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 257
#define ROOM_RELEASE 8
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
static struct {
    char id[129], session[33]; uint32_t version, epoch;
    struct { char id[129], session[33]; bool failed; uint32_t version; } release;
    bool dirty; int64_t poll_at;
} s_room;
static uint32_t epoch=3;
static int64_t clock_ms=10000;
static int queue_rc, requests;
static char body_copy[512];
static uint32_t platform_client_epoch(void) { return epoch; }
static int64_t now_ms(void) { return clock_ms; }
static esp_err_t room_http(int op, const cJSON *body) {
    assert(op==ROOM_RELEASE); ++requests;
    assert(cJSON_PrintPreallocated((cJSON *)body,body_copy,sizeof(body_copy),false));
    return queue_rc;
}
typedef struct { int contacts, phase; } starter_runtime_product_snapshot_t;
static void *s_product_mutex;
static starter_runtime_product_snapshot_t s_product_snapshot={2,3};
static int available, gives;
static int xSemaphoreTake(void *mutex, unsigned wait) { assert(mutex && wait==0); return available; }
static void xSemaphoreGive(void *mutex) { assert(mutex); ++gives; }
'''
code += '\n'.join(function(room, n) for n in (
    'room_remember_release', 'room_release_request', 'room_complete_release'))
code += function(runtime, 'starter_runtime_try_product_snapshot')
code += r'''
int main(void) {
    strcpy(s_room.id,"old-room"); strcpy(s_room.session,"old-session");
    s_room.version=4; s_room.epoch=3;
    room_remember_release(false);
    assert(!strcmp(s_room.release.session,"old-session"));
    /* Later assignment/session changes cannot change the queued release tuple. */
    strcpy(s_room.id,"new-room"); strcpy(s_room.session,"new-session"); s_room.version=5;
    queue_rc=257; assert(room_release_request()==257);
    assert(!strcmp(s_room.release.session,"old-session"));
    queue_rc=0; assert(room_release_request()==0 && requests==2);
    cJSON *body=cJSON_Parse(body_copy); assert(body);
    assert(!strcmp(cJSON_GetObjectItem(body,"room_id")->valuestring,"old-room"));
    assert(!strcmp(cJSON_GetObjectItem(body,"session_id")->valuestring,"old-session"));
    assert(cJSON_GetObjectItem(body,"assignment_version")->valueint==4);
    assert(!strcmp(cJSON_GetObjectItem(body,"state")->valuestring,"suspended"));
    cJSON_Delete(body);
    assert(!room_complete_release(50000) && s_room.release.id[0]);
    assert(!room_complete_release(200.5) && s_room.release.id[0]);
    assert(!room_complete_release(40300) && s_room.release.id[0]);
    assert(room_complete_release(200) && !s_room.release.id[0]);
    assert(s_room.dirty && s_room.poll_at==clock_ms);
    room_remember_release(true); room_remember_release(false);
    assert(s_room.release.failed && room_release_request()==0);
    body=cJSON_Parse(body_copy);
    assert(!strcmp(cJSON_GetObjectItem(body,"state")->valuestring,"connect_failed"));
    cJSON_Delete(body);
    assert(room_complete_release(40921));
    room_remember_release(false); assert(!s_room.release.failed && room_complete_release(40400));
    epoch=4; room_remember_release(false); assert(!s_room.release.id[0]);

    starter_runtime_product_snapshot_t out={9,9};
    assert(!starter_runtime_try_product_snapshot(&out) && out.contacts==9);
    s_product_mutex=&out; available=0;
    assert(!starter_runtime_try_product_snapshot(&out) && out.contacts==9 && gives==0);
    available=1; assert(starter_runtime_try_product_snapshot(&out));
    assert(out.contacts==2 && out.phase==3 && gives==1);
    assert(!starter_runtime_try_product_snapshot(NULL));
    puts("PASS: release tuple, failures, stale receipt, epoch, UI nonblocking snapshot");
}
'''
cjson = root / 'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='p4-room-release-') as directory:
    p = Path(directory)
    (p / 'test.c').write_text(code, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I'+str(cjson),
                    str(p/'test.c'), str(cjson/'cJSON.c'), '-lm', '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
