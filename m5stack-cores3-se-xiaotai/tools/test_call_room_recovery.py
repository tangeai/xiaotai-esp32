#!/usr/bin/env python3
"""Exercise the actual runtime HTTP handlers with cJSON and a bounded mock worker."""
from pathlib import Path
import re
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
dial_type = re.search(r'typedef struct \{[^}]*\} call_dial_t;', source)
dial_globals = re.findall(r'^static EXT_RAM_BSS_ATTR (?:call_dial_t|uint32_t) s_call_(?:dials|cleanup_sequence)[^;]*;', source, re.M)
constants = re.findall(r'^#define CALL_(?:CANCEL_H5_HOLD_MS|DIAL_TRACK_DEPTH|CLEANUP_\w+) .+$', source, re.M)
stages = re.search(r'typedef enum \{[^}]*\} call_http_stage_t;', source)
assert dial_type and len(dial_globals) == 2 and len(constants) == 6 and stages


def function(name):
    match = re.search(r'^static [^;{}]*?\b' + name + r'\([^;{}]*\)\n\{', source, re.M)
    assert match, name
    start = match.start()
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
#define ESP_LOGE(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define EXT_RAM_BSS_ATTR
#define esp_err_to_name(err) "mock"
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define PLATFORM_SERVICE_CALL 0
#define CALL_CONNECT_TIMEOUT_MS 30000
#define CALL_COMMAND_HANGUP 0x2001U
#define STARTER_RUNTIME_WAITING 0
#define STARTER_RUNTIME_CALL_INCOMING 4
#define STARTER_RUNTIME_CALL_CONNECTING 5
#define STARTER_RUNTIME_CALL_ACTIVE 6
typedef int esp_err_t;
typedef int starter_runtime_state_t;
'''
code += stages[0] + '\n'
code += r'''
typedef struct { uint32_t request_tag, command, platform_epoch; int error; char *text; size_t length; } runtime_event_t;
static struct { char room_id[129]; call_http_stage_t stage; bool attempted; } s_call_room_recovery;
static char s_call_room_id[129], s_call_id[65], s_call_peer_id[65]="peer";
static bool s_call_wechat;
static uint32_t s_session_generation=7;
static atomic_int s_public_state=STARTER_RUNTIME_CALL_CONNECTING;
static int64_t s_deadline_ms;
static int requests, finishes, submission_error;
static char last_path[128], last_body[256];
static unsigned last_stage, last_tag;
static int64_t clock_ms=1000;
static int64_t now_ms(void) { return clock_ms; }
/* Cancel/hangup path and the H5 admission gate. */
static bool s_call_outgoing=true, s_voip_profile_ready=true, h5_open=true;
static char s_call_wx_app_id[65], s_call_wx_model_id[65], s_call_wx_session_token[257];
static char s_call_wx_payload[513];
static int posts, post_error, sessions, wechat_rejects;
static char post_path[64], post_body[256];
static bool platform_ready=true;
static uint32_t binding_epoch=17;
static bool platform_client_ready(void) { return platform_ready; }
static uint32_t platform_client_epoch(void) { return binding_epoch; }
static void starter_tirtc_accept_h5(bool accept) { h5_open=accept; }
static bool starter_tirtc_connected(void) { return false; }
static int starter_tirtc_send_command(uint32_t c, const void *d, uint32_t n) {
 (void)c; (void)d; (void)n; return 0;
}
static void reject_wechat_values(const char *a, const char *m, const char *t, const char *r,
 const char *p, int reason) { (void)a; (void)m; (void)t; (void)r; (void)p; (void)reason; ++wechat_rejects; }
static int platform_client_request(int service, const char *path, const char *body,
 void (*cb)(const char *, void *), void *tag) {
 assert(service==0 && body && !cb && !tag); ++posts;
 snprintf(post_path,sizeof(post_path),"%s",path); snprintf(post_body,sizeof(post_body),"%s",body);
 return post_error;
}
static void cancel_pending_dial(void);
static void update_h5_admission(void);
/* Mirror the real exit ordering; the real finish_session is also exercised
 * by test_voice_reliability with a callback injected inside disconnect. */
