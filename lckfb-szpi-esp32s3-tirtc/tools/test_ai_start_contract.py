#!/usr/bin/env python3
"""Exercise production AI RPC construction and audio profiles with real cJSON."""
from pathlib import Path
import subprocess
import tempfile

from test_voice_reliability import function

root = Path(__file__).resolve().parents[1]
source = "components/starter_runtime/src/starter_runtime.c"
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define AI_COMMAND 0x2100
#define AI_RESPONSE_TIMEOUT_MS 10000
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGE ESP_LOGI
#define ESP_LOGW ESP_LOGI
static uint32_t s_connection_generation=3, s_session_generation=5, s_ai_rpc_sent_ms;
static int64_t s_deadline_ms, s_diagnostic_ai_started_ms;
static char s_ai_request_id[24], s_device_id[65]="test-device", s_ai_role_id[65]="test-role";
static bool connected=true, fail_alloc;
static unsigned calls, allocations, next_id, allocation_attempts, fail_at;
static int send_error, ended;
static char request[1024];
static int64_t now_ms(void) { return 1234; }
static bool starter_tirtc_connected(void) { return connected; }
static uint32_t esp_random(void) { return ++next_id; }
static void finish_session(int error) { ended=error; s_deadline_ms=0; }
static void *allocate(size_t n) {
    allocation_attempts++;
    if(fail_alloc || (fail_at && allocation_attempts==fail_at)) return NULL;
    void *p=malloc(n); if(p)allocations++; return p;
}
static void release(void *p) { if(p) { assert(allocations); allocations--; free(p); } }
static int starter_tirtc_send_command(unsigned cmd, const void *data, uint32_t len) {
    assert(cmd==AI_COMMAND && len<sizeof(request));
    memcpy(request,data,len); request[len]=0; calls++;
    return send_error ? send_error : (int)len;
}
'''
code += function(source, "send_ai_start")
code += function(source, "ai_audio_profile_valid")
code += function(source, "send_device_action_result")
code += r'''
int main(void) {
    cJSON_Hooks hooks={allocate,release}; cJSON_InitHooks(&hooks);
    send_ai_start();
    assert(calls==1 && !ended && !allocations && s_deadline_ms==11234 && s_ai_rpc_sent_ms==1234);
    unsigned construction_allocations=allocation_attempts;
    cJSON *root=cJSON_Parse(request); assert(root);
    assert(!strcmp(cJSON_GetObjectItem(root,"method")->valuestring,"start_session"));
    assert(!strcmp(cJSON_GetObjectItem(root,"id")->valuestring,s_ai_request_id));
    cJSON *params=cJSON_GetObjectItem(root,"params");
    assert(!strcmp(cJSON_GetObjectItem(params,"device_id")->valuestring,"test-device"));
    assert(!strcmp(cJSON_GetObjectItem(params,"role_id")->valuestring,"test-role"));
    cJSON *input=cJSON_GetObjectItem(params,"input_audio");
    cJSON *output=cJSON_GetObjectItem(params,"output_audio");
    assert(ai_audio_profile_valid(input) && ai_audio_profile_valid(output));
    cJSON_SetNumberValue(cJSON_GetObjectItem(input,"channels"),2);
    assert(!ai_audio_profile_valid(input));
    cJSON_SetNumberValue(cJSON_GetObjectItem(input,"channels"),1);
    cJSON_SetNumberValue(cJSON_GetObjectItem(input,"sample_rate"),16000);
    assert(!ai_audio_profile_valid(input));
    cJSON_SetNumberValue(cJSON_GetObjectItem(input,"sample_rate"),8000);
    assert(ai_audio_profile_valid(input) && !ai_audio_profile_valid(NULL));
    cJSON_Delete(root); assert(!allocations);
    send_error=-9; send_ai_start();
    assert(calls==2 && ended==-9 && !allocations && !s_deadline_ms);
    fail_alloc=true; send_ai_start();
    assert(calls==2 && ended==ESP_ERR_NO_MEM && !allocations);
    fail_alloc=false; connected=false; send_ai_start();
    assert(calls==2 && ended==ESP_ERR_INVALID_STATE && !allocations);
    connected=true; s_connection_generation=0; send_ai_start();
    assert(calls==2 && ended==ESP_ERR_INVALID_STATE && !allocations);
    s_connection_generation=3; send_error=0;
    for(unsigned i=1; i<=construction_allocations; i++) {
        calls=0; ended=0; allocation_attempts=0; fail_at=i;
        send_ai_start();
        if(calls || ended!=ESP_ERR_NO_MEM || allocations) {
            fprintf(stderr,"allocation failure %u: sends=%u ended=%d leaked=%u\n", i,calls,ended,allocations);
            abort();
        }
    }
    fail_at=0; ended=0;
    send_ai_start();
    assert(calls==1 && !ended && !allocations && s_deadline_ms==11234);
    printf("PASS: every AI RPC allocation failure (%u sites) releases ownership and never sends partial JSON\n",construction_allocations);
    cJSON id={.type=cJSON_String,.valuestring="request-7"};
    calls=0; allocation_attempts=0;
    send_device_action_result(&id,true,"ok","completed");
    unsigned reply_allocations=allocation_attempts;
    assert(calls==1 && !allocations);
    root=cJSON_Parse(request); assert(root);
    assert(!strcmp(cJSON_GetObjectItem(root,"id")->valuestring,"request-7"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"result"),"ok")));
    cJSON_Delete(root);
    for(unsigned i=1; i<=reply_allocations; i++) {
        calls=0; allocation_attempts=0; fail_at=i;
        send_device_action_result(&id,false,"busy","not available");
        if(calls || allocations) {
            fprintf(stderr,"action reply allocation failure %u: sends=%u leaked=%u\n",i,calls,allocations);
            abort();
        }
    }
    fail_at=0; calls=0;
    send_device_action_result(NULL,true,"ok","notification");
    assert(!calls && !allocations);
    id=(cJSON){.type=cJSON_Number,.valueint=42,.valuedouble=42};
    send_device_action_result(&id,false,"busy","not available");
    assert(calls==1 && !allocations);
    root=cJSON_Parse(request); assert(root);
    assert(cJSON_GetObjectItem(root,"id")->valueint==42);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"result"),"ok")));
    cJSON_Delete(root);
    send_error=-8; send_device_action_result(&id,false,"busy","not available");
    assert(calls==2 && !allocations);
    printf("PASS: every action reply allocation failure (%u sites), notification and string/numeric IDs\n",reply_allocations);
    puts("PASS: AI RPC generation, exact mono A-law profile, send/allocation/disconnect errors and cleanup");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ai-start-") as temp:
    directory = Path(temp)
    (directory / "test.c").write_text(code, encoding="utf-8")
    cjson = root / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(cjson),
                    str(directory / "test.c"), str(cjson / "cJSON.c"),
                    "-lm", "-o", str(directory / "test")], check=True)
    subprocess.run([str(directory / "test")], check=True, timeout=10)
