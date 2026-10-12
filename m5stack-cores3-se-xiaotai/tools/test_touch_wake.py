#!/usr/bin/env python3
"""Drive real LVGL input through the production dark-screen wake filter.

The home face tap, WeChat shortcut, Room PTT and a settings switch are built
from production code. A first contact on a dark screen, physical or AT+TAP,
must only wake it; the next contact must act normally. No hardware is used.
"""
from pathlib import Path
import re
import subprocess
import tempfile
from test_voice_reliability import function

ROOT = Path(__file__).resolve().parents[1]
product = "components/starter_product/src/starter_product.c"
ui = "components/starter_product/src/starter_product_s3_ui.inc"
room = "components/starter_product/src/starter_product_s3_room.inc"
product_text = (ROOT / product).read_text(encoding="utf-8")
ui_text = (ROOT / ui).read_text(encoding="utf-8")
room_text = (ROOT / room).read_text(encoding="utf-8")


def between(text: str, first: str, last: str) -> str:
    start = text.index(first)
    return text[start:text.index(last, start)]


wake = between(product_text, "/* Per pointer device, LVGL task only.", "static esp_err_t preferences_write(")
reader = between(product_text, "static EXT_RAM_BSS_ATTR touch_wake_t s_touch_wake;", "static esp_err_t lvgl_init(void)")
debug = between(product_text, "EXT_RAM_BSS_ATTR static struct {\n    lv_point_t from, to;",
                "esp_err_t starter_product_debug_state")
init = function(product, "lvgl_init")
install = re.search(r"^    s_touch_read = touch_input->driver->read_cb;\n"
                    r"    touch_input->driver->read_cb = core_touch_read;\n", init, re.M)
assert install, "real touch indev must be wrapped by core_touch_read"
assert init.index("lvgl_port_add_touch(&touch)") < install.start()
assert "debug_input.read_cb = debug_touch_read;" in init
assert "touch_wake_filter(&s_debug_touch.wake, data);" in debug
home = function(ui, "s3_render_home")
face_tap = re.search(r"^    lv_obj_t \*tap = lv_obj_create\(screen\);\n.*?on_home_tap, LV_EVENT_CLICKED, NULL\);\n",
                     home, re.M | re.S)[0]
wechat = re.search(r"^    s3_ui\.wechat_button = make_button\(screen, .*?\);\n", home, re.M | re.S)[0]
ptt = re.search(r"^    s3_room\.ptt = make_button\(screen, .*?\n(?:.*\n)*?.*s3_room_ptt_event, LV_EVENT_ALL, NULL\);\n",
                function(room, "s3_room_render"), re.M)[0]
