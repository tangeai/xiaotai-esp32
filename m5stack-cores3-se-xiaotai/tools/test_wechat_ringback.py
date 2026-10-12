#!/usr/bin/env python3
"""Replay the real MQTT call_incoming owner: WeChat ring-back correlation.

The production handler is extracted verbatim and linked with cJSON. Fakes only
record reject/connect side effects; MQTT, HTTP and the SDK are not exercised.
"""
from pathlib import Path
import re
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')


def function(name):
    match = re.search(r'^static [^;{}\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, cursor = 1, match.end()
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[match.start():cursor]


code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define CONFIG_IDF_TARGET_ESP32P4 0
#include "starter_runtime.h"
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_ERR_TIMEOUT 0x107
#define PLATFORM_SERVICE_CALL 0
#define CALL_CONNECT_TIMEOUT_MS 30000
#define CALL_PENDING_TIMEOUT_MS 45000
enum { EVENT_VOIP_CONNECT = 1 };
typedef struct { int type; char *text; uint32_t length; } runtime_event_t;
static atomic_int s_public_state;
static uint32_t s_session_generation;
static int64_t s_deadline_ms;
static char s_device_id[65] = "dev1";
static char s_call_room_id[129], s_call_id[65], s_call_peer_id[65], s_call_peer_name[65];
static char s_call_wx_app_id[65], s_call_wx_model_id[65];
static char s_call_connect_peer[1024], s_call_connect_token[1024];
static char s_call_wx_session_token[257], s_call_wx_payload[513];
static bool s_call_wechat, s_call_outgoing, s_call_peer_answered, s_call_p2p_connected;
static bool s_voip_profile_ready = true;
static int rejects, last_reason, connects, device_rejects, finishes, incoming;
static char rejected_room[129], device_reject_body[256];
static int64_t now_ms(void) { return 1000; }
static bool platform_client_ready(void) { return true; }
static void reconcile_platform_binding(bool unbound) { (void)unbound; assert(!"unexpected unbind"); }
static void room_invalidate_assignment(void) {}
static void refresh_contacts(void) {}
static void request_voip_profile(void) {}
static void finish_call_session(int error, const char *result) { (void)error; (void)result; ++finishes; }
static void maybe_activate_outgoing_device_call(const char *from) { (void)from; }
static void reject_wechat_values(const char *app, const char *model, const char *token,
                                 const char *room, const char *payload, int reason) {
    assert(!strcmp(app, "wxa") && !strcmp(model, "m") && !strcmp(token, "st") &&
           !strcmp(payload, "p"));
    ++rejects; last_reason = reason;
    snprintf(rejected_room, sizeof(rejected_room), "%s", room);
}
static int platform_client_request(int service, const char *path, const char *body,
                                   void (*cb)(const char *, void *), void *tag) {
    assert(service == PLATFORM_SERVICE_CALL && !strcmp(path, "/v1/call/reject") && !cb && !tag);
    ++device_rejects; snprintf(device_reject_body, sizeof(device_reject_body), "%s", body);
    return 0;
}
static bool preempt_for_call(bool in) {
    (void)in; return atomic_load(&s_public_state) == STARTER_RUNTIME_WAITING;
}
static void starter_tirtc_accept_h5(bool accept) { (void)accept; }
static void product_snapshot_reset(void) {}
static void product_set_call(bool in, bool wx, const char *peer, bool muted) {
    (void)in; (void)wx; (void)peer; (void)muted;
}
static void publish_state(starter_runtime_state_t state) {
    atomic_store(&s_public_state, state); incoming += state == STARTER_RUNTIME_CALL_INCOMING;
}
static bool queue_event(const runtime_event_t *event) {
    assert(event->type == EVENT_VOIP_CONNECT); ++connects; return true;
}
static void diagnostic_event(const char *label, int value) { (void)label; (void)value; }
'''
for name in ('copy_json_string', 'call_signal_matches_room', 'wechat_incoming_room_type',
             'handle_platform_signal'):
    code += function(name) + '\n'
code += r'''
static void push(const char *json) {
    runtime_event_t e = {.text = (char *)json, .length = (uint32_t)strlen(json)};
    handle_platform_signal(&e);
}
/* NULL omits a field, as the server does for an unparseable mini-program Payload. */
static void wx(const char *room, const char *openid, const char *call_id, const char *from) {
    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\"type\":\"call_incoming\",\"channel\":\"wx\",\"payload\":{\"wx_room_id\":\"%s\","
        "\"peer_id\":\"whips://wxvoip?r=%s\",\"token\":\"tok-%s\",\"wx_app_id\":\"wxa\","
        "\"wx_model_id\":\"m\",\"wx_server_token\":\"st\",\"wx_payload\":\"p\","
        "\"wx_user_openid\":\"%s\"%s%s%s%s%s%s}}",
        room, room, room, openid,
        call_id ? ",\"wx_call_id\":\"" : "", call_id ? call_id : "", call_id ? "\"" : "",
        from ? ",\"wx_from\":\"" : "", from ? from : "", from ? "\"" : "");
    assert(n > 0 && (size_t)n < sizeof(json));
    push(json);
}
static void reset(starter_runtime_state_t state) {
    atomic_store(&s_public_state, state);
    s_session_generation = 7;
    s_call_room_id[0] = s_call_id[0] = s_call_peer_id[0] = s_call_connect_peer[0] = 0;
    s_call_wechat = s_call_outgoing = false;
    rejects = last_reason = connects = device_rejects = finishes = incoming = 0;
    rejected_room[0] = 0;
}
/* Outgoing WeChat dial to OpenID oA; call_id is empty until the HTTP answer. */
static void dial(const char *call_id) {
    reset(STARTER_RUNTIME_CALL_CONNECTING);
    s_call_wechat = s_call_outgoing = true;
    snprintf(s_call_peer_id, sizeof(s_call_peer_id), "oA");
    snprintf(s_call_id, sizeof(s_call_id), "%s", call_id);
}
static void still_dialling(void) {
    assert(atomic_load(&s_public_state) == STARTER_RUNTIME_CALL_CONNECTING);
    assert(s_session_generation == 7 && s_call_outgoing && !strcmp(s_call_peer_id, "oA"));
    assert(!incoming && !finishes);
}
int main(void) {
    /* Our ring-back joins once; a redelivery is ignored; another room is never adopted. */
    dial("call-1"); wx("r1", "oA", "call-1", "dev1");
    assert(connects == 1 && !rejects && !strcmp(s_call_room_id, "r1"));
    assert(!strcmp(s_call_connect_peer, "whips://wxvoip?r=r1")); still_dialling();
    wx("r1", "oA", "call-1", "dev1"); assert(connects == 1 && !rejects);
    wx("r2", "oA", "call-1", "dev1");
    assert(connects == 1 && rejects == 1 && last_reason == 7 && !strcmp(rejected_room, "r2"));
    assert(!strcmp(s_call_room_id, "r1") && !strcmp(s_call_connect_peer, "whips://wxvoip?r=r1"));
    wx("r3", "oB", NULL, NULL);
    assert(rejects == 2 && last_reason == 5 && !strcmp(s_call_room_id, "r1")); still_dialling();
    /* An unrelated user's call during the dial is rejected (5), never auto-answered. */
    dial("call-1"); wx("rB", "oB", NULL, NULL);
    assert(!connects && rejects == 1 && last_reason == 5 && !strcmp(rejected_room, "rB"));
    assert(!s_call_room_id[0]); still_dialling();
    dial(""); wx("rB", "oB", NULL, NULL);
    assert(!connects && rejects == 1 && last_reason == 5 && !s_call_room_id[0]); still_dialling();
    /* Ring-back before the HTTP answer: correlated by wx_from and OpenID. */
    dial(""); wx("r1", "oA", "call-1", "dev1");
    assert(connects == 1 && !rejects && !strcmp(s_call_room_id, "r1")); still_dialling();
    /* The dialled user's own mini-program call is not our ring-back once call_id is known. */
    dial("call-1"); wx("rA", "oA", NULL, NULL);
    assert(!connects && rejects == 1 && last_reason == 5); still_dialling();
    dial(""); wx("rA", "oA", NULL, NULL); /* reference rule while call_id is unknown */
    assert(connects == 1 && !rejects && !strcmp(s_call_room_id, "rA"));
    /* An older dial's late ring-back, wrong OpenID or another device's Payload. */
    dial("call-2"); wx("rOld", "oA", "call-1", "dev1");
    assert(!connects && rejects == 1 && last_reason == 7 && !s_call_room_id[0]); still_dialling();
    dial("call-1"); wx("rY", "oZ", "call-1", "dev1");
    assert(!connects && rejects == 1 && last_reason == 7); still_dialling();
    dial(""); wx("rX", "oA", "call-9", "dev2");
    assert(!connects && rejects == 1 && last_reason == 5); still_dialling();
    dial("call-1");
    push("{\"type\":\"call_incoming\",\"payload\":{\"room_id\":\"d_x\",\"caller_id\":\"devB\"}}");
    assert(device_rejects == 1 && strstr(device_reject_body, "\"busy\"") && !connects);
    still_dialling();
    /* Unchanged: idle incoming call, busy during a device dial, idle orphan. */
    reset(STARTER_RUNTIME_WAITING); wx("rI", "oB", NULL, NULL);
    assert(incoming == 1 && !rejects && !connects && s_session_generation == 8);
    assert(!strcmp(s_call_room_id, "rI") && !strcmp(s_call_peer_id, "oB") && !s_call_outgoing);
    reset(STARTER_RUNTIME_CALL_CONNECTING); s_call_outgoing = true;
    wx("rW", "oB", NULL, NULL); assert(rejects == 1 && last_reason == 8 && !connects);
    reset(STARTER_RUNTIME_WAITING); wx("rO", "oA", "call-1", "dev1");
    assert(rejects == 1 && last_reason == 7 && !incoming && !connects);
    puts("PASS: WeChat ring-back needs call_id/wx_from/OpenID; unrelated calls rejected 5, "
         "orphans 7, one room per dial, redelivery ignored");
    return 0;
}
'''
cjson = project / 'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='wechat-ringback-') as directory:
    path = Path(directory)
    (path / 'esp_err.h').write_text('typedef int esp_err_t;\n')
    (path / 'test.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(path), '-I', str(cjson),
                    '-I', str(project / 'components/starter_runtime/include'),
                    str(path / 'test.c'), str(cjson / 'cJSON.c'), '-lm', '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
