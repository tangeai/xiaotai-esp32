#!/usr/bin/env python3
"""Run the actual query owner and JSON views with ASan and a bounded HTTP fake."""
from pathlib import Path
import argparse
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk-header', type=Path,
                    default=project / 'third_party/tirtc/include/tirtc/tiRTC.h')
parser.add_argument('--cjson-dir', type=Path,
                    default=project / 'managed_components/espressif__cjson/cJSON')
args = parser.parse_args()
source = (project / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')

def function(name):
    pos = source.index('\nstatic ', source.index(name) - 80)
    while name not in source[pos:source.index('\n{', pos)]:
        pos = source.index('\nstatic ', pos + 1)
    return source[pos:source.index('\n}', source.index('\n{', pos)) + 2]

code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_SIZE -3
#define RUNTIME_TEXT_MAX 4096U
#define AI_COMMAND 0x2100
#define CONTACT_QUERY_RESULT 100
#define EVENT_CONTACT_QUERY_RESULT 100
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define STARTER_RUNTIME_AI_ACTIVE 3
#define PLATFORM_SERVICE_CALL 1
typedef int esp_err_t;
typedef struct { int type, error; uint32_t request_tag, length; char *text; } runtime_event_t;
static const char *TAG="test";
static atomic_int s_public_state=STARTER_RUNTIME_AI_ACTIVE;
static uint32_t s_session_generation=7, s_connection_generation=9;
static int64_t clock_ms=100;
static int requests, replies, submission_error, no_memory, allocations, queue_full;
static bool forbid_json_allocation;
static char response[4096];
static runtime_event_t queued;
static void (*callback)(const char *,void *);
static void *callback_tag;
static int64_t now_ms(void) { return clock_ms; }
static void *json_alloc(size_t n) { assert(!forbid_json_allocation); return malloc(n); }
static void *heap_caps_malloc(size_t n,int caps) {
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(no_memory) return NULL;
 void *p=malloc(n); if(p) ++allocations; return p;
}
static void *heap_caps_calloc(size_t n,size_t s,int caps) {
 void *p=heap_caps_malloc(n*s,caps); if(p)memset(p,0,n*s); return p;
}
static void heap_caps_free(void *p) { if(p) --allocations; free(p); }
static void release_event(runtime_event_t *e) { heap_caps_free(e->text); e->text=NULL; }
static bool queue_event(const runtime_event_t *e) { if(queue_full)return false; queued=*e; return true; }
static int starter_tirtc_send_command(int cmd,const void *data,uint32_t size) {
 assert(cmd==AI_COMMAND && size<sizeof(response));
 memcpy(response,data,size);response[size]=0;++replies;return (int)size;
}
static int platform_client_request_timeout(int service,const char *path,const char *body,
 unsigned ms,void (*cb)(const char *,void *),void *tag) {
 assert(service==PLATFORM_SERVICE_CALL && !strcmp(path,"/v1/call/device/contacts") &&
        body==NULL && ms==4000); ++requests; callback=cb;callback_tag=tag; return submission_error;
}
'''
sdk_header = args.sdk_header.read_text(encoding='utf-8')
for name in ('GET_CMD', 'MAKE_CMDW', 'RESPONSE_BIT'):
    code += next(line for line in sdk_header.splitlines()
                 if line.startswith('#define ' + name)) + '\n'
adapter = (project / 'components/starter_tirtc/src/starter_tirtc.c').read_text(encoding='utf-8')
start = adapter.index('bool starter_tirtc_is_ai_command(')
code += adapter[start:adapter.index('\n}', start) + 2] + '\n'
for name in ('response_ok', 'normalize_contact_name', 'action_payload'):
    code += function(name) + '\n'
code += (project / 'components/starter_runtime/src/starter_contact_query.inc').read_text(encoding='utf-8')
code += r'''
static void begin(const char *filter,const char *target,const char *id) {
 cJSON *params=cJSON_CreateObject(), *data=cJSON_AddObjectToObject(params,"data");
 cJSON *rid=cJSON_CreateString(id);
 if(filter)cJSON_AddStringToObject(data,"status_filter",filter);
 if(target)cJSON_AddStringToObject(data,"target",target);
 forbid_json_allocation=true; start_contact_query(rid,params); forbid_json_allocation=false;
 cJSON_Delete(rid); cJSON_Delete(params);
}
static void deliver(const char *body) {
 callback(body,callback_tag);
 if(queue_full)return;
 if(contact_query_current(queued.request_tag)) {
   cJSON *root=queued.text?cJSON_Parse(queued.text):NULL;
   forbid_json_allocation=true;complete_contact_query(&queued,root);forbid_json_allocation=false;
   cJSON_Delete(root);
 }
 release_event(&queued);
}
static cJSON *result(const char *status,unsigned count) {
 cJSON *r=cJSON_Parse(response); assert(r);
 cJSON *v=cJSON_GetObjectItem(r,"result");
 cJSON *error=cJSON_GetObjectItem(r,"error");
 if(strcmp(status,"ok")) {
   assert(!v && cJSON_IsObject(error));
   int code=cJSON_GetObjectItem(error,"code")->valueint;
   assert(code==(!strcmp(status,"invalid_params")?-32602:-32000));
   v=cJSON_GetObjectItem(error,"data");assert(cJSON_IsFalse(cJSON_GetObjectItem(v,"ok")));
 } else {assert(!error && v);assert(cJSON_IsTrue(cJSON_GetObjectItem(v,"ok")));}
 assert(v);
 assert(!strcmp(cJSON_GetObjectItem(v,"status")->valuestring,status));
 assert((unsigned)atoi(cJSON_GetObjectItem(v,"count")->valuestring)==count);
 return r;
}
static const char *body="{\"code\":200,\"data\":{\"contacts\":["
 "{\"type\":\"device\",\"device_id\":\"d_A\",\"remark\":\"小王\",\"online\":true},"
 "{\"type\":\"device\",\"device_id\":\"d_B\",\"remark\":\"Alice\",\"online\":false},"
 "{\"type\":\"voip\",\"device_id\":\"wx_C\",\"remark\":\"妈妈\"}]}}";
int main(void) {
 (void)TAG;
 for(uint32_t sn=0;sn<=0xffffU;++sn) {
   uint32_t raw=(sn<<16)|AI_COMMAND, encoded=MAKE_CMDW(AI_COMMAND,sn);
   assert(starter_tirtc_is_ai_command(raw));
   assert(starter_tirtc_is_ai_command(raw|RESPONSE_BIT));
   assert(starter_tirtc_is_ai_command(encoded));
   assert(starter_tirtc_is_ai_command(encoded|RESPONSE_BIT));
 }
 for(uint32_t low=0;low<=0xffffU;++low) {
   uint32_t word=0xa55a0000U|low;
   bool legacy=GET_CMD(word)==AI_COMMAND || GET_CMD(word&~RESPONSE_BIT)==AI_COMMAND ||
       word==AI_COMMAND || (word&~RESPONSE_BIT)==AI_COMMAND ||
       low==AI_COMMAND || (low&~RESPONSE_BIT)==AI_COMMAND;
   assert(starter_tirtc_is_ai_command(word)==legacy);
 }
 cJSON_Hooks hooks={.malloc_fn=json_alloc,.free_fn=free};cJSON_InitHooks(&hooks);
 begin(NULL,NULL,"req-online");assert(s_contact_query && !replies);deliver(body);
 cJSON *r=result("ok",2);assert(!strcmp(cJSON_GetObjectItem(r,"id")->valuestring,"req-online"));
 cJSON *items=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"result"),"contacts");
 assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"online_known")));
 assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(items,1),"online_known")));cJSON_Delete(r);
 begin("offline",NULL,"off");deliver(body);cJSON_Delete(result("ok",1));
 begin("all",NULL,"all");deliver(body);cJSON_Delete(result("ok",3));
 begin("all","d_B","by-id");deliver(body);cJSON_Delete(result("ok",1));
 begin(NULL,"d_B","offline-target");deliver(body);r=result("ok",1);
 items=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"result"),"contacts");
 assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"online")));cJSON_Delete(r);
 begin("all"," aLiCe ","by-alias");deliver(body);cJSON_Delete(result("ok",1));
 begin("all","小王","中文\"id");deliver(body);r=result("ok",1);
 assert(!strcmp(cJSON_GetObjectItem(r,"id")->valuestring,"中文\"id"));cJSON_Delete(r);
 begin("all","absent","none");deliver(body);cJSON_Delete(result("not_found",0));
 begin(NULL,"妈妈","wechat-name");deliver(body);r=result("ok",1);
 items=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"result"),"contacts");
 assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"type")->valuestring,"voip"));
 assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"online")));
 assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"online_known")));cJSON_Delete(r);
 begin("offline","wx_C","wechat-id");deliver(body);cJSON_Delete(result("ok",1));
 const char *duplicate="{\"code\":200,\"data\":{\"contacts\":["
 "{\"type\":\"device\",\"device_id\":\"d_A\",\"remark\":\"妈妈\",\"online\":true},"
 "{\"type\":\"voip\",\"device_id\":\"wx_C\",\"remark\":\"妈妈\"}]}}";
 begin(NULL,"妈妈","same-name");deliver(duplicate);cJSON_Delete(result("ambiguous",0));
 begin(NULL,"wx_C","same-name-by-id");deliver(duplicate);cJSON_Delete(result("ok",1));
 begin("invalid",NULL,"bad");cJSON_Delete(result("invalid_params",0));assert(!s_contact_query);
 begin("all",NULL,"first");int before=requests;
 begin("all",NULL,"second");assert(requests==before);cJSON_Delete(result("busy",0));
 deliver(body);r=result("ok",3);assert(!strcmp(cJSON_GetObjectItem(r,"id")->valuestring,"first"));cJSON_Delete(r);
 begin("all",NULL,"timeout");void *oldtag=callback_tag;clock_ms+=10001;contact_query_tick();
 cJSON_Delete(result("timeout",0));before=replies;
 callback(body,oldtag);assert(!contact_query_current(queued.request_tag));release_event(&queued);assert(replies==before);
 begin("all",NULL,"old-session");++s_session_generation;contact_query_tick();assert(!s_contact_query);
 deliver(body);assert(replies==before);
 begin("all",NULL,"new-session");callback(body,oldtag);assert(!contact_query_current(queued.request_tag));
 release_event(&queued);deliver(body);cJSON_Delete(result("ok",3));
 submission_error=-5;begin("all",NULL,"submit");cJSON_Delete(result("unavailable",0));submission_error=0;
 no_memory=1;begin("all",NULL,"oom");cJSON_Delete(result("no_memory",0));no_memory=0;
 begin("all",NULL,"transport");deliver(NULL);cJSON_Delete(result("upstream_error",0));
 begin("all",NULL,"malformed");deliver("{}");cJSON_Delete(result("upstream_error",0));
 const char *unknown="{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"a\"}]}}";
 begin(NULL,NULL,"missing-online");deliver(unknown);cJSON_Delete(result("status_unavailable",0));
 begin("all",NULL,"unknown");deliver(unknown);r=result("ok",1);
 items=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"result"),"contacts");
 assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"online")));cJSON_Delete(r);
 begin("all",NULL,"queue");queue_full=1;deliver(body);queue_full=0;
 clock_ms+=10001;contact_query_tick();cJSON_Delete(result("timeout",0));
 char huge[4098];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
 begin("all",NULL,"huge");deliver(huge);cJSON_Delete(result("response_too_large",0));
 begin("all",NULL,"callback-oom");no_memory=1;deliver(body);no_memory=0;
 cJSON_Delete(result("upstream_error",0));assert(!allocations);
 begin("all",NULL,"old-connection");before=replies;++s_connection_generation;
 contact_query_tick();deliver(body);assert(replies==before && !allocations);
 begin("all",NULL,"inactive");atomic_store(&s_public_state,0);
 contact_query_tick();deliver(body);assert(replies==before && !allocations);
 atomic_store(&s_public_state,STARTER_RUNTIME_AI_ACTIVE);
 cJSON numeric={.type=cJSON_Number,.valuedouble=42,.valueint=42};
 forbid_json_allocation=true;start_contact_query(&numeric,NULL);forbid_json_allocation=false;
 deliver(body);r=result("ok",2);assert(cJSON_GetObjectItem(r,"id")->valueint==42);cJSON_Delete(r);
 cJSON *many=cJSON_CreateObject(), *many_data=cJSON_AddObjectToObject(many,"data");
 cJSON_AddNumberToObject(many,"code",200);
 cJSON *many_rows=cJSON_AddArrayToObject(many_data,"contacts");
 for(int i=0;i<17;++i) {
   cJSON *row=cJSON_CreateObject();cJSON_AddItemToArray(many_rows,row);
   cJSON_AddStringToObject(row,"type","device");cJSON_AddStringToObject(row,"device_id","d");
   cJSON_AddBoolToObject(row,"online",true);
 }
 char *many_text=cJSON_PrintUnformatted(many);assert(strlen(many_text)<4096);
 begin("all",NULL,"too-many");deliver(many_text);cJSON_Delete(result("response_too_large",0));
 cJSON_free(many_text);cJSON_Delete(many);
 for(int i=0;i<500;++i){begin("all",NULL,"repeat");deliver(body);assert(!s_contact_query && !allocations);}
 assert(atomic_load(&s_public_state)==STARTER_RUNTIME_AI_ACTIVE && !allocations);
 puts("PASS: fresh async query, filters/alias/id, assumed WeChat status, errors, timeout, stale reply, PSRAM-only new storage, 500 repeats; no call/session mutation");
}
'''
cjson = args.cjson_dir
with tempfile.TemporaryDirectory(prefix='contact-query-') as d:
    path = Path(d)
    (path/'test.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
                    '-I',str(cjson),str(path/'test.c'),str(cjson/'cJSON.c'),'-lm','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
owner=source[source.index('static void handle_ai_command('):source.index('static void end_ai_session(')]
assert 'event->command != AI_COMMAND' not in owner
assert owner.index('event->generation != s_connection_generation') < owner.index('starter_tirtc_is_ai_command(')
assert owner.index('starter_tirtc_is_ai_command(event->command)') < owner.index('cJSON_ParseWithLength(')
assert owner.index('start_contact_query(id, params)') < owner.index('parse_ai_call_action(')
query=(project/'components/starter_runtime/src/starter_contact_query.inc').read_text(encoding='utf-8')
assert 'dial_contact(' not in query and 'finish_session(' not in query and 'end_session' not in query