actions = re.search(r"typedef enum \{\s*ACTION_AI.*?\} product_action_t;", product_text, re.S)[0]
s3_actions = re.search(r"enum \{\s*S3_ACTION_AI_STOP.*?\};", ui_text, re.S)[0]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "starter_runtime.h"
#define EXT_RAM_BSS_ATTR
#define CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE 1
#define PRODUCT_TEXT_FONT lv_font_montserrat_14
#define LCD_H_RES 320
#define LCD_V_RES 240
#define FACE_CANVAS_H 108
static const char *TAG = "touch_wake";
static unsigned info_logs, error_logs;
#define ESP_LOGI(tag, ...) ((void)(tag), ++info_logs)
#define ESP_LOGE(tag, ...) ((void)(tag), ++error_logs)
'''
code += actions + "\n" + s3_actions + "\n"
code += "\n".join(re.findall(r"^#define S3_.*", ui_text, re.M)) + "\n"
code += r'''
static int64_t clock_us = INT64_C(5000000000);
static int64_t esp_timer_get_time(void) { return clock_us; }
static esp_err_t backlight_error;
static unsigned backlight_writes, face_resumes, face_pauses;
static bool backlight_on;
static esp_err_t starter_board_backlight(bool on) {
    ++backlight_writes; if (!backlight_error) backlight_on = on; return backlight_error;
}
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
static void s3_face_visibility_changed(bool awake) { if (awake) ++face_resumes; else ++face_pauses; }
static bool s_display_awake = true;
static int64_t s_last_interaction_ms;
static bool s_started = true, locked;
static bool lvgl_port_lock(unsigned timeout) { assert(timeout == 100 && !locked); return locked = true; }
static void lvgl_port_unlock(void) { assert(locked); locked = false; }
static struct {lv_style_t button_style, pressed_style, disabled_style; bool styles_ready; lv_obj_t *wechat_button;} s3_ui;
static struct {starter_room_view_t view; uint32_t held_generation; lv_obj_t *ptt;} s3_room;
static unsigned action_counts[256], ai_requests, ptt_down, ptt_up, feedback;
static void on_action(lv_event_t *e) {
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    assert(action < 256); ++action_counts[action];
}
static bool s3_handle_action(product_action_t action) { assert(action == ACTION_AI); ++ai_requests; return true; }
static void s3_feedback(const char *text) { assert(text); ++feedback; }
esp_err_t starter_runtime_room_ptt(uint32_t generation, bool pressed) {
    assert(generation == 7); if (pressed) ++ptt_down; else ++ptt_up; return ESP_OK;
}
'''
code += function(product, "monotonic_ms")
code += function(product, "backlight_set")
code += function(product, "note_interaction")
code += wake
code += function(ui, "s3_style_button")
code += function(product, "make_button")
code += function(ui, "s3_switch")
code += function(product, "on_home_tap")
code += function(room, "s3_room_release")
code += function(room, "s3_room_ptt_event")
code += reader
code += debug
code += "static void install_touch(lv_indev_t *touch_input) {\n" + install[0] + "}\n"
code += "static void build_home(lv_obj_t *screen) {\n" + face_tap + wechat + "}\n"
code += "static void build_ptt(lv_obj_t *screen) {\n" + ptt + "}\n"
code += r'''
enum {E_PRESSED, E_PRESSING, E_LONG, E_SHORT, E_CLICKED, E_RELEASED, E_LOST, E_VALUE, E_COUNT};
typedef struct {unsigned n[E_COUNT];} counter_t;
static counter_t c_screen, c_face, c_wechat, c_ptt, c_switch;
static counter_t *const counters[] = {&c_screen, &c_face, &c_wechat, &c_ptt, &c_switch};
static void count_event(lv_event_t *e) {
    counter_t *c = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: ++c->n[E_PRESSED]; break;
    case LV_EVENT_PRESSING: ++c->n[E_PRESSING]; break;
    case LV_EVENT_LONG_PRESSED: ++c->n[E_LONG]; break;
    case LV_EVENT_SHORT_CLICKED: ++c->n[E_SHORT]; break;
    case LV_EVENT_CLICKED: ++c->n[E_CLICKED]; break;
    case LV_EVENT_RELEASED: ++c->n[E_RELEASED]; break;
    case LV_EVENT_PRESS_LOST: ++c->n[E_LOST]; break;
    case LV_EVENT_VALUE_CHANGED: ++c->n[E_VALUE]; break;
    default: break;
    }
}
static unsigned input_events(void) {
    unsigned total = 0;
    for (unsigned i = 0; i < sizeof(counters) / sizeof(*counters); ++i)
        for (unsigned j = 0; j < E_COUNT; ++j) total += counters[i]->n[j];
    return total;
}
static unsigned actions_total(void) {
    unsigned total = ai_requests + ptt_down + ptt_up;
    for (unsigned i = 0; i < 256; ++i) total += action_counts[i];
    return total;
}
static void reset_counts(void) {
    for (unsigned i = 0; i < sizeof(counters) / sizeof(*counters); ++i) memset(counters[i], 0, sizeof(counter_t));
    memset(action_counts, 0, sizeof(action_counts)); ai_requests = ptt_down = ptt_up = 0;
}

static lv_color_t buffer[320 * 20];
static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) { (void)a; (void)c; lv_disp_flush_ready(d); }
static bool finger;
static lv_point_t finger_at;
static void panel_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d; data->point = finger_at;
    data->state = finger ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static lv_indev_t *panel, *injector;
/* One LVGL input period (30 ms) for both pointer devices. */
static void period(void) {
    clock_us += 30000; lv_tick_inc(30);
    lv_indev_read_timer_cb(panel->driver->read_timer);
    lv_indev_read_timer_cb(injector->driver->read_timer);
}
static void hold(int x, int y, unsigned ms) {
    finger_at = (lv_point_t){(lv_coord_t)x, (lv_coord_t)y}; finger = true;
    for (unsigned t = 0; t < ms; t += 30) period();
}
static void lift(void) { finger = false; for (unsigned i = 0; i < 4; ++i) period(); }
static void tap(int x, int y) { hold(x, y, 150); lift(); }
static void sleep_screen(void) {
    backlight_set(false); assert(!s_display_awake && !backlight_on);
}
static void expect_consumed(unsigned wakes_before) {
    assert(input_events() == 0 && actions_total() == 0);
    assert(s_touch_wakes == wakes_before + 1 && info_logs == wakes_before + 1);
    assert(!s3_room.held_generation);
}
static void inject(lv_point_t at) {
    assert(starter_product_debug_tap((uint16_t)at.x, (uint16_t)at.y) == ESP_OK);
    for (unsigned i = 0; i < 10; ++i) period();
}