static void finish_session(int error) {
 (void)error; ++sessions; starter_tirtc_accept_h5(false); cancel_pending_dial();
 assert(!h5_open); /* concurrent SDK admission during disconnect */
 s_call_room_id[0]=0; s_call_outgoing=false;
 s_call_wechat=false; memset(&s_call_room_recovery,0,sizeof(s_call_room_recovery));
 atomic_store(&s_public_state,STARTER_RUNTIME_WAITING);
 update_h5_admission();
}
static void starter_tirtc_expect_call(uint32_t g) { assert(g==s_session_generation); }
static void finish_call_session(int error, const char *message) {
 assert(message); ++finishes; finish_session(error);
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
 last_tag=(unsigned)(uintptr_t)tag>>8;
 return submission_error;
}
static bool prepare_external_connect_memory(void) { return true; }
static void restore_external_connect_memory(void) {}
static int starter_tirtc_call_connect(const char *id, const char *token, uint32_t g) {
 (void)id; (void)token; (void)g; return 0;
}
'''
code += '\n'.join(constants) + '\n' + dial_type[0] + '\n' + '\n'.join(dial_globals) + '\n'
for name in ('copy_json_string', 'response_ok', 'cancelled_dial_pending', 'update_h5_admission',
             'find_call_dial', 'track_call_dial', 'cancel_pending_dial', 'forget_call_dial',
             'dial_definitely_not_created', 'submit_device_call', 'valid_device_room_id',
             'request_call_room', 'handle_call_room_response', 'defer_cancel_cleanup',
             'request_cancel_cleanup', 'verify_cancel_complete', 'poll_h5_after_cancel',
             'reject_or_hangup_call', 'cancel_stale_dial_room', 'handle_cancel_cleanup', 'handle_call_http'):
    code += function(name) + '\n'
code += r'''
static void reset(void) {
 memset(&s_call_room_recovery,0,sizeof(s_call_room_recovery));
 s_call_room_id[0]=0; s_call_wechat=false; requests=finishes=submission_error=0;
 s_session_generation=7; atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
 s_call_outgoing=true; posts=post_error=sessions=wechat_rejects=0;
 memset(s_call_dials,0,sizeof(s_call_dials)); s_call_cleanup_sequence=0;
 platform_ready=true; binding_epoch=17; h5_open=true; clock_ms=1000;
}
static void deliver(unsigned stage, const char *body, unsigned gen) {
 runtime_event_t e={.request_tag=gen,.command=stage,.platform_epoch=binding_epoch,.error=200,
                   .text=(char *)body,.length=body ? strlen(body) : 0};
 handle_call_http(&e);
}
static void cleanup_reply(const char *body) { deliver(last_stage,body,last_tag); }
static void start_and_cancel(void) {
 assert(submit_device_call()==ESP_OK); requests=0;
 reject_or_hangup_call(false);
 assert(sessions && cancelled_dial_pending() && !h5_open);
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

 /* Hangup/timeout before /v1/call/request answers: the late room is cancelled. */
 const char *late="{\"code\":200,\"data\":{\"room_id\":\"d_late\"}}";
 reset(); start_and_cancel();
 assert(sessions==1 && !posts && atomic_load(&s_public_state)==STARTER_RUNTIME_WAITING);
 call_dial_t *old=find_call_dial(7);
 assert(old && old->cancelled && old->due_ms==1000+CALL_CONNECT_TIMEOUT_MS);
 finish_session(0); assert(!h5_open); /* another exit never bypasses admission policy */
 clock_ms=2000;
 deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 assert(requests==1 && last_stage==CALL_HTTP_CANCEL_RELEASE &&
        !strcmp(last_path,"/v1/call/cancel") && !strcmp(last_body,"{\"room_id\":\"d_late\"}"));
 assert(!s_call_room_id[0] && !finishes && old->tag==7 && !old->verified); /* never adopted */
 cleanup_reply("{\"code\":200}");
 assert(requests==2 && last_stage==CALL_HTTP_CANCEL_VERIFY && !h5_open);
 cleanup_reply(empty); assert(old->verified && !h5_open);
 clock_ms=2000+CALL_CANCEL_H5_HOLD_MS-1; poll_h5_after_cancel(clock_ms); assert(!h5_open);
 ++clock_ms; poll_h5_after_cancel(clock_ms);
 assert(h5_open && !cancelled_dial_pending());
 /* A definite failure created no room: release the hold, never touch 40202's older room. */
 reset(); start_and_cancel();
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40201,\"msg\":\"offline\"}",7);
 assert(!posts && !requests && !cancelled_dial_pending() && h5_open);
 reset(); start_and_cancel();
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}",7);
 assert(!posts && !requests && h5_open && !s_call_room_recovery.attempted);

 /* Lost, 5xx or malformed dial results query the server; no fabricated success. */
 const char *unknown[]={NULL,"","garbage","{\"code\":500}","{\"code\":200,\"data\":{\"room_id\":\"d bad\"}}"};
 for(unsigned i=0;i<sizeof(unknown)/sizeof(*unknown);++i) {
  reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,unknown[i],7);
  assert(requests==1 && last_stage==CALL_HTTP_CANCEL_VERIFY && !strcmp(last_body,"GET") && !h5_open);
  assert(find_call_dial(7)->room_id[0]==0);
  cleanup_reply(empty); assert(h5_open && !cancelled_dial_pending());
 }
 reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,NULL,7);
 cleanup_reply("{\"code\":200,\"data\":{\"room_id\":\"d_unowned\",\"status\":\"active\",\"role\":\"caller\"}}");
 assert(requests==1 && cancelled_dial_pending() && !h5_open); /* cannot infer unknown room ownership */
 clock_ms+=CALL_CLEANUP_RETRY_MS; poll_h5_after_cancel(clock_ms);
 assert(last_stage==CALL_HTTP_CANCEL_VERIFY && !strcmp(last_body,"GET"));
 /* A late original reply can still establish the exact room, not a guessed one. */
 deliver(CALL_HTTP_DEVICE_DIAL,late,7); assert(last_stage==CALL_HTTP_CANCEL_RELEASE);

 /* An accepted request whose cancel reply is lost/5xx must still be verified. */
 const char *failed_cancel[]={NULL,"{\"code\":500}","{\"code\":200}"};
 for(unsigned i=0;i<3;++i) {
  reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,late,7);
  cleanup_reply(failed_cancel[i]); assert(last_stage==CALL_HTTP_CANCEL_VERIFY);
  cleanup_reply("{\"code\":200,\"data\":{\"room_id\":\"d_late\",\"status\":\"answered\",\"role\":\"caller\"}}");
  assert(last_stage==CALL_HTTP_CANCEL_RELEASE && !strcmp(last_path,"/v1/call/hangup"));
  cleanup_reply("{\"code\":200}"); cleanup_reply(empty);
  clock_ms+=CALL_CANCEL_H5_HOLD_MS; poll_h5_after_cancel(clock_ms);
  assert(h5_open && !cancelled_dial_pending());
 }
 reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 unsigned duplicate_tag=last_tag;
 deliver(CALL_HTTP_DEVICE_DIAL,late,7); assert(requests==1 && last_tag==duplicate_tag);
 cleanup_reply("{\"code\":500}");
 cleanup_reply("{\"code\":200,\"data\":{\"room_id\":\"d_late\",\"status\":\"active\",\"role\":\"caller\"}}");
 assert(last_stage==CALL_HTTP_CANCEL_RELEASE && !strcmp(last_path,"/v1/call/cancel"));

 /* Enqueue failure keeps the ID; retries GET before acting, not blind cancel. */
 reset(); start_and_cancel(); submission_error=-9; deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 assert(requests==1 && !h5_open && !strcmp(find_call_dial(7)->room_id,"d_late"));
 clock_ms+=CALL_CLEANUP_RETRY_MS; submission_error=0; poll_h5_after_cancel(clock_ms);
 assert(requests==2 && last_stage==CALL_HTTP_CANCEL_VERIFY);
 cleanup_reply(empty); assert(h5_open && !cancelled_dial_pending());
 /* A misleading success body on HTTP 500 is not authoritative room proof. */
 reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,late,7); cleanup_reply(NULL);
 runtime_event_t http500={.request_tag=last_tag,.command=last_stage,.platform_epoch=binding_epoch,
                         .error=500,.text=(char *)empty,.length=strlen(empty)};
 handle_call_http(&http500); assert(cancelled_dial_pending() && !h5_open && !find_call_dial(7)->verified);
 clock_ms+=CALL_CLEANUP_RETRY_MS; poll_h5_after_cancel(clock_ms);
 cleanup_reply(empty); clock_ms+=CALL_CANCEL_H5_HOLD_MS; poll_h5_after_cancel(clock_ms);
 assert(h5_open && !cancelled_dial_pending());
 /* Lost runtime delivery times out to GET; stale attempt callbacks cannot finish it. */
 reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 unsigned obsolete_stage=last_stage, obsolete_tag=last_tag;
 clock_ms+=CALL_CLEANUP_RESULT_TIMEOUT_MS; poll_h5_after_cancel(clock_ms);
 assert(requests==2 && last_stage==CALL_HTTP_CANCEL_VERIFY && last_tag!=obsolete_tag);
 deliver(obsolete_stage,"{\"code\":200}",obsolete_tag); assert(requests==2 && !h5_open);
 cleanup_reply(empty); assert(h5_open && !cancelled_dial_pending());
 /* Time alone never turns a failed cleanup into success; work is bounded. */
 reset(); start_and_cancel(); deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 cleanup_reply(NULL);
 while(find_call_dial(7)->attempts<CALL_CLEANUP_MAX_REQUESTS) {
  cleanup_reply(NULL); clock_ms+=CALL_CLEANUP_RETRY_MS; poll_h5_after_cancel(clock_ms);
 }
 cleanup_reply(NULL); old=find_call_dial(7);
 assert(old && old->due_ms==0 && !old->verified && !h5_open && requests==CALL_CLEANUP_MAX_REQUESTS);
 clock_ms+=1000000; poll_h5_after_cancel(clock_ms);
 assert(requests==CALL_CLEANUP_MAX_REQUESTS && !h5_open);

 /* Redial: the old answer is cancelled without disturbing the new CONNECTING call. */
 reset(); start_and_cancel();
 s_session_generation=8; s_call_outgoing=true;
 atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
 deliver(CALL_HTTP_DEVICE_DIAL,late,7);
 assert(requests==1 && strstr(last_body,"d_late") && !s_call_room_id[0] && !finishes);
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":200,\"data\":{\"room_id\":\"d_new\"}}",8);
 assert(!strcmp(s_call_room_id,"d_new") && requests==1 && !finishes);
 cleanup_reply(NULL);
 cleanup_reply("{\"code\":200,\"data\":{\"room_id\":\"d_new\",\"status\":\"active\",\"role\":\"caller\"}}");
 clock_ms+=CALL_CANCEL_H5_HOLD_MS; poll_h5_after_cancel(clock_ms);
 assert(!strcmp(s_call_room_id,"d_new") && !finishes && !h5_open && !cancelled_dial_pending());

 /* Two quick cancels retain both IDs; completing one cannot open the gate. */
 reset(); start_and_cancel();
 s_session_generation=8; s_call_outgoing=true;
 atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
 assert(submit_device_call()==ESP_OK); requests=0; reject_or_hangup_call(false);
 assert(find_call_dial(7)->cancelled && find_call_dial(8)->cancelled);
 deliver(CALL_HTTP_DEVICE_DIAL,late,7); obsolete_stage=last_stage; obsolete_tag=last_tag;
 deliver(CALL_HTTP_DEVICE_DIAL,"{\"code\":200,\"data\":{\"room_id\":\"d_late2\"}}",8);
 unsigned other_stage=last_stage, other_tag=last_tag;
 deliver(obsolete_stage,NULL,obsolete_tag); cleanup_reply(empty);
 clock_ms+=CALL_CANCEL_H5_HOLD_MS; poll_h5_after_cancel(clock_ms); assert(!h5_open);
 deliver(other_stage,NULL,other_tag); cleanup_reply(empty);
 poll_h5_after_cancel(clock_ms); assert(h5_open && !cancelled_dial_pending());

 /* A dial answer orphaned by any other session end is cancelled too. */
 reset(); assert(submit_device_call()==ESP_OK); finish_session(-9); assert(!h5_open);
 requests=0; deliver(CALL_HTTP_DEVICE_DIAL,late,7); assert(requests==1 && !h5_open);
 reset(); atomic_store(&s_public_state,STARTER_RUNTIME_WAITING);
 deliver(CALL_HTTP_DEVICE_INFO,"{\"code\":200,\"data\":{\"token\":\"t\",\"device_id\":\"d\"}}",7);
 assert(!posts && !requests && !finishes); /* other stale stages remain dropped */
 /* Current malformed reply also retains unknown server outcome across finish. */
 reset(); assert(submit_device_call()==ESP_OK); deliver(CALL_HTTP_DEVICE_DIAL,NULL,7);
 assert(find_call_dial(7) && find_call_dial(7)->cancelled && !h5_open);
 clock_ms+=CALL_CONNECT_TIMEOUT_MS; poll_h5_after_cancel(clock_ms);
 assert(last_stage==CALL_HTTP_CANCEL_VERIFY);

 /* Exhausted records cannot be overwritten by yet another dial. */
 reset();
 for(unsigned i=0;i<CALL_DIAL_TRACK_DEPTH;++i) {
  s_session_generation=7+i; atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
  assert(submit_device_call()==ESP_OK); finish_session(0);
 }
 int submitted=requests; ++s_session_generation;
 assert(submit_device_call()==ESP_ERR_NO_MEM && requests==submitted && !h5_open);
 ++binding_epoch; poll_h5_after_cancel(clock_ms); assert(h5_open && !cancelled_dial_pending());
 runtime_event_t old_owner={.request_tag=7,.command=CALL_HTTP_DEVICE_DIAL,.platform_epoch=17,
                           .text=(char *)late,.length=strlen(late)};
 handle_call_http(&old_owner); assert(requests==submitted && !cancelled_dial_pending());
 /* Platform offline alone is not a binding change or cleanup proof. */
 reset(); start_and_cancel(); platform_ready=false; clock_ms+=CALL_CONNECT_TIMEOUT_MS;
 poll_h5_after_cancel(clock_ms); assert(!requests && cancelled_dial_pending() && !h5_open);
 platform_ready=true; clock_ms+=CALL_CLEANUP_RETRY_MS; poll_h5_after_cancel(clock_ms);
 assert(requests==1 && last_stage==CALL_HTTP_CANCEL_VERIFY);

 /* Known room, recovery in flight, WeChat and incoming paths keep their behaviour. */
 reset(); snprintf(s_call_room_id,sizeof(s_call_room_id),"d_known"); reject_or_hangup_call(false);
 assert(posts==1 && !strcmp(post_path,"/v1/call/cancel") &&
        !strcmp(post_body,"{\"room_id\":\"d_known\"}"));
 assert(!cancelled_dial_pending() && h5_open);
 reset(); conflict(); reject_or_hangup_call(false);
 assert(!posts && !cancelled_dial_pending() && h5_open); /* no dial request is in flight */
 reset(); s_call_wechat=true; reject_or_hangup_call(false);
 assert(!posts && !wechat_rejects && !cancelled_dial_pending() && h5_open);
 reset(); s_call_outgoing=false; atomic_store(&s_public_state,STARTER_RUNTIME_CALL_INCOMING);
 snprintf(s_call_room_id,sizeof(s_call_room_id),"d_in"); reject_or_hangup_call(true);
 assert(posts==1 && !strcmp(post_path,"/v1/call/reject") && !cancelled_dial_pending());
 printf("dial ownership PSRAM: %zu bytes + %zu-byte sequence\n",sizeof(s_call_dials),sizeof(s_call_cleanup_sequence));
 return 0;
}
'''
owner = source[source.index('static void runtime_task('):source.index('esp_err_t starter_runtime_start(')]
assert owner.index('poll_h5_after_cancel(now_ms());') < owner.index('xQueueReceive(s_queue')
assert re.search(r'state == STARTER_RUNTIME_CALL_CONNECTING\) \{\s*reject_or_hangup_call\(false\);', owner)
assert 'update_h5_admission();' in function('handle_voip_profile')
finish = function('finish_session')
assert finish.index('starter_tirtc_accept_h5(false);') < finish.index('starter_tirtc_disconnect()')
assert finish.index('cancel_pending_dial();') < finish.index('starter_tirtc_disconnect()')
assert finish.index('update_h5_admission();') > finish.index('publish_state(STARTER_RUNTIME_WAITING)')
cjson = project / 'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='call-room-recovery-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(cjson),
                    str(path / 'test.c'), str(cjson / 'cJSON.c'), '-lm', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: real call-room handlers, bounded recovery/roles/cancel/stale responses/failure semantics; '
      'persistent cancelled-dial ownership, authoritative verification, bounded failures and H5 admission')
