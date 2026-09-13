#!/usr/bin/env python3
"""Execute S3 onboarding navigation and AI admission; no display/driver stubs in firmware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
ui = (root / 'components/starter_product/src/starter_product_s3_ui.inc').read_text()
runtime = (root / 'components/starter_runtime/src/starter_runtime.c').read_text()
product = (root / 'components/starter_product/src/starter_product.c').read_text()
setup = ui[ui.index('static void s3_update_setup('):ui.index('static void s3_reset_page(')]
# The following helper renders other pages; test only the complete setup owner.
depth = 0
start = setup.index('{')
for i in range(start, len(setup)):
    depth += (setup[i] == '{') - (setup[i] == '}')
    if depth == 0:
        setup = setup[:i + 1]
        break
gate = runtime[runtime.index('static bool ai_network_ready('):runtime.index('static void begin_ai_session(')]
entry = runtime[runtime.index('esp_err_t starter_runtime_ai_start(void)'):
                runtime.index('esp_err_t starter_runtime_ai_stop(void)')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ESP_LOGI(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_TIMEOUT -3
#define EVENT_AI_START 7
#define LV_OBJ_FLAG_HIDDEN 1
enum { PAGE_HOME_FACE, PAGE_NETWORK, PAGE_BINDING, PAGE_CALL, PAGE_SETTINGS };
enum { STARTER_BINDING_CHECKING, STARTER_BINDING_REQUIRED, STARTER_BINDING_FAILED, STARTER_BINDING_READY };
typedef int starter_product_binding_state_t;
typedef int product_page_t;
typedef int esp_err_t;
typedef struct { int type; uint32_t generation; } runtime_event_t;
static struct { bool setup_active; void *binding_code; void *binding_hint; void *binding_retry; } s3_ui;
static atomic_int s_binding_ui_state;
static product_page_t s_page;
static char s_previous_verification_code[32];
static bool wifi;
static bool reconciling, unbound, retry_hidden;
static bool platform_client_reconciling(void) {return reconciling;}
static bool platform_client_known_unbound(void) {return unbound;}
static void lv_obj_clear_flag(void *o,int f) {(void)o;assert(f==1);retry_hidden=false;}
static void lv_obj_add_flag(void *o,int f) {(void)o;assert(f==1);retry_hidden=true;}
static int render_count, enqueued;
static bool wifi_manager_connected(void) { return wifi; }
static int enqueue_simple(int type) { assert(type == EVENT_AI_START); ++enqueued; return 0; }
static bool queue_event(const runtime_event_t *e) { assert(e->type == EVENT_AI_START); ++enqueued; return true; }
static void note_interaction(void) {}
static void render_page(void) { ++render_count; }
static void label_set_text_if_changed(void *label, const char *text) { (void)label; (void)text; }
''' + gate + entry + setup + r'''
int main(void) {
    for (int page = PAGE_HOME_FACE; page <= PAGE_SETTINGS; ++page) {
        for (int idle = 0; idle < 2; ++idle) {
            s_page = page; s3_ui.setup_active = false;
            atomic_store(&s_binding_ui_state, STARTER_BINDING_READY);
            s3_update_setup(false, idle, false, NULL);
            assert(s_page == PAGE_NETWORK && s3_ui.setup_active);
            int count = render_count;
            for (int i = 0; i < 1000; ++i) s3_update_setup(false, idle, false, NULL);
            assert(render_count == count); /* No repeated redraw while offline. */
        }
    }
    s3_update_setup(true, true, false, NULL);
    assert(s_page == PAGE_HOME_FACE && !s3_ui.setup_active);
    atomic_store(&s_binding_ui_state, STARTER_BINDING_CHECKING);
    s3_update_setup(true, true, false, NULL);
    assert(s_page == PAGE_BINDING); /* Do not stay on Wi-Fi while checking identity. */
    atomic_store(&s_binding_ui_state, STARTER_BINDING_REQUIRED);
    s3_update_setup(true, true, true, "123456");
    assert(s_page == PAGE_BINDING && s3_ui.setup_active);
    s3_update_setup(false, false, true, "123456");
    assert(s_page == PAGE_NETWORK && s3_ui.setup_active);
    atomic_store(&s_binding_ui_state, STARTER_BINDING_READY);
    s_page = PAGE_CALL; /* A retained session has reappeared after network recovery. */
    s3_update_setup(true, false, false, NULL);
    assert(s_page == PAGE_CALL && !s3_ui.setup_active);
    reconciling=true;unbound=true;
    s3_ui.binding_code=(void*)1;
    s3_update_setup(true, false, false, NULL);
    assert(s_page == PAGE_BINDING && s3_ui.setup_active && retry_hidden);
    atomic_store(&s_binding_ui_state, STARTER_BINDING_FAILED);
    s3_update_setup(true, true, false, NULL);assert(!retry_hidden);
    reconciling=false;
    for (unsigned i = 1; i <= 1000; ++i) {
        assert(starter_runtime_ai_start() == ESP_ERR_INVALID_STATE);
        assert(starter_runtime_ai_start_from_wake(i) == ESP_ERR_INVALID_STATE);
    }
    assert(enqueued == 0);
    wifi = true;
    assert(starter_runtime_ai_start() == ESP_OK);
    assert(starter_runtime_ai_start_from_wake(123) == ESP_OK && enqueued == 2);
    assert(starter_runtime_ai_start_from_wake(0) == ESP_ERR_INVALID_ARG);
    puts("PASS: offline page lock, no redraw loop, binding/online restore, retained call page, offline AI admission and online unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='xiaotai-offline-') as d:
    path = Path(d)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
voice = product[product.index('static void handle_voice_result('):product.index('static void on_action(')]
assert voice.index('!wifi_manager_connected()') < voice.index('switch (result->intent)')
assert 'starter_media_cancel_ai_preroll(result->wake_token)' in voice
action = ui[ui.index('static bool s3_handle_action('):ui.index('static void s3_refresh_ui(')]
assert action.index('!wifi_manager_connected()') < action.index('starter_runtime_status()')
owner = runtime[runtime.index('static void begin_ai_session('):runtime.index('static void handle_ai_token(')]
assert owner.index('!ai_network_ready()') < owner.index('starter_tirtc_accept_h5(false)')
