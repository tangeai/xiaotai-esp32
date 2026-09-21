#!/usr/bin/env python3
"""Run the complete production Room owner with HTTP/SDK boundaries faked.

Real cJSON, real media PTT admission and queue ownership. No device/network I/O.
"""
from pathlib import Path
import subprocess
import tempfile
import re
from test_voice_reliability import function

ROOT = Path(__file__).resolve().parents[1]
runtime = "components/starter_runtime/src/starter_runtime.c"
media = "components/starter_media/src/starter_media.c"
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define xSemaphoreTake(a,b) ((void)(a),(void)(b),1)
#define xSemaphoreGive(a) ((void)(a))
#define PLATFORM_SERVICE_CALL 2
enum { EVENT_ROOM_HTTP, EVENT_ROOM_INTENT };
typedef struct {
    int type, error;
    starter_tirtc_mode_t mode;
    uint32_t generation, request_tag, command, platform_epoch, length;
    bool flag;
    char *text;
} runtime_event_t;
static atomic_int s_public_state;
static atomic_uint s_public_connection_generation;
static void *s_product_mutex = (void *)1;
static uint32_t s_session_generation, s_connection_generation;
static int64_t s_deadline_ms, clock_ms=1000;
static bool s_external_connect_reserve_loaned, ready=true, wifi=true;
static uint32_t epoch=1;
static const char s_device_id[]="self";
typedef struct {char peer_id[1024], token[1024];} ai_credentials_t;
static unsigned requests, commands, disconnects, heap_calls;
static int http_error, send_error;
static char last_path[100], last_body[4096], last_command[4096];
static runtime_event_t queued;
static bool queue_full;
static void *heap_caps_calloc(size_t n,size_t s,int caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); heap_calls++; return calloc(n,s);
}
static int64_t now_ms(void) {return clock_ms;}
static bool platform_client_ready(void) {return ready;}
static bool wifi_manager_connected(void) {return wifi;}
static uint32_t platform_client_epoch(void) {return epoch;}
static uint32_t esp_random(void) {return 0x12345678;}
static bool queue_http_result(int type,uint32_t ticket,uint32_t stage,const char *body) {
    (void)type;(void)ticket;(void)stage;(void)body;return true;
}
static esp_err_t platform_client_request_timeout(int service,const char *path,const char *body,
    unsigned timeout,void(*callback)(const char*,void*),void *user) {
    assert(service==PLATFORM_SERVICE_CALL && timeout>0 && callback);(void)user;
    requests++;snprintf(last_path,sizeof(last_path),"%s",path);
    snprintf(last_body,sizeof(last_body),"%s",body?body:"");return http_error;
}
bool starter_tirtc_started(void) {return true;}
void starter_tirtc_accept_h5(bool allow) {(void)allow;}
static bool prepare_external_connect_memory(void) {return true;}
static void restore_external_connect_memory(void) {}
int starter_tirtc_room_connect(const char *peer,const char *token,uint32_t tag) {
    assert(!strcmp(peer,"peer") && !strcmp(token,"secret") && tag==s_session_generation);return 0;
}
bool starter_tirtc_is_room_command(uint32_t cmd) {return cmd==0x2200;}
int starter_tirtc_send_command(uint32_t cmd,const void *data,uint32_t len) {
    assert(cmd==0x2200 && len<sizeof(last_command));
    memcpy(last_command,data,len);last_command[len]=0;commands++;return send_error?send_error:(int)len;
}
static atomic_bool s_active, s_microphone_muted;
static atomic_int s_mode;
static atomic_uint s_generation, s_drain_generation, s_room_ptt_generation, s_mute_epoch;
static struct {bool microphone_muted;} starter_media_status(void) {
    return (typeof(starter_media_status())){atomic_load(&s_microphone_muted)};
}
starter_tirtc_mode_t starter_tirtc_mode(void) {return s_active ? STARTER_TIRTC_ROOM : STARTER_TIRTC_NONE;}
static esp_err_t starter_media_start(starter_tirtc_mode_t mode,uint32_t gen) {
    assert(mode==STARTER_TIRTC_ROOM);s_active=true;s_mode=mode;s_generation=gen;return 0;
}
static void publish_state(starter_runtime_state_t state) {
    s_public_state=state;s_public_connection_generation=s_connection_generation;
}
static void finish_session(int error);
static bool copy_event_text(runtime_event_t *event,const char *text,size_t len) {
    event->text=malloc(len+1);assert(event->text);memcpy(event->text,text,len+1);event->length=len;return true;
}
static bool queue_event(const runtime_event_t *event) {if(queue_full)return false;queued=*event;return true;}
static void release_event(runtime_event_t *event) {free(event->text);event->text=NULL;}
'''
code += re.search(r"EXT_RAM_BSS_ATTR static struct \{[^}]+\} s_room_audio_diag;",
                  (ROOT / media).read_text(), re.S)[0]
code += function(media, "same_session")
code += function(media, "uplink_session_current")
code += function(media, "starter_media_room_ptt")
code += function(runtime, "copy_json_string")
code += (ROOT / "components/starter_runtime/src/starter_room.inc").read_text()
code += function("components/starter_product/src/starter_product_s3_room.inc", "s3_room_state_text")
code += r'''
static void finish_session(int error) {
    room_detach(error);s_active=false;s_connection_generation=0;
    s_deadline_ms=0;disconnects++;publish_state(STARTER_RUNTIME_WAITING);
}
static void response(const char *body) {
    runtime_event_t event={.request_tag=s_room.ticket,.text=(char*)body,.length=strlen(body)};
    room_http(&event);
}
static void signal(const char *json) {
    runtime_event_t event={.mode=STARTER_TIRTC_ROOM,.generation=s_room.generation,
        .command=0x2200,.text=(char*)json,.length=strlen(json)};
    room_command(&event);
}
static const char *assignment="{\"code\":200,\"data\":{\"room_id\":\"group_room_1\",\"room_code\":\"001234\",\"desired_state\":\"joined\",\"assignment_version\":1}}";
static const char *token="{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"secret\",\"heartbeat_seconds\":15,\"lease_seconds\":45}}";
static const char *snapshot="{\"jsonrpc\":\"2.0\",\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"app:group_room_1\",\"self\":{\"device_id\":\"self\"},\"participants\":[{\"participant_id\":\"self-p\",\"device_id\":\"self\",\"mic_state\":\"off\",\"state\":\"active\"},{\"participant_id\":\"peer-p\",\"device_id\":\"peer\",\"mic_state\":\"speaking\",\"state\":\"active\"}]}}";
static void join(void) {
    room_tick(clock_ms); assert(s_room.pending==ROOM_TOKEN && room_owns_media());
    response(token);assert(s_room.pending==0 && heap_calls>0);
    runtime_event_t event={.mode=STARTER_TIRTC_ROOM,.request_tag=s_session_generation,.generation=s_session_generation+10,.flag=true};
    room_connection(&event);assert(strstr(last_command,"join_room"));
    assert(strstr(last_command,"g711a") && strstr(last_command,"8000") && !s_active);
    char ack[400];snprintf(ack,sizeof(ack),"{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"session_id\":\"server-session\",\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}",s_session_generation);
    signal(ack);assert(s_active && s_room.phase==STARTER_ROOM_LISTENING && !s_deadline_ms);
    assert(!uplink_session_current(STARTER_TIRTC_ROOM,s_room.generation));
    room_tick(clock_ms);assert(s_room.pending==ROOM_PRESENCE && strstr(last_body,"joined"));
    response("{\"code\":200,\"data\":null}");signal(snapshot);
}
static unsigned json_live, attempts, fail_at;
static void *jm(size_t n) { if(++attempts==fail_at)return NULL;void*p=malloc(n);if(p)json_live++;return p; }
static void jf(void*p) {if(p){assert(json_live);json_live--;free(p);}}
int main(void) {
    cJSON *probe=room_json_cJSON_Parse("{\"a\":[1,2,3]}");assert(probe);
    room_json_cJSON_Delete(probe);
    cJSON_Hooks hooks={jm,jf};room_json_cJSON_InitHooks(&hooks);
    room_tick(clock_ms);
    assert(requests==0 && !s_active && !room_app_open());
    assert(starter_runtime_room_create("")==ESP_ERR_INVALID_STATE);
    starter_runtime_room_set_open(true);
    room_apply_app_request();
    assert(starter_runtime_room_join("12345","")==ESP_ERR_INVALID_ARG);
    assert(starter_runtime_room_join("123456","abc4")==ESP_ERR_INVALID_ARG);
    assert(starter_runtime_room_create("0000")==0);
    assert(strstr(queued.text,"0000"));
    room_intent(&queued);assert(requests==0 && !s_room.assignment_known);
    release_event(&queued);
    queue_full=true;assert(starter_runtime_room_create("")==ESP_ERR_TIMEOUT);queue_full=false;
    for(unsigned i=1;i<40;i++){fail_at=i;attempts=0;int rc=starter_runtime_room_join("001234","0123");
        if(rc==0)release_event(&queued);
        assert(json_live==0);}
    fail_at=0;
    room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    response("{\"code\":40300}");
    assert(s_room.access_denied && s_room.error==40300 && !s_room.pending && !s_active);
    unsigned denied_requests=requests;
    for(unsigned i=0;i<12;i++){clock_ms+=5000;room_tick(clock_ms);}
    assert(requests==denied_requests && s_room.retries==1 && !s_room.generation);
    starter_room_view_t denied_view;
    assert(starter_runtime_room_read(0,&denied_view) && denied_view.phase==STARTER_ROOM_ERROR);
    assert(!strcmp(s3_room_state_text(&denied_view),"请检查设备绑定"));
    response(assignment);assert(s_room.access_denied && !s_room.assigned);
    assert(starter_runtime_room_refresh()==0);room_intent(&queued);release_event(&queued);
    room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC && requests==denied_requests+1);
    response("{\"code\":200,\"data\":{\"desired_state\":\"\",\"assignment_version\":0}}");
    assert(!s_room.access_denied && !s_room.error && !s_room.retries);
    assert(!s_room.assigned && s_room.phase==STARTER_ROOM_EMPTY);
    assert(starter_runtime_room_join("001234","")==0);room_intent(&queued);release_event(&queued);
    assert(strstr(last_path,"/join") && strstr(last_body,"001234"));
    response(assignment);assert(s_room.assigned && !strcmp(s_room.room_code,"001234") && !s_active);
    /* Repeated notices coalesce, but invalidate an already in-flight read. */
    s_room.due=clock_ms;room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    unsigned before_notices=requests;
    for(unsigned i=0;i<100;i++){room_invalidate_assignment();room_tick(clock_ms);}
    assert(requests==before_notices);response(assignment);assert(s_room.needs_sync);
    room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC && requests==before_notices+1);
    response(assignment);assert(!s_room.needs_sync);
    join();assert(s_room.count==2 && s_room.members[0].self && s_room.members[1].speaking);
    uint32_t generation=s_room.generation;
    assert(starter_runtime_room_ptt(generation,true)==0);assert(uplink_session_current(STARTER_TIRTC_ROOM,generation));
    room_tick(clock_ms);assert(s_room.mic_announced && strstr(last_command,"speaking"));
    unsigned mute_epoch=s_mute_epoch;queue_full=true;
    assert(starter_runtime_room_ptt(generation,false)==0);assert(!uplink_session_current(STARTER_TIRTC_ROOM,generation));
    assert(s_mute_epoch>mute_epoch && !s_microphone_muted);queue_full=false;
    room_tick(clock_ms);assert(!s_room.mic_announced);
    assert(starter_runtime_room_ptt(generation-1,true)==ESP_ERR_INVALID_STATE);
    signal("{\"method\":\"participant_left\",\"params\":{\"room_id\":\"wrong:group_room_1\",\"participant_id\":\"peer-p\"}}");
    assert(s_room.count==2);
    signal("{\"method\":\"participant_left\",\"params\":{\"room_id\":\"app:group_room_1\",\"participant_id\":\"peer-p\"}}");assert(s_room.count==1);
    signal(snapshot);assert(s_room.count==2);
    /* Self may be outside participants, but is still shown exactly once. */
    signal("{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"app:group_room_1\",\"self\":{\"participant_id\":\"self-p\",\"device_id\":\"self\"},\"participants\":[{\"participant_id\":\"peer-p\",\"device_id\":\"peer\",\"mic_state\":\"off\"}]}}");
    assert(s_room.count==2 && s_room.members[1].self);
    signal(snapshot);
    room_publish();starter_room_view_t view;assert(starter_runtime_room_read(99,&view)&&view.offset==0&&view.count==2);
    /* Optional self metadata is never replaced by a hard-coded product name. */
    signal("{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"app:group_room_1\",\"self\":{\"participant_id\":\"self-p\",\"device_id\":\"self\",\"device_name\":\"Living room\"},\"participants\":[{\"participant_id\":\"self-p\",\"device_id\":\"self\"}]}}");
    room_publish();assert(starter_runtime_room_read(0,&view)&&!strcmp(view.self_name,"Living room"));
    signal("{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"app:group_room_1\",\"self\":{\"participant_id\":\"self-p\",\"device_id\":\"self\",\"device_name\":\"\"},\"participants\":[]}}");
    room_publish();assert(starter_runtime_room_read(0,&view)&&!view.self_name[0]);
    signal(snapshot);
    /* A temporary failed heartbeat preserves audio, but cannot extend a lease. */
    int64_t lease=s_room.lease_deadline;
    s_room.heartbeat_due=clock_ms;room_tick(clock_ms);assert(s_room.pending==ROOM_PRESENCE);
    response("{\"code\":50000}");assert(s_active && s_room.lease_deadline==lease && s_room.error==50000);
    clock_ms+=2000;room_tick(clock_ms);assert(s_room.pending==ROOM_PRESENCE);
    atomic_store(&s_room_response_lost,s_room.ticket-1);room_tick(clock_ms);
    assert(s_active && s_room.pending==ROOM_PRESENCE);
    atomic_store(&s_room_response_lost,s_room.ticket);room_tick(clock_ms);
    assert(s_active && !s_room.pending && s_room.lease_deadline==lease);
    clock_ms+=2000;room_tick(clock_ms);assert(s_room.pending==ROOM_PRESENCE);
    int64_t renewed_at=clock_ms;clock_ms+=1000;
    response("{\"code\":200}");assert(s_active && s_room.error==0 && s_room.lease_deadline>lease);
    assert(s_room.lease_deadline==renewed_at+s_room.lease_ms);
    /* Stale assignment responses neither tear down nor cause a tight poll loop. */
    s_room.due=clock_ms;room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    response("{\"code\":200,\"data\":{\"assignment_version\":0,\"desired_state\":\"\"}}");
    unsigned calls=requests;room_tick(clock_ms);assert(s_active && requests==calls && s_room.due>clock_ms);
    s_room.needs_sync=false;s_room.due=clock_ms+30000;
    /* A full-capacity snapshot must fit the ingress bound and survive parsing
     * with maximum-width IDs; UI publication remains a three-row copy. */
    cJSON *large=cJSON_Parse(snapshot);
    cJSON *params=cJSON_GetObjectItem(large,"params");
    cJSON *members=cJSON_GetObjectItem(params,"participants");
    cJSON_DeleteItemFromArray(members,1);
    for(unsigned i=1;i<100;i++) {
        cJSON *item=cJSON_CreateObject();char pid[97],device[65];
        memset(pid,'p',96);pid[96]=0;pid[0]='0'+i/10;pid[1]='0'+i%10;
        memset(device,'d',64);device[64]=0;device[0]='0'+i/10;device[1]='0'+i%10;
        assert(cJSON_AddStringToObject(item,"participant_id",pid));
        assert(cJSON_AddStringToObject(item,"device_id",device));
        assert(cJSON_AddStringToObject(item,"mic_state","speaking"));
        assert(cJSON_AddItemToArray(members,item));
    }
    char *large_json=cJSON_PrintUnformatted(large);assert(strlen(large_json)<=32768);
    signal(large_json);assert(s_active && s_room.count==100 && json_live==0);
    room_publish();assert(starter_runtime_room_read(99,&view)&&view.count==1&&view.offset==99);
    cJSON_free(large_json);cJSON_Delete(large);signal(snapshot);
    /* A call preempts without deleting the assignment; old events cannot revive it. */
    finish_session(0);assert(s_room.assigned && !s_active && strstr(last_body,"suspended"));
    publish_state(STARTER_RUNTIME_CALL_ACTIVE);room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    response(assignment);room_tick(clock_ms);assert(s_room.pending==0 && !s_active);
    runtime_event_t stale={.mode=STARTER_TIRTC_ROOM,.generation=generation,.request_tag=s_session_generation,.flag=true};
    room_connection(&stale);assert(!s_active);
    publish_state(STARTER_RUNTIME_WAITING);join();
    /* SDK established disconnect uses request_tag=0, generation is authoritative. */
    runtime_event_t closed={.mode=STARTER_TIRTC_ROOM,.generation=s_room.generation};
    room_connection(&closed);assert(!s_active && s_room.phase==STARTER_ROOM_ERROR);
    clock_ms=s_room.retry_due;room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);response(assignment);join();
    /* Invalidated queued PCM stays invalid after a quick release/repress. */
    generation=s_room.generation;assert(starter_runtime_room_ptt(generation,true)==0);mute_epoch=s_mute_epoch;
    starter_runtime_room_ptt(generation,false);starter_runtime_room_ptt(generation,true);assert(mute_epoch!=s_mute_epoch);
    /* Lease expiry does not pretend to remain joined. */
    clock_ms=s_room.lease_deadline;room_tick(clock_ms);assert(!s_active && s_room.error==ESP_ERR_TIMEOUT);
    clock_ms=s_room.retry_due;room_tick(clock_ms);response(assignment);join();
    /* Leave supersedes a metadata read, releases media then changes relation. */
    s_room.due=clock_ms;room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    uint32_t old_ticket=s_room.ticket;
    assert(starter_runtime_room_leave()==0);room_intent(&queued);assert(s_room.pending==ROOM_LEAVE && !s_active);
    runtime_event_t late={.request_tag=old_ticket,.text=(char*)assignment,.length=strlen(assignment)};
    room_http(&late);assert(s_room.pending==ROOM_LEAVE);
    response("{\"code\":200,\"data\":{\"desired_state\":\"left\",\"assignment_version\":2}}");assert(!s_room.assigned);
    /* New identity cannot reuse old assignment/version/queued operations. */
    epoch++;room_tick(clock_ms);assert(s_room.version==0 && s_room.pending==ROOM_SYNC);
    /* Reject fractional assignment versions; no silent integer truncation. */
    response("{\"code\":200,\"data\":{\"assignment_version\":1.5,\"desired_state\":\"\"}}");
    assert(s_room.error==ESP_ERR_INVALID_RESPONSE && !s_active);
    clock_ms=s_room.retry_due;room_tick(clock_ms);response(assignment);join();
    /* Refused negotiation never enables capture or playout. */
    finish_session(0);room_tick(clock_ms);response(assignment);
    room_tick(clock_ms);assert(s_room.pending==ROOM_TOKEN);response(token);
    runtime_event_t connected={.request_tag=s_session_generation,.generation=s_session_generation+10,.flag=true};
    room_connection(&connected);
    char bad_ack[512];snprintf(bad_ack,sizeof(bad_ack),"{\"id\":%u,\"result\":{\"session_id\":\"x\",\"input_audio\":{\"codec\":\"pcm\",\"sample_rate\":16000,\"channels\":1},\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}",s_session_generation);
    signal(bad_ack);assert(!s_active && s_room.error==ESP_ERR_INVALID_RESPONSE);
    clock_ms=s_room.retry_due;room_tick(clock_ms);response(assignment);join();
    /* Protocol notifications do not accidentally use request IDs. */
    finish_session(0);assert(strstr(last_command,"leave_room") && !strstr(last_command,"\"id\""));
    /* Every command-builder OOM releases JSON and cannot return false success. */
    for(unsigned i=1;i<25;i++) {
        fail_at=i;attempts=0;int ret=room_send("leave_room",NULL,false);
        if(attempts>=i)assert(ret<0);
        assert(json_live==0);
    }
    fail_at=0;
    char deep[66];memset(deep,'[',32);deep[32]='0';memset(deep+33,']',32);deep[65]=0;
    assert(!room_json_cJSON_Parse(deep));assert(json_live==0);
    /* The private parser's hooks/recursion bound cannot affect other users. */
    cJSON *ordinary=cJSON_Parse(deep);assert(ordinary);cJSON_Delete(ordinary);
    ordinary=cJSON_Parse("{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1.5}");
    assert(!room_audio_profile(ordinary));cJSON_Delete(ordinary);
    /* Closing an app keeps assignment, but stops media/lease/polling. It must
     * also work with a full event queue and a connecting/token request. */
    room_tick(clock_ms);response(assignment);join();
    generation=s_room.generation;
    assert(starter_runtime_room_ptt(generation,true)==0);
    queue_full=true;starter_runtime_room_set_open(false);
    assert(!uplink_session_current(STARTER_TIRTC_ROOM,generation));
    room_tick(clock_ms);queue_full=false;
    assert(!s_active && s_room.assigned && s_room.phase==STARTER_ROOM_OFFLINE);
    assert(strstr(last_path,"/presence") && strstr(last_body,"\"left\""));
    unsigned closed_requests=requests, closed_commands=commands, closed_disconnects=disconnects;
    for(unsigned i=0;i<100;i++){clock_ms+=1000;room_invalidate_assignment();room_tick(clock_ms);}
    assert(requests==closed_requests && commands==closed_commands && disconnects==closed_disconnects);
    publish_state(STARTER_RUNTIME_AI_ACTIVE);s_active=true;mute_epoch=s_mute_epoch;
    starter_runtime_room_set_open(false);room_tick(clock_ms);
    assert(s_public_state==STARTER_RUNTIME_AI_ACTIVE && s_active && disconnects==closed_disconnects);
    assert(s_mute_epoch==mute_epoch);
    s_active=false;publish_state(STARTER_RUNTIME_WAITING);
    starter_runtime_room_set_open(true);room_tick(clock_ms);
    assert(s_room.pending==ROOM_SYNC && !s_active);response(assignment);join();
    generation=s_room.generation;unsigned old_disconnects=disconnects;
    /* Close/reopen entirely between owner ticks still discards the old RTC. */
    starter_runtime_room_set_open(false);starter_runtime_room_set_open(true);
    assert(starter_runtime_room_ptt(generation,true)==ESP_ERR_INVALID_STATE);
    room_tick(clock_ms);assert(disconnects==old_disconnects+1 && s_room.pending==ROOM_SYNC);
    response(assignment);room_tick(clock_ms);assert(s_room.pending==ROOM_TOKEN);
    old_ticket=s_room.ticket;
    starter_runtime_room_set_open(false);room_tick(clock_ms);
    late=(runtime_event_t){.request_tag=old_ticket,.text=(char*)token,.length=strlen(token)};
    room_http(&late);assert(!s_active && !s_room.pending && !s_room.generation);
    /* A queued old-page CRUD intent cannot execute after closing/reopening. */
    starter_runtime_room_set_open(true);room_apply_app_request();
    assert(starter_runtime_room_refresh()==0);
    starter_runtime_room_set_open(false);starter_runtime_room_set_open(true);
    room_apply_app_request();closed_requests=requests;room_intent(&queued);
    assert(requests==closed_requests);release_event(&queued);
    starter_runtime_room_set_open(false);room_tick(clock_ms);
    starter_runtime_room_set_open(true);room_tick(clock_ms);response(assignment);
    room_tick(clock_ms);assert(s_room.pending==ROOM_TOKEN);
    response("{\"code\":40300}");
    assert(s_room.access_denied && s_room.assigned && !s_active && !room_owns_media());
    denied_requests=requests;
    for(unsigned i=0;i<12;i++){clock_ms+=5000;room_tick(clock_ms);}
    assert(requests==denied_requests && s_room.error==40300);
    assert(starter_runtime_room_refresh()==0);room_intent(&queued);release_event(&queued);
    room_tick(clock_ms);response(assignment);
    assert(!s_room.error && !s_room.access_denied);join();
    generation=s_room.generation;assert(starter_runtime_room_ptt(generation,true)==0);
    s_room.heartbeat_due=clock_ms;room_tick(clock_ms);assert(s_room.pending==ROOM_PRESENCE);
    response("{\"code\":40300}");
    assert(s_room.access_denied && s_room.assigned && !s_active && !s_room.generation);
    assert(!uplink_session_current(STARTER_TIRTC_ROOM,generation));
    denied_requests=requests;closed_disconnects=disconnects;
    publish_state(STARTER_RUNTIME_AI_ACTIVE);s_active=true;
    for(unsigned i=0;i<12;i++){clock_ms+=5000;room_tick(clock_ms);}
    assert(requests==denied_requests && disconnects==closed_disconnects);
    assert(s_public_state==STARTER_RUNTIME_AI_ACTIVE && s_active);
    s_active=false;publish_state(STARTER_RUNTIME_WAITING);
    room_invalidate_assignment();room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC);
    response("{\"code\":40300}");
    starter_runtime_room_set_open(false);starter_runtime_room_set_open(true);
    room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC && !s_room.access_denied);
    response("{\"code\":40300}");
    wifi=false;room_tick(clock_ms);wifi=true;room_tick(clock_ms);
    assert(s_room.pending==ROOM_SYNC && !s_room.access_denied);
    response("{\"code\":40300}");
    epoch++;room_tick(clock_ms);assert(s_room.pending==ROOM_SYNC && !s_room.access_denied);
    response(assignment);join();
    starter_runtime_room_set_open(false);room_tick(clock_ms);
    assert(json_live==0);assert(disconnects>=4);
    printf("PASS: Room owner, optional self name, foreground open/close, no background polls, stale-page intents, CRUD, PTT, lease, other-owner isolation, denial and recovery\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="s3-room-") as directory:
    temp = Path(directory)
    (temp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_NO_MEM 257\n#define ESP_ERR_INVALID_ARG 258\n#define ESP_ERR_INVALID_STATE 259\n"
        "#define ESP_ERR_INVALID_SIZE 260\n#define ESP_ERR_INVALID_RESPONSE 264\n#define ESP_ERR_TIMEOUT 263\n")
    (temp / "test.c").write_text(code)
    cjson = ROOT / "managed_components/espressif__cjson/cJSON"
    (temp / "headers.cmake").write_text(f'cmake_minimum_required(VERSION 3.16)\ninclude("{ROOT}/cmake/room_json.cmake")\n'
        f'starter_room_json_headers("{cjson}" "{temp}")\n')
    subprocess.run(["cmake", "-P", str(temp / "headers.cmake")], check=True)
    (temp / "esp_heap_caps.h").write_text('#include <assert.h>\n#include <stdlib.h>\n'
        '#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n'
        'static void *heap_caps_malloc(size_t n,int c){assert(c==3);return malloc(n);}\n'
        'static void *heap_caps_realloc(void*p,size_t n,int c){assert(c==3);return realloc(p,n);}\n')
    subprocess.run(["cc", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(temp), "-I", str(cjson), "-I", str(ROOT / "components/starter_runtime/include"),
        "-I", str(ROOT / "components/starter_tirtc/include"), str(temp / "test.c"), str(cjson / "cJSON.c"),
        str(ROOT / "components/starter_runtime/src/room_json.c"),
        "-o", str(temp / "test")], check=True)
    subprocess.run([str(temp / "test")], check=True, timeout=15)
