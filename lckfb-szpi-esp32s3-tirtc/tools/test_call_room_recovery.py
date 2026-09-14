#!/usr/bin/env python3
"""Exercise the actual runtime HTTP handlers with cJSON and a bounded mock worker."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'components/starter_runtime/src/starter_runtime.c').read_text()


def function(name):
    start = source.index('\nstatic ', source.index(name) - 80)
    while name not in source[start:source.index('\n{', start)]:
        start = source.index('\nstatic ', start + 1)
    end = source.index('\n}', source.index('\n{', start)) + 2
    return source[start:end]


code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define esp_err_to_name(err) "mock"
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define PLATFORM_SERVICE_CALL 0
#define CALL_CONNECT_TIMEOUT_MS 30000
#define STARTER_RUNTIME_CALL_CONNECTING 5
typedef int esp_err_t;
typedef enum { CALL_HTTP_DEVICE_DIAL=1, CALL_HTTP_DEVICE_INFO, CALL_HTTP_VOIP_DIAL,
 CALL_HTTP_ROOM_QUERY, CALL_HTTP_ROOM_RELEASE, CALL_HTTP_ROOM_VERIFY } call_http_stage_t;
typedef struct { uint32_t request_tag, command; char *text; size_t length; } runtime_event_t;
static struct { char room_id[129]; call_http_stage_t stage; bool attempted; } s_call_room_recovery;
static char s_call_room_id[129], s_call_id[65], s_call_peer_id[65]="peer";
static bool s_call_wechat;
static uint32_t s_session_generation=7;
static atomic_int s_public_state=STARTER_RUNTIME_CALL_CONNECTING;
static int64_t s_deadline_ms;
static int requests, finishes, submission_error;
static char last_path[128], last_body[256];
static unsigned last_stage;
static int64_t now_ms(void) { return 1000; }
static void starter_tirtc_expect_call(uint32_t g) { assert(g==s_session_generation); }
static void finish_call_session(int error, const char *message) {
 (void)error; assert(message); ++finishes; atomic_store(&s_public_state,0);
 memset(&s_call_room_recovery,0,sizeof(s_call_room_recovery));
}
static void call_http_response(const char *body, void *arg) { (void)body; (void)arg; }
static void *call_http_tag(uint32_t g, call_http_stage_t stage) {
 return (void *)(uintptr_t)((g<<8)|(unsigned)stage);
}
static int platform_client_request_timeout(int service, const char *path, const char *body,
 unsigned timeout, void (*cb)(const char *,void *), void *tag) {
 assert(service==0 && timeout<=30000 && cb); ++requests;
 snprintf(last_path,sizeof(last_path),"%s",path);
 snprintf(last_body,sizeof(last_body),"%s",body ? body : "GET");
 last_stage=(unsigned)(uintptr_t)tag&255;
 return submission_error;
}
static bool prepare_external_connect_memory(void) { return true; }
static void restore_external_connect_memory(void) {}
static int starter_tirtc_call_connect(const char *id, const char *token, uint32_t g) {
 (void)id; (void)token; (void)g; return 0;
}
'''
for name in ('copy_json_string', 'response_ok', 'submit_device_call', 'valid_device_room_id',
             'request_call_room', 'handle_call_room_response', 'handle_call_http'):
    code += function(name) + '\n'
code += r'''
static void reset(void) {
 memset(&s_call_room_recovery,0,sizeof(s_call_room_recovery));
 s_call_room_id[0]=0; s_call_wechat=false; requests=finishes=submission_error=0;
 s_session_generation=7; atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
}
static void deliver(unsigned stage, const char *body, unsigned gen) {
 runtime_event_t e={.request_tag=gen,.command=stage,.text=(char *)body,.length=strlen(body)};
 handle_call_http(&e);
}
static void conflict(void) {
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}",7);
 assert(!finishes && requests==1 && last_stage==CALL_HTTP_ROOM_QUERY);
 assert(!strcmp(last_path,"/v1/call/room") && !strcmp(last_body,"GET"));
}
static void room(const char *status,const char *role) {
 char body[256]; snprintf(body,sizeof(body),
 "{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"status\":\"%s\",\"role\":\"%s\"}}",status,role);
 deliver(CALL_HTTP_ROOM_QUERY,body,7);
}
int main(void) {
 const char *empty="{\"code\":200,\"data\":null}";
 const char *cases[][3]={{"answered","caller","/v1/call/hangup"},
 {"answered","callee","/v1/call/hangup"},{"active","caller","/v1/call/cancel"},
 {"active","callee","/v1/call/reject"}};
 for(unsigned i=0;i<4;i++) {
  reset(); conflict(); room(cases[i][0],cases[i][1]);
  assert(!strcmp(last_path,cases[i][2]) && strstr(last_body,"d_old"));
  assert(!s_call_room_id[0] && last_stage==CALL_HTTP_ROOM_RELEASE);
  deliver(CALL_HTTP_ROOM_RELEASE,"{\"code\":200}",7);
  assert(last_stage==CALL_HTTP_ROOM_VERIFY);
  deliver(CALL_HTTP_ROOM_VERIFY,empty,7);
  assert(requests==4 && !finishes && last_stage==CALL_HTTP_DEVICE_DIAL);
  assert(!strcmp(last_path,"/v1/call/request") && s_call_room_recovery.attempted);
  deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":200,\"data\":{\"room_id\":\"d_new\"}}",7);
  assert(!strcmp(s_call_room_id,"d_new"));
 }
 reset(); conflict(); room("answered","caller");
 deliver(CALL_HTTP_ROOM_RELEASE,"",7); /* lost HTTP response */
 assert(last_stage==CALL_HTTP_ROOM_VERIFY);
 deliver(CALL_HTTP_ROOM_VERIFY,empty,7); assert(requests==4 && !finishes);
 reset(); conflict(); room("answered","callee");
 deliver(CALL_HTTP_ROOM_RELEASE,"{\"code\":200,\"msg\":\"ok\"}",7);
 deliver(CALL_HTTP_ROOM_VERIFY,"{\"code\":200,\"msg\":\"ok\"}",7);
 assert(requests==4 && !finishes); /* deployed server omits empty data */
 reset(); conflict(); deliver(CALL_HTTP_ROOM_QUERY,"{\"code\":200,\"data\":{}}",7);
 assert(finishes==1 && requests==1); /* missing fields in a nonempty object are not idle */
 reset(); conflict(); deliver(CALL_HTTP_ROOM_QUERY,"{\"code\":500}",7);
 assert(finishes==1 && requests==1);
 reset(); conflict(); room("answered","caller");
 deliver(CALL_HTTP_ROOM_RELEASE,"{\"code\":500}",7);
 deliver(CALL_HTTP_ROOM_VERIFY,"{\"code\":200,\"data\":{\"room_id\":\"d_old\"}}",7);
 assert(finishes==1 && requests==3); /* never retry with a nonempty room */
 reset(); conflict();
 deliver(CALL_HTTP_ROOM_QUERY,"{\"code\":200,\"data\":{\"room_id\":\"d_other\",\"status\":\"answered\",\"role\":\"caller\"}}",7);
 assert(finishes==1 && requests==1); /* never close a different room */
 reset(); conflict(); room("unknown","caller"); assert(finishes==1 && requests==1);
 reset(); conflict(); room("answered","stranger"); assert(finishes==1 && requests==1);
 reset(); conflict(); deliver(CALL_HTTP_ROOM_QUERY,empty,7);
 assert(requests==2 && last_stage==CALL_HTTP_DEVICE_DIAL); /* concurrently ended */
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}",7);
 assert(finishes==1 && requests==2); /* one recovery per user intent */
 reset(); conflict(); atomic_store(&s_public_state,0);
 deliver(CALL_HTTP_ROOM_QUERY,empty,7); assert(requests==1); /* user cancelled */
 reset(); conflict(); ++s_session_generation;
 deliver(CALL_HTTP_ROOM_QUERY,empty,7); assert(requests==1); /* old generation */
 reset(); conflict(); deliver(CALL_HTTP_ROOM_VERIFY,empty,7); assert(requests==1); /* wrong stage */
 reset(); submission_error=-4; deliver(CALL_HTTP_DEVICE_DIAL,
 "{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}",7); assert(finishes==1);
 reset(); deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40202}",7); assert(finishes==1 && requests==0);
 reset(); deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40201}",7); assert(finishes==1 && requests==0);
 reset(); s_call_wechat=true;
 deliver(CALL_HTTP_VOIP_DIAL,"{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}",7);
 assert(finishes==1 && requests==0);
 reset(); deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":200,\"data\":{\"room_id\":\"d_new\"}}",7);
 assert(!finishes && !requests && !strcmp(s_call_room_id,"d_new"));
 assert(!valid_device_room_id("wx_room") && !valid_device_room_id("d_") &&
        !valid_device_room_id("d_quote\"") && valid_device_room_id("d_roomid_0123"));
 return 0;
}
'''
cjson = project / 'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='call-room-recovery-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(cjson),
                    str(path / 'test.c'), str(cjson / 'cJSON.c'), '-lm', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: real call-room handlers, bounded recovery/roles/cancel/stale responses/failure semantics')
