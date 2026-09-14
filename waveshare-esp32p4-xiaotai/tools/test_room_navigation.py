"""Host checks for foreground room ownership; does not operate a device."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
room = (root / 'components/starter_runtime/src/starter_room.inc').read_text(encoding='utf-8')
runtime = (root / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
ui = (root / 'components/starter_product/src/starter_product_room.inc').read_text(encoding='utf-8')
home = (root / 'components/starter_product/src/starter_product_s3_ui.inc').read_text(encoding='utf-8')


def function(source, name):
    # A forward declaration is not the implementation under test.
    match = re.search(r'(?m)^(?:static )?(?:bool|void|esp_err_t) ' + re.escape(name)
                      + r'\([^;{}]*\)\s*\{', source)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


tick = function(room, 'room_tick')
closed = tick.index('if (!room_app_current() && !s_room.leave_requested) return;')
assert tick.index('room_apply_navigation();') < closed
assert tick.index('if (s_room.release.id[0])') < closed
assert closed < tick.index('STARTER_RUNTIME_ROOM_ACTIVE') < tick.index('room_http(ROOM_GET')
assert 'if (!room_app_current()) return;' in tick
assert 'room_app_open()' in function(room, 'starter_runtime_room_ptt')
assert 'atomic_load(&s_room_ptt_app_request)' in function(room, 'starter_runtime_room_ptt')
assert 'event->request_tag != s_room.app_request' in function(room, 'room_handle_intent')
assert '!room_app_current() || !room_owns_media()' in function(room, 'room_http_result')
assert 'else room_remember_release(false);' in function(room, 'room_http_result')
owner = function(runtime, 'runtime_task')
assert owner.index('room_apply_navigation();') < owner.index('switch (event.type)')
assert 'room_ui_navigation();' in function(ui, 'room_ui_reset')
assert 'room_ui_navigation();' in function(ui, 'room_ui_tick')
assert 'PAGE_ROOM_FORM' in function(ui, 'room_ui_navigation')
assert 'lv_keyboard_' not in ui  # local matrix never changes other forms
keys = re.search(r'keys\[\]\s*=\s*\{(.*?)\};', ui, re.S).group(1)
assert keys.count('"\\n"') == 2
assert len(re.findall(r'"[0-9]"', keys)) == 10
assert '"9", "0", LV_SYMBOL_BACKSPACE, "确认"' in keys
assert 'lv_obj_set_size(room_ui.keyboard, 304, 152);' in ui
assert 'room_ui.self[i]' in ui and '（本机）' in ui
assert 'member->self ? "本机"' not in room
status = function(home, 's3_refresh_home_status')
assert 'bool idle = runtime.state == STARTER_RUNTIME_WAITING && !ai_pending;' in status
assert 'case STARTER_RUNTIME_CALL_INCOMING:' in status
assert 'case STARTER_RUNTIME_CALL_CONNECTING:' in status
assert 'case STARTER_RUNTIME_CALL_ACTIVE:' in status
assert 'lv_obj_clear_flag(s3_ui.wake_hint, LV_OBJ_FLAG_HIDDEN);' in status

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define ESP_LOGI(...) ((void)0)
enum { STARTER_RUNTIME_WAITING, STARTER_RUNTIME_ROOM_CONNECTING,
       STARTER_RUNTIME_ROOM_ACTIVE, STARTER_RUNTIME_AI_ACTIVE, STARTER_RUNTIME_CALL_ACTIVE };
enum { STARTER_ROOM_EMPTY, STARTER_ROOM_SYNCING, STARTER_ROOM_SUSPENDED };
typedef int starter_room_state_t;
static atomic_uint s_room_app_request, s_room_ptt_admitted;
static atomic_int s_public_state;
static struct {
    unsigned app_request, change_revision, failures;
    bool dirty, blocked;
    int64_t retry_at, poll_at;
    struct { bool assigned, assignment_known, members_ready;
             unsigned member_count, online_count; int state; } view;
    struct { char id[129]; } release;
} s_room;
static unsigned stopped, ptt_stops, ptt_generation;
static void starter_media_room_ptt(unsigned generation, bool pressed) {
    assert(!pressed); ++ptt_stops; ptt_generation= generation;
}
static void finish_session(int error) {
    assert(error == 0); ++stopped;
    atomic_store(&s_public_state, STARTER_RUNTIME_WAITING);
}
static void room_status(starter_room_state_t state, const char *text) {
    assert(text && text[0]); s_room.view.state=state;
}
'''
code += '\n'.join(function(room, name) for name in (
    'room_app_open', 'room_app_current', 'starter_runtime_room_set_open',
    'room_owns_media', 'room_apply_navigation', 'room_self_name'))
code += r'''
enum { STARTER_RUNTIME_H5_ACTIVE=20, STARTER_RUNTIME_AI_CONNECTING,
       STARTER_RUNTIME_CALL_INCOMING, STARTER_RUNTIME_CALL_CONNECTING };
enum { S3_ACTION_AI_STOP=30, LV_OBJ_FLAG_HIDDEN=1 };
typedef struct { int state; } starter_runtime_status_t;
typedef struct { int ai_phase; } starter_runtime_product_snapshot_t;
typedef struct { char text[192]; bool hidden; } lv_obj_t;
static lv_obj_t header, hint, captions;
static lv_obj_t *s_state=&header;
static struct { lv_obj_t *wake_hint, *caption_panel; unsigned pending_action; }
    s3_ui={&hint,&captions,0};
static struct { bool microphone_muted; } s_preferences;
static char s_voice_feedback[64];
static const char *phase_text(int phase) { (void)phase; return "listening"; }
static void label_set_text_if_changed(lv_obj_t *label, const char *text) {
    snprintf(label->text,sizeof(label->text),"%s",text);
}
static void lv_obj_clear_flag(lv_obj_t *obj, int flag) { assert(flag==1);obj->hidden=false; }
static void lv_obj_add_flag(lv_obj_t *obj, int flag) { assert(flag==1);obj->hidden=true; }
'''
code += function(home, 's3_refresh_home_status')
code += r'''
int main(void) {
    assert(!room_app_open());
    starter_runtime_room_set_open(true);
    assert(room_app_open() && !room_app_current());
    room_apply_navigation();
    assert(room_app_current() && stopped==0 && s_room.dirty);
    assert(!s_room.view.assignment_known);
    unsigned request=atomic_load(&s_room_app_request);
    starter_runtime_room_set_open(true); room_apply_navigation();
    assert(atomic_load(&s_room_app_request)==request && stopped==0);

    /* Going offline retains the assignment and closes PTT before the owner tick. */
    s_room.view.assigned=true; s_room.view.members_ready=true; s_room.view.member_count=3;
    atomic_store(&s_public_state, STARTER_RUNTIME_ROOM_ACTIVE);
    atomic_store(&s_room_ptt_admitted, 29);
    starter_runtime_room_set_open(false);
    assert(ptt_stops==1 && ptt_generation==29 && !atomic_load(&s_room_ptt_admitted));
    assert(!room_app_current() && stopped==0);
    room_apply_navigation();
    assert(stopped==1 && s_room.view.assigned && !s_room.view.members_ready);
    assert(s_room.view.member_count==0 && s_room.view.state==STARTER_ROOM_SUSPENDED);
    room_apply_navigation(); assert(stopped==1);

    /* A previous failed admission cannot suppress user-requested cleanup. */
    starter_runtime_room_set_open(true); room_apply_navigation();
    s_room.blocked=true; s_room.failures=5; s_room.retry_at=10000;
    strcpy(s_room.release.id,"old-room");
    starter_runtime_room_set_open(false); room_apply_navigation();
    assert(!s_room.blocked && s_room.failures==0 && s_room.retry_at==0);
    s_room.release.id[0]=0;

    /* A close/open pair between ticks must not reuse a prior media session. */
    starter_runtime_room_set_open(true); room_apply_navigation();
    atomic_store(&s_public_state, STARTER_RUNTIME_ROOM_CONNECTING);
    starter_runtime_room_set_open(false); starter_runtime_room_set_open(true);
    assert(!room_app_current()); room_apply_navigation();
    assert(room_app_current() && stopped==2 && s_room.view.assigned);

    /* A replacement AI or CALL belongs to another app, including on close. */
    atomic_store(&s_public_state, STARTER_RUNTIME_AI_ACTIVE);
    starter_runtime_room_set_open(false); room_apply_navigation();
    assert(stopped==2 && atomic_load(&s_public_state)==STARTER_RUNTIME_AI_ACTIVE);
    starter_runtime_room_set_open(true); room_apply_navigation();
    atomic_store(&s_public_state, STARTER_RUNTIME_CALL_ACTIVE);
    starter_runtime_room_set_open(false); room_apply_navigation();
    assert(stopped==2 && atomic_load(&s_public_state)==STARTER_RUNTIME_CALL_ACTIVE);

    char name[65]="previous";
    cJSON *value=cJSON_Parse("{\"device_name\":\"Living room\"}");
    room_self_name(value,name); assert(!strcmp(name,"Living room")); cJSON_Delete(value);
    value=cJSON_Parse("{}"); room_self_name(value,name);
    assert(!strcmp(name,"Living room")); cJSON_Delete(value);
    value=cJSON_Parse("{\"device_name\":null}"); room_self_name(value,name);
    assert(!strcmp(name,"Living room")); cJSON_Delete(value);
    value=cJSON_Parse("{\"device_name\":\"\"}"); room_self_name(value,name);
    assert(!name[0]); cJSON_Delete(value);
    value=cJSON_Parse("{\"device_name\":\"01234567890123456789012345678901234567890123456789012345678901234567890\"}");
    room_self_name(value,name); assert(!name[0]); cJSON_Delete(value);

    starter_runtime_product_snapshot_t product={0};
    starter_runtime_status_t runtime={STARTER_RUNTIME_WAITING};
    /* Return from every call state must restore both lines on the same tick. */
    const int active[]={STARTER_RUNTIME_H5_ACTIVE,STARTER_RUNTIME_AI_CONNECTING,
        STARTER_RUNTIME_AI_ACTIVE,STARTER_RUNTIME_CALL_INCOMING,
        STARTER_RUNTIME_CALL_CONNECTING,STARTER_RUNTIME_CALL_ACTIVE,
        STARTER_RUNTIME_ROOM_CONNECTING,STARTER_RUNTIME_ROOM_ACTIVE};
    for(unsigned i=0;i<sizeof(active)/sizeof(active[0]);i++) {
        runtime.state=active[i];s3_refresh_home_status(runtime,&product,false);
        assert(hint.hidden && !captions.hidden && strcmp(header.text,"待唤醒"));
        runtime.state=STARTER_RUNTIME_WAITING;s3_refresh_home_status(runtime,&product,false);
        assert(!hint.hidden && captions.hidden && !strcmp(header.text,"待唤醒"));
        assert(strstr(hint.text,"你好小钛") && strchr(hint.text,'\n') && strstr(hint.text,"点击表情"));
    }
    s3_refresh_home_status(runtime,&product,true);
    assert(hint.hidden && !captions.hidden);
    s_preferences.microphone_muted=true;s3_refresh_home_status(runtime,&product,false);
    assert(!hint.hidden && captions.hidden && strstr(hint.text,"麦克风已关闭"));
    s_preferences.microphone_muted=false;strcpy(s_voice_feedback,"feedback");
    s3_refresh_home_status(runtime,&product,false);
    assert(!hint.hidden && !strcmp(header.text,"feedback"));
    puts("PASS: foreground ownership, rapid navigation, isolated calls, optional server name");
    puts("PASS: wake hint and captions across eight session states, pending, mute and feedback");
}
'''
cjson = root / 'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='p4-room-navigation-') as directory:
    directory = Path(directory)
    source = directory / 'test.c'
    binary = directory / ('test.exe' if os.name == 'nt' else 'test')
    source.write_text(code, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(cjson), str(source), str(cjson / 'cJSON.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