int main(void) {
    lv_init();
    lv_disp_draw_buf_t db; lv_disp_draw_buf_init(&db, buffer, NULL, 320 * 20);
    lv_disp_drv_t d; lv_disp_drv_init(&d); d.hor_res = 320; d.ver_res = 240; d.draw_buf = &db; d.flush_cb = flush;
    assert(lv_disp_drv_register(&d));
    static lv_indev_drv_t panel_drv, debug_input;
    lv_indev_drv_init(&panel_drv); panel_drv.type = LV_INDEV_TYPE_POINTER; panel_drv.read_cb = panel_read;
    panel = lv_indev_drv_register(&panel_drv); assert(panel);
    install_touch(panel);
    assert(panel->driver->read_cb == core_touch_read && s_touch_read == panel_read);
    lv_indev_drv_init(&debug_input); debug_input.type = LV_INDEV_TYPE_POINTER; debug_input.read_cb = debug_touch_read;
    injector = lv_indev_drv_register(&debug_input); assert(injector);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, count_event, LV_EVENT_ALL, &c_screen);
    build_home(screen);
    lv_obj_t *face = lv_obj_get_child(screen, 0);
    assert(face && s3_ui.wechat_button);
    lv_obj_add_event_cb(face, count_event, LV_EVENT_ALL, &c_face);
    lv_obj_add_event_cb(s3_ui.wechat_button, count_event, LV_EVENT_ALL, &c_wechat);
    s3_room.view.generation = 7;
    build_ptt(screen);
    lv_obj_add_event_cb(s3_room.ptt, count_event, LV_EVENT_ALL, &c_ptt);
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row); lv_obj_set_pos(row, 0, 0); lv_obj_set_size(row, 320, 48);
    lv_obj_t *control = s3_switch(row, ACTION_SPEAKER_MUTE);
    lv_obj_add_event_cb(control, count_event, LV_EVENT_ALL, &c_switch);
    lv_obj_update_layout(screen);
    const lv_point_t at_face = {160, 120}, at_wechat = {298, 172}, at_ptt = {100, 214};
    const lv_point_t at_switch = {262, 24}, at_screen = {20, 60};
    const lv_point_t targets[] = {at_face, at_wechat, at_ptt, at_switch, at_screen};

    /* 1. Dark screen: every first contact only wakes and restarts the timer. */
    for (unsigned round = 0; round < 20; ++round) {
        for (unsigned i = 0; i < sizeof(targets) / sizeof(*targets); ++i) {
            reset_counts(); sleep_screen();
            unsigned wakes = s_touch_wakes; info_logs = s_touch_wakes;
            int64_t pressed_ms = (clock_us + 30000) / 1000;
            tap(targets[i].x, targets[i].y);
            expect_consumed(wakes);
            assert(s_display_awake && backlight_on && s_last_interaction_ms == pressed_ms);
            assert(!lv_obj_has_state(control, LV_STATE_CHECKED));
        }
    }
    /* 2. A long hold and a swipe across widgets on a dark screen. */
    reset_counts(); sleep_screen(); info_logs = s_touch_wakes;
    unsigned wakes = s_touch_wakes;
    hold(at_ptt.x, at_ptt.y, 1500); lift();
    expect_consumed(wakes);
    reset_counts(); sleep_screen(); info_logs = s_touch_wakes; wakes = s_touch_wakes;
    finger = true;
    for (int x = 60; x < 310; x += 10) { finger_at = (lv_point_t){(lv_coord_t)x, 120}; period(); }
    for (int y = 120; y < 230; y += 10) { finger_at = (lv_point_t){300, (lv_coord_t)y}; period(); }
    lift();
    expect_consumed(wakes);

    /* 3. Awake: the next contact acts normally on each production widget. */
    wakes = s_touch_wakes; reset_counts();
    tap(at_face.x, at_face.y);
    assert(ai_requests == 1 && c_face.n[E_PRESSED] == 1 && c_face.n[E_CLICKED] == 1);
    tap(at_wechat.x, at_wechat.y);
    assert(action_counts[S3_ACTION_WECHAT] == 1 && c_wechat.n[E_CLICKED] == 1);
    hold(at_ptt.x, at_ptt.y, 300);
    assert(ptt_down == 1 && s3_room.held_generation == 7 && !ptt_up);
    lift();
    assert(ptt_up == 1 && !s3_room.held_generation);
    tap(at_switch.x, at_switch.y);
    assert(action_counts[ACTION_SPEAKER_MUTE] == 1 && c_switch.n[E_VALUE] == 1);
    assert(lv_obj_has_state(control, LV_STATE_CHECKED));
    assert(s_touch_wakes == wakes && c_screen.n[E_PRESSED] == 0);

    /* 4. A contact already delivered stays delivered if the screen sleeps. */
    reset_counts();
    hold(at_ptt.x, at_ptt.y, 90);
    assert(ptt_down == 1);
    sleep_screen();
    hold(at_ptt.x, at_ptt.y, 600);
    assert(ptt_down == 1 && !ptt_up && !c_ptt.n[E_LOST] && s3_room.held_generation == 7);
    lift();
    assert(ptt_up == 1 && c_ptt.n[E_RELEASED] == 1 && s_touch_wakes == wakes);
    reset_counts(); info_logs = s_touch_wakes;
    tap(at_wechat.x, at_wechat.y);   /* Still dark: consumed, wakes. */
    expect_consumed(wakes);
    wakes = s_touch_wakes;

    /* 5. A failed backlight write keeps every new dark contact consumed. */
    reset_counts(); sleep_screen(); backlight_error = -1; info_logs = s_touch_wakes;
    unsigned errors = error_logs;
    for (unsigned i = 0; i < 3; ++i) {
        tap(at_wechat.x, at_wechat.y);
        assert(!s_display_awake && input_events() == 0 && actions_total() == 0);
    }
    assert(s_touch_wakes == wakes + 3 && error_logs == errors + 3);
    backlight_error = 0; wakes = s_touch_wakes;
    tap(at_wechat.x, at_wechat.y);
    assert(s_display_awake && s_touch_wakes == wakes + 1 && actions_total() == 0);
    wakes = s_touch_wakes;

    /* 6. AT+TAP: dark screen only wakes; an awake screen gets a real click. */
    reset_counts(); sleep_screen(); info_logs = s_touch_wakes;
    inject(at_wechat);
    expect_consumed(wakes);
    assert(s_display_awake);
    inject(at_wechat);
    assert(action_counts[S3_ACTION_WECHAT] == 1 && c_wechat.n[E_CLICKED] == 1);
    inject(at_face);
    assert(ai_requests == 1 && s_touch_wakes == wakes + 1);
    assert(lv_mem_test() == LV_RES_OK && !locked);
    printf("PASS: dark-screen first touch only wakes (5 targets x20, hold, swipe, backlight failure, AT+TAP); "
           "awake face/WeChat/PTT/switch act; delivered press survives sleep; wakes=%lu\n",
           (unsigned long)s_touch_wakes);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="touch-wake-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(code, encoding="utf-8")
    (temp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
                                    "#define ESP_ERR_INVALID_ARG 0x102\n#define ESP_ERR_INVALID_STATE 0x103\n"
                                    "#define ESP_ERR_TIMEOUT 0x107\n")
    (temp / "lv_conf.h").write_text("#define LV_CONF_H\n#define LV_COLOR_DEPTH 16\n"
        "#define LV_MEM_SIZE (32U * 1024U)\n#define LV_FONT_MONTSERRAT_14 1\n"
        "#define LV_USE_LOG 1\n#define LV_LOG_PRINTF 1\n#define LV_LOG_LEVEL LV_LOG_LEVEL_ERROR\n"
        "#define LV_USE_ASSERT_MALLOC 1\n#define LV_USE_ASSERT_STYLE 1\n#define LV_USE_ASSERT_OBJ 1\n"
        "#define LV_ASSERT_HANDLER assert(0);\n#define LV_ASSERT_HANDLER_INCLUDE <assert.h>\n")
    lvgl = ROOT / "managed_components/lvgl__lvgl"
    (temp / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(touch_wake C)
set(LV_CONF_PATH "{temp}/lv_conf.h" CACHE STRING "" FORCE)
add_subdirectory("{lvgl}" lvgl)
add_executable(touch_wake test.c)
target_compile_options(touch_wake PRIVATE -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined)
target_link_options(touch_wake PRIVATE -fsanitize=address,undefined)
target_include_directories(touch_wake PRIVATE "{temp}" "{ROOT}/components/starter_runtime/include")
target_link_libraries(touch_wake PRIVATE lvgl m)
''')
    subprocess.run(["cmake", "-S", str(temp), "-B", str(temp / "out"), "-DCMAKE_BUILD_TYPE=Debug"],
                   check=True, stdout=subprocess.DEVNULL)
    built = subprocess.run(["cmake", "--build", str(temp / "out"), "--target", "touch_wake", "-j4"],
                           capture_output=True, text=True)
    if built.returncode:
        print(built.stdout[-8000:], built.stderr[-16000:])
        built.check_returncode()
    subprocess.run([str(temp / "out/touch_wake")], check=True, timeout=60)
