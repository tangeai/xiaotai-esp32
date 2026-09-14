#!/usr/bin/env python3
"""Run production C logic with host SDK/RTOS boundary fakes.

Functions are extracted verbatim, not reimplemented in the tests. Preroll is
linked directly. These tests cover deterministic logic, not real RTOS timing,
the closed-source speech model, or physical audio quality.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path: str, name: str) -> str:
    source = (ROOT / path).read_text()
    match = re.search(r"^(?:static )?[A-Za-z_][^;{}\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if match is None:
        raise AssertionError(f"production function missing: {name}")
    depth = 1
    cursor = match.end()
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[match.start():cursor]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include <setjmp.h>
#include "starter_runtime.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
'''


def run(name: str, body: str, sources=()):
    with tempfile.TemporaryDirectory(prefix="xiaotai-test-") as directory:
        temp = Path(directory)
        (temp / "esp_err.h").write_text("typedef int esp_err_t;\n")
        (temp / "test.c").write_text(COMMON + body)
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                   "-fsanitize=undefined", "-I", str(temp),
                   "-I", str(ROOT / "components/starter_runtime/include"),
                   "-I", str(ROOT / "components/starter_media/src"),
                   "-I", str(ROOT / "components/starter_media/include"),
                   "-I", str(ROOT / "components/starter_voice/include"),
                   "-I", str(ROOT / "components/starter_tirtc/include"),
                   str(temp / "test.c"), *(str(ROOT / p) for p in sources),
                   "-o", str(temp / "test")]
        subprocess.run(command, check=True)
        subprocess.run([str(temp / "test")], check=True, timeout=10)
    print(f"PASS: {name}", flush=True)


def test_contacts():
    path = "components/starter_runtime/src/starter_runtime.c"
    body = r'''
typedef enum { CONTACT_MATCH_NOT_FOUND, CONTACT_MATCH_UNIQUE, CONTACT_MATCH_AMBIGUOUS } contact_match_t;
static void *s_product_mutex = (void *)1;
static starter_runtime_product_snapshot_t s_product_snapshot;
#define xSemaphoreTake(a,b) ((void)(a),(void)(b),pdTRUE)
#define xSemaphoreGive(a) ((void)(a))
'''
    body += "\n".join(function(path, n) for n in
                      ("contact_channel_matches", "normalize_contact_name", "find_contact"))
    body += r'''
int main(void) {
    s_product_snapshot.contact_count = 2;
    s_product_snapshot.contacts[0] = (starter_product_contact_t){.id="dad", .name="爸爸"};
    s_product_snapshot.contacts[1] = (starter_product_contact_t){.id="mom", .name="妈妈"};
    uint8_t i = 99;
    assert(find_contact("", "爸爸", "", &i) == CONTACT_MATCH_UNIQUE && i == 0);
    assert(find_contact("dad", "爸爸", "device", &i) == CONTACT_MATCH_UNIQUE);
    assert(find_contact("mom", "爸爸", "", &i) == CONTACT_MATCH_NOT_FOUND);
    assert(find_contact("dad", "", "", &i) == CONTACT_MATCH_NOT_FOUND);
    assert(find_contact("", "未知", "", &i) == CONTACT_MATCH_NOT_FOUND);
    assert(find_contact("dad", "爸爸", "wechat", &i) == CONTACT_MATCH_NOT_FOUND);
    strcpy(s_product_snapshot.contacts[1].name, "爸爸");
    assert(find_contact("dad", "爸爸", "", &i) == CONTACT_MATCH_AMBIGUOUS);
    strcpy(s_product_snapshot.contacts[0].name, "Dad");
    assert(find_contact("dad", " DAD ", "", &i) == CONTACT_MATCH_UNIQUE);
    return 0;
}
'''
    run("name-first matching: 8 conflict/ambiguity/normalization cases", body)



def test_ack():
    body = r'''
typedef enum { CALL_RING_NONE, CALL_RING_OUTGOING, CALL_RING_INCOMING, CALL_RING_AI_ACK } call_ring_kind_t;
typedef struct { int64_t expires_ms; bool male; uint32_t expected_session, playback_epoch; } ai_ack_t;
static atomic_uint s_call_ring_kind;
static void *s_ai_ack_queue = (void *)1;
static bool pending;
static ai_ack_t request;
static unsigned plays, delays;
static int64_t now_ms;
static starter_runtime_status_t status;
static jmp_buf done;
static uint8_t asset[64];
#define WAV_PCM_OFFSET 44
#define kids_watch_outgoing_tiny_wav_start asset
#define kids_watch_outgoing_tiny_wav_end (asset+64)
#define kids_watch_incoming_tiny_wav_start asset
#define kids_watch_incoming_tiny_wav_end (asset+64)
#define ai_ack_male_8k_wav_start asset
#define ai_ack_male_8k_wav_end (asset+64)
#define ai_ack_female_8k_wav_start asset
#define ai_ack_female_8k_wav_end (asset+64)
static int xQueueReceive(void *q, ai_ack_t *out, int wait) {
    (void)q; (void)wait;
    if (!pending) return 0;
    pending = false; *out = request; return pdTRUE;
}
static int64_t esp_timer_get_time(void) { return now_ms * 1000; }
starter_runtime_status_t starter_runtime_status(void) { return status; }
static int starter_media_play_pcm8k(const int16_t *p, size_t n) {
    (void)p; (void)n; ++plays; return -1; /* interrupted/muted/busy */
}
static int starter_media_play_pcm8k_at_epoch(const int16_t *p, size_t n, uint32_t epoch) {
    if (epoch != 4) return -1;
    return starter_media_play_pcm8k(p,n);
}
static void vTaskDelay(unsigned ms) {
    now_ms += ms;
    if (++delays == 4) longjmp(done, 1);
}
'''
    body += function("components/starter_product/src/starter_product.c", "call_ring_task")
    body += r'''
static void scenario(int state, int64_t expires, unsigned expected) {
    status.state = state; status.session_generation = 7; now_ms = 1000; plays = delays = 0;
    pending = true; request = (ai_ack_t){.expires_ms=expires,.expected_session=7,.playback_epoch=4};
    if (setjmp(done) == 0) call_ring_task(NULL);
    assert(!pending && plays == expected);
}
int main(void) {
    scenario(STARTER_RUNTIME_AI_CONNECTING, 2000, 1);
    scenario(STARTER_RUNTIME_AI_CONNECTING, 900, 0);
    scenario(STARTER_RUNTIME_AI_ACTIVE, 2000, 0);
    scenario(STARTER_RUNTIME_CALL_ACTIVE, 2000, 0);
    scenario(STARTER_RUNTIME_WAITING, 2000, 0); /* failed/cancelled AI cannot acknowledge */
    return 0;
}
'''
    run("acknowledgement: cancellation never retries; expired/active requests discarded", body)



def test_preroll():
    run("preroll: ordered replay/live, backpressure, tokens, cancellation, timeout, overflow, gaps", r'''
#include "starter_preroll.h"
static int16_t memory[PREROLL_CAPACITY_SAMPLES], pcm[160], output[160];
static starter_preroll_t q;
static unsigned value;
static int64_t tick;
static void append(void) {
    for (unsigned i=0; i<160; ++i) pcm[i]=(int16_t)value++;
    tick += 20;
    starter_preroll_append(&q, pcm, 160, tick);
}
int main(void) {
    starter_preroll_init(&q, memory, PREROLL_CAPACITY_SAMPLES);
    for (unsigned i=0; i<100; ++i) append();
    assert(q.count == 12000);
    uint32_t token=starter_preroll_prepare(&q, tick, tick);
    assert(token && q.count == 6400);
    assert(starter_preroll_prepare(&q, tick, tick) == 0);
    for (unsigned i=0; i<100; ++i) append();
    assert(!starter_preroll_bind(&q, token+1, 7, tick));
    assert(starter_preroll_bind(&q, token, 7, tick));
    assert(!starter_preroll_bind(&q, token, 8, tick));
    uint32_t timestamp;
    assert(!starter_preroll_peek(&q, 8, output, 160, &timestamp));
    unsigned expected=9600, packets=0;
    while (starter_preroll_peek(&q, 7, output, 160, &timestamp)) {
        assert(timestamp == 1200 + packets*20);
        assert(output[0] == (int16_t)expected);
        assert(starter_preroll_peek(&q, 7, output, 160, &timestamp)); /* no consume on backpressure */
        for (unsigned i=0;i<160;++i) assert(output[i] == (int16_t)expected++);
        starter_preroll_consume(&q, 160); ++packets;
    }
    assert(expected == 32000);
    append();
    assert(starter_preroll_peek(&q, 7, output, 160, &timestamp));
    assert(output[0] == (int16_t)expected); /* live follows replay exactly once */
    starter_preroll_clear(&q);
    assert(!starter_preroll_valid(&q, token, tick));
    assert(!starter_preroll_peek(&q, 7, output, 160, &timestamp));
    append();
    uint32_t next=starter_preroll_prepare(&q, tick, tick);
    assert(next != token);
    assert(!starter_preroll_bind(&q, next, 9, tick+PREROLL_TIMEOUT_MS+1));
    starter_preroll_clear(&q);
    append(); next=starter_preroll_prepare(&q, tick, tick);
    tick+=200; append();
    assert(q.failed && !starter_preroll_valid(&q,next,tick));
    starter_preroll_init(&q,memory,320);
    append(); next=starter_preroll_prepare(&q,tick,tick);
    append(); append();
    assert(q.failed && !starter_preroll_valid(&q,next,tick));
    return 0;
}
''', ("components/starter_media/src/starter_preroll.c",))



def test_preroll_handoff():
    run("AI preroll handoff: partial packet preserved; live gaps recover; pending gaps abort", r'''
#include "starter_preroll.h"
static int16_t memory[PREROLL_CAPACITY_SAMPLES], pcm[256], tail[160];
int main(void) {
    starter_preroll_t q; starter_preroll_init(&q,memory,PREROLL_CAPACITY_SAMPLES);
    for(int i=0;i<256;++i) pcm[i]=i;
    starter_preroll_append(&q,pcm,256,1000);
    unsigned token=starter_preroll_prepare(&q,1000,1000);
    assert(starter_preroll_bind(&q,token,1,1000));
    size_t count=99; uint32_t timestamp=0;
    assert(!starter_preroll_finish_replay(&q,1,tail,160,&count,&timestamp));
    starter_preroll_consume(&q,160);
    assert(!starter_preroll_finish_replay(&q,2,tail,160,&count,&timestamp));
    assert(starter_preroll_finish_replay(&q,1,tail,160,&count,&timestamp));
    assert(count==96 && timestamp==988 && q.token==0 && q.generation==0);
    for(int i=0;i<96;++i) assert(tail[i]==i+160);
    starter_preroll_append(&q,pcm,256,2000);
    token=starter_preroll_prepare(&q,2000,2000);
    starter_preroll_append(&q,pcm,256,2200);
    assert(starter_preroll_failure_requires_abort(&q,token,2200));
    starter_preroll_clear(&q);
    starter_preroll_append(&q,pcm,256,3000);
    token=starter_preroll_prepare(&q,3000,3000);
    assert(starter_preroll_bind(&q,token,3,3000));
    starter_preroll_append(&q,pcm,256,3200);
    assert(q.failed && !starter_preroll_failure_requires_abort(&q,token,3200));
    return 0;
}
''', ("components/starter_media/src/starter_preroll.c",))


def test_mqtt_lifetime():
    body = r'''
#include <time.h>
#undef ESP_LOGI
#define ESP_LOGI(tag,...) printf(__VA_ARGS__)
static int64_t esp_timer_get_time(void) {static int64_t t;return t+=1000;}
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
static void *s_mqtt_lifecycle=(void*)1, *s_mqtt=(void*)2;
static bool s_ready=true, s_mqtt_connected=true, locked;
static bool platform_client_ready(void) {return s_ready;}
static int stop_error,destroy_error;
static unsigned publishes,destroys;
static const char *s_device_id="test";
typedef void *esp_mqtt_client_handle_t;
static int xSemaphoreTake(void *m,int wait) {(void)m;(void)wait;if(locked)return 0;locked=true;return pdTRUE;}
static void xSemaphoreGive(void *m) {(void)m;assert(locked);locked=false;}
static int esp_mqtt_client_publish(void *m,const char *t,const char *b,int n,int q,int r) {
    (void)t;(void)b;(void)n;(void)q;(void)r;assert(locked && m);++publishes;return 0;
}
static void publish_heartbeat(unsigned sequence);
static int esp_mqtt_client_stop(void *m) {
    assert(locked && m); unsigned before=publishes;
    publish_heartbeat(99); /* concurrent publisher cannot enter during teardown */
    assert(publishes==before);return stop_error;
}
static int esp_mqtt_client_destroy(void *m) {assert(locked && m);++destroys;return destroy_error;}
'''
    path = "components/platform_client/src/platform_client.c"
    body += function(path, "publish_heartbeat") + "\n" + function(path, "stop_mqtt")
    body += "\n" + function(path, "platform_client_suspend_mqtt_for_realtime")
    body += r'''
int main(void) {
    publish_heartbeat(1);assert(publishes==1 && !locked);
    locked=true;assert(platform_client_suspend_mqtt_for_realtime()==ESP_ERR_TIMEOUT);locked=false;
    assert(destroys==0);
    stop_error=9;assert(platform_client_suspend_mqtt_for_realtime()==9 && !locked && s_mqtt);
    stop_error=0;destroy_error=8;
    assert(platform_client_suspend_mqtt_for_realtime()==8 && !locked && s_mqtt);
    destroy_error=0;
    assert(platform_client_suspend_mqtt_for_realtime()==ESP_OK && !locked && !s_mqtt);
    publish_heartbeat(2);assert(publishes==1 && !locked);
    assert(platform_client_suspend_mqtt_for_realtime()==ESP_OK && !locked);
    return 0;
}
'''
    run("MQTT lifetime: publish/teardown exclusion and every failure unlock", body)


def test_command_recovery():
    body = r'''
#include "starter_tirtc.h"
#define RUNTIME_TEXT_MAX 4096
#define RUNTIME_ROOM_TEXT_MAX 32768
#define STARTER_TIRTC_ROOM 4
#define EVENT_COMMAND 1
typedef struct {int type;starter_tirtc_mode_t mode;uint32_t generation,command,received_at_ms;} runtime_event_t;
static int64_t now_ms(void) {return 1234;}
static atomic_bool s_transport_recovery_required;
static bool copy_ok,queue_ok;static int released;
static bool copy_event_text(runtime_event_t *e,const void *p,size_t n) {(void)e;(void)p;(void)n;return copy_ok;}
static bool queue_event(const runtime_event_t *e) {assert(e->received_at_ms==1234);return queue_ok;}
static void release_event(runtime_event_t *e) {(void)e;++released;}
'''
    # Only the callback's enum/type dependency is needed, not SDK transport headers.
    body = body.replace('#include "starter_tirtc.h"', 'typedef int starter_tirtc_mode_t;')
    body += function("components/starter_runtime/src/starter_runtime.c", "on_tirtc_command")
    body += r'''
int main(void) {
    on_tirtc_command(3,7,0x2001,NULL,0,NULL);
    assert(atomic_load(&s_transport_recovery_required));
    atomic_store(&s_transport_recovery_required,false);copy_ok=true;
    on_tirtc_command(3,7,0x2001,NULL,0,NULL);
    assert(atomic_load(&s_transport_recovery_required) && released==1);
    atomic_store(&s_transport_recovery_required,false);queue_ok=true;
    on_tirtc_command(3,7,0x2001,NULL,0,NULL);
    assert(!atomic_load(&s_transport_recovery_required));
    on_tirtc_command(STARTER_TIRTC_ROOM,7,0x2200,NULL,32769,NULL);
    assert(atomic_load(&s_transport_recovery_required));
    return 0;
}
'''
    run("command loss: allocation/queue failure schedules session recovery", body)


def test_navigation_priority():
    product_path = "components/starter_product/src/starter_product.c"
    source = (ROOT / product_path).read_text()
    pages = re.search(r"typedef enum \{\s*PAGE_HOME_FACE.*?\} product_page_t;", source, re.S).group()
    run("navigation: AI/status/menu retained; other pages queue stop; queue failure stays put", pages + r'''
static product_page_t s_page;
static int stop_count, stop_error;
esp_err_t starter_runtime_ai_stop(void) { ++stop_count; return stop_error; }
''' + function(product_path, "page_ends_ai") + function(product_path, "enter_page") + r'''
int main(void) {
    const product_page_t keep[] = {PAGE_HOME_FACE, PAGE_HOME_CLOCK, PAGE_MENU,
        PAGE_AI_CHAT, PAGE_DIAGNOSTICS, PAGE_CALL, PAGE_CALL_RESULT};
    const product_page_t stop[] = {PAGE_CONTACTS, PAGE_CONTACT_DETAIL, PAGE_EMOJIS,
        PAGE_EMOJI_PREVIEW, PAGE_SETTINGS, PAGE_NETWORK};
    for (unsigned i=0; i<sizeof(keep)/sizeof(keep[0]); ++i) {
        assert(enter_page(keep[i])); assert(s_page==keep[i]); assert(stop_count==0);
    }
    for (unsigned i=0; i<sizeof(stop)/sizeof(stop[0]); ++i) {
        assert(enter_page(stop[i])); assert(s_page==stop[i]); assert(stop_count==(int)i+1);
    }
    s_page=PAGE_MENU; stop_error=1;
    assert(!enter_page(PAGE_SETTINGS)); assert(s_page==PAGE_MENU);
    return 0;
}
''')


def test_stop_and_diagnostics():
    runtime_path = "components/starter_runtime/src/starter_runtime.c"
    run("AI stop: ignored during waiting/H5/call states, stops only AI", r'''
static atomic_int s_public_state;
static int finished, commands;
#define AI_COMMAND 1
static bool starter_tirtc_connected(void) { return true; }
static int starter_tirtc_send_command(int cmd, const char *data, size_t size) {
    assert(cmd==AI_COMMAND && data && size); ++commands; return 0;
}
static void finish_session(int error) { assert(error==0); ++finished; }
''' + function(runtime_path, "end_ai_session") + r'''
int main(void) {
    for (int state=0; state<=STARTER_RUNTIME_CALL_ACTIVE; ++state) {
        s_public_state=state; finished=commands=0; end_ai_session();
        int ai=state==STARTER_RUNTIME_AI_CONNECTING || state==STARTER_RUNTIME_AI_ACTIVE;
        assert(finished==ai && commands==ai);
    }
    return 0;
}
''')
    run("diagnostic ring: bounded eviction, ordering, session attribution", r'''
static starter_runtime_diagnostics_t s_diagnostics;
static uint32_t s_session_generation=7;
static int64_t clock_ms;
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
''' + function(runtime_path, "diagnostic_event") + r'''
int main(void) {
    for (int i=0; i<12; ++i) { clock_ms=i*100; diagnostic_event("call tool received", i); }
    assert(s_diagnostics.count==STARTER_DIAGNOSTIC_EVENTS);
    for (unsigned i=0; i<s_diagnostics.count; ++i) {
        assert(s_diagnostics.events[i].value==(int)i+4);
        assert(s_diagnostics.events[i].at_ms==(i+4)*100);
        assert(s_diagnostics.events[i].session==7);
        assert(strcmp(s_diagnostics.events[i].label, "call tool received")==0);
    }
    return 0;
}
''')
    runtime_path = "components/starter_runtime/src/starter_runtime.c"
    run("priority: incoming/outgoing preempt AI/H5/Room, never another call (18 cases)", r'''
static atomic_int s_public_state;
static int finishes, events;
static void finish_session(int error) { assert(error==0); ++finishes; s_public_state=STARTER_RUNTIME_WAITING; }
static void diagnostic_event(const char *label, int value) { assert(label); assert(value>0); ++events; }
''' + function(runtime_path, "preempt_for_call") + r'''
int main(void) {
    for (int incoming=0; incoming<2; ++incoming) {
        for (int state=STARTER_RUNTIME_WAITING; state<=STARTER_RUNTIME_ROOM_ACTIVE; ++state) {
            s_public_state=state; finishes=events=0;
            bool lower=(state>=STARTER_RUNTIME_H5_ACTIVE && state<=STARTER_RUNTIME_AI_ACTIVE) ||
                state==STARTER_RUNTIME_ROOM_CONNECTING || state==STARTER_RUNTIME_ROOM_ACTIVE;
            assert(preempt_for_call(incoming)==(state==STARTER_RUNTIME_WAITING || lower));
            assert(finishes==(int)lower && events==(int)lower);
            assert(s_public_state==(lower ? STARTER_RUNTIME_WAITING : state));
        }
    }
    return 0;
}
''')


def test_voice_diag_command():
    run("voice-diag command: bounded duration, default, malformed input, read-only snapshots", r'''
static unsigned snapshots, identities, delays;
#define printf(...) ((void)0)
static void print_firmware_identity(void) { ++identities; }
static void print_wake_diagnostics(void) { ++snapshots; }
static void vTaskDelay(unsigned ms) { assert(ms==1000); ++delays; }
''' + function("components/starter_console/src/starter_console.c", "command_voice_diag") + r'''
int main(void) {
    char *args[]={"voice-diag", "5", "extra"};
    assert(command_voice_diag(1,args)==0 && snapshots==21 && delays==20 && identities==1);
    snapshots=delays=identities=0;
    assert(command_voice_diag(2,args)==0 && snapshots==6 && delays==5 && identities==1);
    const char *invalid[]={"4","31","-1","5x","","99999999999999999999999"};
    snapshots=delays=identities=0;
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        args[1]=(char*)invalid[i]; assert(command_voice_diag(2,args)==1);
    }
    assert(command_voice_diag(3,args)==1 && snapshots==0 && delays==0 && identities==0);
    return 0;
}
''')


def test_wake_diagnostics():
    run("PCM diagnostics: silence, DC, alternating extremes, integer RMS, read-only", r'''
#include "starter_signal_level.h"
int main(void) {
    const int16_t silence[4]={0};
    const int16_t dc[4]={-1000,-1000,-1000,-1000};
    int16_t extrema[4]={-32768,32767,-32768,32767}, before[4];
    memcpy(before, extrema, sizeof(before));
    starter_signal_level_t s=starter_signal_measure(NULL, 4);
    assert(s.rms==0 && s.peak==0 && s.dc==0);
    s=starter_signal_measure(silence, 4); assert(s.rms==0 && s.peak==0);
    s=starter_signal_measure(dc, 4); assert(s.rms==1000 && s.peak==1000 && s.dc==-1000);
    s=starter_signal_measure(extrema, 4); assert(s.rms==32767 && s.peak==32768 && s.dc==0);
    assert(memcmp(before, extrema, sizeof(before))==0);
    const int16_t mix[2]={3,4}; s=starter_signal_measure(mix, 2); assert(s.rms==3);
    for (uint32_t i=0; i<32769; ++i) assert(starter_signal_isqrt(i*i)==i);
    return 0;
}
''')
    header = (ROOT / "components/starter_media/include/starter_media.h").read_text()
    typedef = re.search(r"typedef struct \{[^}]+\} starter_media_audio_diagnostics_t;", header).group()
    run("audio diagnostics: 32-frame window, max/average, independent publication", r'''
#include "starter_aec.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
''' + typedef + r'''
static starter_media_audio_diagnostics_t s_audio_diag_acc, s_audio_diag;
''' + function("components/starter_media/src/starter_media.c", "update_audio_diagnostics") + r'''
int main(void) {
    starter_aec_output_t f={0};
    for (unsigned i=0; i<32; ++i) {
        for (unsigned c=0; c<3; ++c) f.level[c]=(starter_signal_level_t){100+i+c, 1000+i+c, -100};
        f.measurement_us=10+i;
        update_audio_diagnostics(&f, 100+i, i*32);
        if (i<31) assert(s_audio_diag.frames==0);
    }
    assert(s_audio_diag.window==1 && s_audio_diag.frames==32 && s_audio_diag.at_ms==992);
    assert(s_audio_diag.rms_avg[0]==115 && s_audio_diag.rms_max[0]==131 && s_audio_diag.peak[2]==1033);
    assert(s_audio_diag.dc_avg[0]==-100 && s_audio_diag.aec_avg_us==115);
    assert(s_audio_diag.measurement_avg_us==25 && s_audio_diag.measurement_max_us==41);
    memset(&f, 0, sizeof(f));
    for (unsigned i=0; i<32; ++i) update_audio_diagnostics(&f, 0, 2000+i*32);
    assert(s_audio_diag.window==2 && s_audio_diag.rms_max[0]==0 && s_audio_diag.dc_avg[0]==0);
    return 0;
}
''')


def test_wake_window():
    header = str(ROOT / "components/starter_voice/src/wake_window.h")
    run("Voicute latest-window continuity, wraparound, pause epochs, stale/cooldown rejection", '#include "' + header + '"\n' + r'''
int main(void) {
    int16_t ring[WAKE_WINDOW_SAMPLES], out[WAKE_WINDOW_SAMPLES], pcm[512];
    wake_window_t w={.pcm=ring};
    assert(!wake_window_snapshot(&w, out));
    int total=0; int64_t at=1000;
    for (int frame=0; frame<80; ++frame) {
        for (int i=0;i<512;++i) pcm[i]=(int16_t)((total+i)%30000);
        wake_window_append(&w,pcm,512,at); total+=512; at+=32;
        if(frame==30) assert(!wake_window_snapshot(&w,out));
    }
    assert(wake_window_snapshot(&w,out));
    for(size_t i=0;i<WAKE_WINDOW_SAMPLES;++i)
        assert(out[i]==(int16_t)((total-WAKE_WINDOW_SAMPLES+i)%30000));
    assert(!wake_window_snapshot(&w,out));
    for(int i=0;i<4;++i) {wake_window_append(&w,pcm,512,at);at+=32;}
    assert(!wake_window_snapshot(&w,out));
    wake_window_append(&w,pcm,512,at);
    assert(wake_window_snapshot(&w,out));
    uint32_t epoch=w.epoch;
    wake_window_reset(&w);
    assert(w.epoch!=epoch && !wake_window_snapshot(&w,out));
    assert(wake_result_check(true,false,epoch,w.epoch,1000,1100,0)==WAKE_REJECT_EPOCH);
    assert(wake_result_check(false,false,1,1,1000,1100,0)==WAKE_REJECT_EPOCH);
    assert(wake_result_check(true,true,1,1,1000,1100,0)==WAKE_REJECT_EPOCH);
    assert(wake_result_check(true,false,1,1,1000,2001,0)==WAKE_REJECT_AGE);
    assert(wake_result_check(true,false,1,1,1000,999,0)==WAKE_REJECT_AGE);
    assert(wake_result_check(true,false,1,1,1000,1100,1101)==WAKE_REJECT_COOLDOWN);
    assert(wake_result_check(true,false,1,1,1000,1100,1100)==WAKE_ACCEPT);
    wake_window_append(&w,pcm,512,5000); epoch=w.epoch;
    wake_window_append(&w,pcm,512,5200);
    assert(w.epoch!=epoch && w.count==512);
    epoch=w.epoch; wake_window_append(&w,pcm,512,5100);
    assert(w.epoch!=epoch && w.count==512);
    return 0;
}
''')


def test_external_connect_memory():
    path = "components/starter_runtime/src/starter_runtime.c"
    source = (ROOT / path).read_text()
    body = "\n".join(re.findall(r"^#define EXTERNAL_CONNECT_[^\n]+", source, re.M)) + r'''
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define ESP_ERR_NO_MEM 3
static char reserve;
static void *s_external_connect_reserve=&reserve;
static bool s_mqtt_suspended_for_connect, s_external_connect_reserve_loaned;
static int64_t s_mqtt_resume_due_ms;
static size_t available=100000, largest=65536;
static unsigned stops, resumes, frees, allocs;
static bool ready=true, online=true, fail_alloc;
static int stop_error, resume_error;
static bool platform_client_ready(void) { return ready; }
static bool platform_client_mqtt_connected(void) { return online; }
static int64_t now_ms(void) { return 100; }
static size_t heap_caps_get_free_size(unsigned caps) { assert(caps==3); return available; }
static size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps==3); return largest; }
static void heap_caps_free(void *p) { assert(p==&reserve); ++frees; }
static void *heap_caps_malloc(size_t n,unsigned caps) {
    assert(n==17*1024 && caps==3); ++allocs; return fail_alloc ? NULL : &reserve;
}
static esp_err_t platform_client_suspend_mqtt_for_realtime(void) { ++stops; return stop_error; }
static esp_err_t platform_client_resume_mqtt_after_realtime(void) { assert(ready); ++resumes; return resume_error; }
'''
    body += function(path, "starter_runtime_arm_external_connect_reserve")
    body += function(path, "prepare_external_connect_memory")
    body += function(path, "restore_external_connect_memory")
    body += r'''
int main(void) {
    assert(prepare_external_connect_memory());
    assert(stops==0 && !s_mqtt_suspended_for_connect && s_external_connect_reserve_loaned);
    assert(!s_external_connect_reserve && frees==1);
    assert(!prepare_external_connect_memory()); /* no overlapping reserve loans */
    restore_external_connect_memory();
    assert(resumes==0 && allocs==1 && s_external_connect_reserve==&reserve);
    assert(!s_external_connect_reserve_loaned);
    restore_external_connect_memory(); assert(allocs==1);
    available=64*1024; largest=32*1024;
    assert(prepare_external_connect_memory() && stops==0); /* exact boundaries */
    restore_external_connect_memory();
    available--; /* enough contiguous heap, insufficient total free */
    assert(prepare_external_connect_memory() && stops==1 && s_mqtt_suspended_for_connect);
    restore_external_connect_memory(); assert(resumes==1);
    available=100000; largest=32*1024-1; /* fragmentation pressure */
    assert(prepare_external_connect_memory() && stops==2);
    restore_external_connect_memory(); assert(resumes==2);
    largest=65536; online=false; stop_error=7;
    unsigned released=frees;
    assert(!prepare_external_connect_memory());
    assert(frees==released && s_external_connect_reserve==&reserve);
    assert(!s_external_connect_reserve_loaned && !s_mqtt_suspended_for_connect);
    stop_error=0;
    assert(prepare_external_connect_memory() && s_mqtt_suspended_for_connect);
    fail_alloc=true; restore_external_connect_memory();
    assert(s_mqtt_resume_due_ms==1100 && resumes==2 && s_external_connect_reserve_loaned);
    assert(!prepare_external_connect_memory());
    fail_alloc=false; resume_error=8; restore_external_connect_memory();
    assert(resumes==3 && s_mqtt_resume_due_ms==1100 && s_external_connect_reserve==&reserve);
    unsigned allocated=allocs;
    resume_error=0; restore_external_connect_memory();
    assert(resumes==4 && !s_external_connect_reserve_loaned && s_mqtt_resume_due_ms==0);
    assert(allocs==allocated); /* resume failure cannot double-allocate reserve */
    online=true; assert(prepare_external_connect_memory());
    ready=false; restore_external_connect_memory();
    assert(resumes==4 && s_external_connect_reserve==&reserve && !s_external_connect_reserve_loaned);
    ready=true; online=false; assert(prepare_external_connect_memory());
    ready=false; restore_external_connect_memory();
    assert(resumes==4 && !s_mqtt_suspended_for_connect && s_external_connect_reserve==&reserve);
    ready=true; online=true; assert(prepare_external_connect_memory());
    fail_alloc=true; restore_external_connect_memory();
    assert(s_external_connect_reserve_loaned && s_mqtt_resume_due_ms==1100);
    fail_alloc=false; restore_external_connect_memory();
    assert(resumes==4 && !s_external_connect_reserve_loaned && s_mqtt_resume_due_ms==0);
    s_external_connect_reserve=NULL;
    assert(!prepare_external_connect_memory());
    return 0;
}
'''
    run("external memory: MQTT coexistence, free/largest limits, failures, rebind and reserve ownership", body)


def test_connection_lifecycle():
    path = "components/starter_runtime/src/starter_runtime.c"
    body = r'''
#include "starter_tirtc.h"
#define ESP_ERR_INVALID_STATE 1
#define ESP_FAIL 2
#define AI_RESPONSE_TIMEOUT_MS 1000
#define VOIP_CONNECTED_WAIT_TIMEOUT_MS 1000
#define CALL_COMMAND_CONNECT 0x2000
typedef struct {
    starter_tirtc_mode_t mode;
    uint32_t generation, request_tag;
    bool flag;
    int error;
} runtime_event_t;
static atomic_int s_public_state, s_last_error;
static uint32_t s_session_generation=8, s_connection_generation;
static uint32_t s_ai_drain_started_ms;
static int64_t s_deadline_ms;
static bool s_call_waiting_confirm, s_call_outgoing=true;
static bool s_call_p2p_connected, s_call_wechat;
static char s_call_room_id[32];
static unsigned finished, resumed, media_started, ai_sent;
static bool binding_ready=true;
static unsigned rejected_connection;
static bool platform_client_ready(void) { return binding_ready; }
uint32_t starter_tirtc_generation(void) {return 23;}
int starter_tirtc_disconnect(void) {++rejected_connection;return 0;}
static int64_t now_ms(void) { return 100; }
static void finish_session(int error) { (void)error; ++finished; }
static void finish_call_session(int error,const char *text) { (void)text; finish_session(error); }
static void restore_external_connect_memory(void) { ++resumed; }
static void send_ai_start(void) { ++ai_sent; }
static void publish_state(starter_runtime_state_t state) { atomic_store(&s_public_state,state); }
static esp_err_t starter_media_start(starter_tirtc_mode_t mode,uint32_t gen) {
    (void)mode; (void)gen; ++media_started; return ESP_OK;
}
int starter_tirtc_send_command(uint32_t command,const void *data,uint32_t length) {
    (void)command;(void)data;(void)length;return 0;
}
static void maybe_activate_outgoing_device_call(const char *source) { (void)source; }
static void room_connection(const runtime_event_t *event) {(void)event;assert(!"room handled in test_room.py");}
'''
    body += function(path, "handle_connection")
    body += r'''
int main(void) {
    (void)s_call_wechat;
    atomic_store(&s_public_state,STARTER_RUNTIME_AI_CONNECTING);
    runtime_event_t e={.mode=STARTER_TIRTC_H5,.generation=7,.flag=false};
    handle_connection(&e);
    assert(finished==0 && resumed==0); /* old H5 close must not end new AI */
    e=(runtime_event_t){.mode=STARTER_TIRTC_AI,.generation=7,.request_tag=7,.flag=true};
    handle_connection(&e);
    assert(finished==0 && resumed==0); /* queued old success cannot end/resume new connect */
    atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
    e=(runtime_event_t){.mode=STARTER_TIRTC_CALL,.request_tag=7,.flag=false,.error=5};
    handle_connection(&e);
    assert(finished==0 && resumed==0); /* failed old call attempt */
    atomic_store(&s_public_state,STARTER_RUNTIME_AI_CONNECTING);
    e=(runtime_event_t){.mode=STARTER_TIRTC_AI,.request_tag=8,.flag=false,.error=5};
    handle_connection(&e);
    assert(finished==1 && resumed==1); /* current attempt failure still terminates */
    finished=resumed=0;
    e=(runtime_event_t){.mode=STARTER_TIRTC_AI,.generation=21,.request_tag=8,.flag=true};
    handle_connection(&e);
    assert(s_connection_generation==21 && resumed==1 && finished==0);
    assert(ai_sent==1 && media_started==0); /* immediate RPC, not early audio */
    handle_connection(&e);
    assert(ai_sent==1 && resumed==1); /* duplicate success cannot resend RPC */
    atomic_store(&s_public_state,STARTER_RUNTIME_AI_ACTIVE);
    e=(runtime_event_t){.mode=STARTER_TIRTC_AI,.generation=20,.flag=false};
    handle_connection(&e); assert(finished==0 && resumed==1);
    e.generation=21; handle_connection(&e); assert(finished==1);
    s_ai_drain_started_ms=100;
    handle_connection(&e); assert(finished==1); /* EOS tail survives transport close */
    s_ai_drain_started_ms=0;
    finished=resumed=0;
    s_connection_generation=0;
    atomic_store(&s_public_state,STARTER_RUNTIME_WAITING);
    e=(runtime_event_t){.mode=STARTER_TIRTC_H5,.generation=22,.flag=true};
    handle_connection(&e);
    assert(media_started==1 && s_connection_generation==22);
    e.flag=false; handle_connection(&e); assert(finished==1);
    binding_ready=false;e.flag=true;e.generation=23;
    handle_connection(&e);assert(rejected_connection==1 && media_started==1);
    e.generation=22;handle_connection(&e);assert(rejected_connection==1);
    binding_ready=true; s_connection_generation=0; s_call_wechat=true;
    atomic_store(&s_public_state,STARTER_RUNTIME_CALL_CONNECTING);
    e=(runtime_event_t){.mode=STARTER_TIRTC_VOIP,.generation=24,.request_tag=s_session_generation,.flag=true};
    handle_connection(&e);
    assert(s_call_waiting_confirm && media_started==1 && ai_sent==1);
    /* WeChat must still wait for its separate 0x2000 business confirmation. */
    return 0;
}
'''
    run("connection lifecycle: stale H5/AI/call events cannot terminate new owner", body)


def test_call_signal_room_match():
    body = function("components/starter_runtime/src/starter_runtime.c", "call_signal_matches_room")
    body += r'''
int main(void) {
    assert(call_signal_matches_room(true,"room-2","room-2"));
    assert(!call_signal_matches_room(false,"room-2","room-2"));
    assert(!call_signal_matches_room(true,"","room-2"));
    assert(!call_signal_matches_room(true,"room-1","room-2"));
    assert(!call_signal_matches_room(true,NULL,"room-2"));
    assert(!call_signal_matches_room(true,"room-2",NULL));
    return 0;
}
'''
    run("call cancellation: only an exact nonempty room may end the active call", body)


def test_voip_profile_delivery_recovery():
    body = r'''
static atomic_bool s_voip_profile_delivery_failed;
static bool s_voip_profile_inflight, s_voip_profile_ready;
static int64_t s_voip_profile_retry_at_ms;
'''
    body += function("components/starter_runtime/src/starter_runtime.c",
                     "recover_voip_profile_delivery_failure")
    body += r'''
int main(void) {
    s_voip_profile_inflight=true; s_voip_profile_ready=true;
    assert(!recover_voip_profile_delivery_failure(123));
    assert(s_voip_profile_inflight && s_voip_profile_ready);
    atomic_store(&s_voip_profile_delivery_failed,true);
    assert(recover_voip_profile_delivery_failure(456));
    assert(!s_voip_profile_inflight && !s_voip_profile_ready);
    assert(s_voip_profile_retry_at_ms==456);
    assert(!recover_voip_profile_delivery_failure(789));
    return 0;
}
'''
    run("VoIP profile: a lost callback clears inflight and schedules retry", body)


def test_finish_pending_connect():
    body = r'''
static uint32_t s_connection_generation;
static uint32_t s_ai_drain_started_ms=123;
static int64_t s_deadline_ms;
static char s_ai_role_id[2], s_ai_request_id[2], s_call_room_id[2], s_call_peer_id[2];
static char s_call_peer_name[2], s_call_wx_app_id[2], s_call_wx_model_id[2];
static char s_call_connect_peer[2], s_call_connect_token[2], s_call_wx_session_token[2];
static char s_call_wx_payload[2], s_call_id[2];
static struct { char room_id[129]; int stage; bool attempted; } s_call_room_recovery;
static bool s_call_wechat, s_call_outgoing, s_call_waiting_confirm, s_voip_connect_inflight;
static bool s_call_peer_answered, s_call_p2p_connected;
static atomic_int s_last_error;
static bool pending=true, connected=false, platform_ready=true, h5_allowed;
static bool platform_client_ready(void) {return platform_ready;}
static unsigned disconnects;
static void room_detach(int error) {(void)error;}
static void diagnostic_event(const char *s,int e) {(void)s;(void)e;}
static void starter_media_stop(void) {}
static bool starter_tirtc_connected(void) {return connected;}
static int starter_tirtc_disconnect(void) {pending=false;++disconnects;return -1;}
static void starter_tirtc_accept_h5(bool a) {h5_allowed=a;}
static void product_snapshot_reset(void) {}
static void product_set_call(bool a,bool b,const char*c,bool d) {(void)a;(void)b;(void)c;(void)d;}
static void publish_state(starter_runtime_state_t s) {(void)s;}
static void restore_external_connect_memory(void) {}
'''
    body += function("components/starter_runtime/src/starter_runtime.c", "finish_session")
    body += r'''
int main(void) {
    (void)starter_tirtc_connected();
    strcpy(s_call_room_recovery.room_id,"d_old");
    s_call_room_recovery.stage=4; s_call_room_recovery.attempted=true;
    finish_session(0);
    assert(!pending && disconnects==1 && s_ai_drain_started_ms==0);
    assert(!s_call_room_recovery.room_id[0] && !s_call_room_recovery.stage &&
           !s_call_room_recovery.attempted);
    pending=true;connected=true;finish_session(0);
    assert(!pending && disconnects==2 && h5_allowed);
    platform_ready=false;finish_session(0);
    assert(disconnects==3 && !h5_allowed);
    return 0;
}
'''
    run("finish cancels pending transport even without an established handle", body)


def test_wake_score_display():
    body = '#include "starter_voice.h"\n'
    body += function("components/starter_product/src/starter_product.c", "format_wake_debug")
    body += r'''
int main(void) {
    char text[80];
    starter_voice_result_t event = {.acoustic_score_valid=true,
        .probability_milli=954, .threshold_milli=850};
    starter_voice_result_t queued = event;
    event.probability_milli=0; event.threshold_milli=700;
    format_wake_debug(text,sizeof(text),&queued);
    assert(strcmp(text,"语音唤醒 0.954 / 阈值 0.850")==0);
    queued.probability_milli=1000;
    format_wake_debug(text,sizeof(text),&queued);
    assert(strstr(text,"1.000"));
    format_wake_debug(text,sizeof(text),NULL);
    assert(strcmp(text,"手动启动")==0);
    queued.acoustic_score_valid=false;
    format_wake_debug(text,sizeof(text),&queued);
    assert(strcmp(text,"手动启动")==0);
    return 0;
}
'''
    run("wake score UI: immutable event, decimal formatting, manual source", body)


if __name__ == "__main__":
    test_external_connect_memory()
    test_finish_pending_connect()
    test_connection_lifecycle()
    test_call_signal_room_match()
    test_voip_profile_delivery_recovery()
    test_wake_score_display()
    test_wake_window()
    test_contacts()
    test_ack()
    test_preroll()
    test_preroll_handoff()
    test_mqtt_lifetime()
    test_command_recovery()
    test_navigation_priority()
    test_stop_and_diagnostics()
    test_wake_diagnostics()
    test_voice_diag_command()
