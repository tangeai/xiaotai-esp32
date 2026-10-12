#!/usr/bin/env python3
"""Render production 320x240 Room views with the actual LVGL/font, offline.

Optional --output DIR keeps PPM screenshots outside the source tree. The host
build lives in a temporary directory; no firmware or device access is needed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_voice_reliability import function

ROOT = Path(__file__).resolve().parents[1]
product = "components/starter_product/src/starter_product.c"
ui = "components/starter_product/src/starter_product_s3_ui.inc"
source = (ROOT / ui).read_text()
header = (ROOT / "components/starter_product/include/starter_product.h").read_text()
tick = function(product, "product_tick")
# The owner tick defers only the offline setup takeover, and only through the hold.
assert tick.count("s3_update_setup(") == 1
setup_gate = re.search(r"^    if \(!s3_room_offline_hold\(now, wifi_connected\)\) \{\n.*?^    \}\n",
                       tick, re.M | re.S)[0]
assert "s3_update_setup(wifi_connected, session_idle, binding_now, verification_code);" in setup_gate
actions = function(ui, "s3_handle_action")
assert (actions.index("S3_ACTION_ROOM_BACK && s_page == PAGE_ROOM") <
        actions.index("if (!wifi_manager_connected()) return true;"))
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include "lvgl.h"
#include "starter_runtime.h"
#define EXT_RAM_BSS_ATTR
#define PRODUCT_TEXT_FONT ui_font_cn_18
#define ESP_LOGI(...) ((void)0)
extern const lv_font_t ui_font_cn_18;
typedef unsigned product_action_t;
typedef unsigned product_page_t;
enum {PAGE_MENU, PAGE_ROOM, PAGE_HOME_FACE, PAGE_NETWORK, PAGE_BINDING};
static unsigned s_page=PAGE_ROOM;
static lv_obj_t *s_clock, *s_wifi_signal, *s_header_title, *s_wifi_signal_bars[4];
static struct {lv_style_t button_style, pressed_style, disabled_style; bool styles_ready;
    bool setup_active; lv_obj_t *binding_code, *binding_hint, *binding_retry;} s3_ui;
static starter_room_view_t model;
static starter_runtime_product_snapshot_t product;
static unsigned pressed, released, submitted, refreshed;
static unsigned opened, closed, view_stops;
static void note_interaction(void) {}
static unsigned feedbacks;
static void s3_feedback(const char *s) {assert(s);feedbacks++;}
static bool enter_page(unsigned p) {s_page=p;return true;}
static void render_page(void);
static bool s3_room_action(product_action_t);
static void on_action(lv_event_t *e);
void starter_runtime_room_set_open(bool open) {if(open)opened++;else closed++;}
esp_err_t starter_runtime_room_create(const char *p) {assert(p);submitted++;return 0;}
esp_err_t starter_runtime_room_join(const char *c,const char *p) {
    assert(c&&p&&!strcmp(c,"001234")&&!strcmp(p,"0123"));submitted++;return 0;
}
esp_err_t starter_runtime_room_leave(void) {return 0;}
esp_err_t starter_runtime_room_refresh(void) {refreshed++;return 0;}
esp_err_t starter_runtime_room_ptt(uint32_t g,bool p) {assert(g);if(p)pressed++;else released++;return 0;}
bool starter_runtime_room_read(uint16_t offset,starter_room_view_t *out) {
    *out=model;out->offset=offset;out->count=out->member_count-offset;
    if(out->count>3)out->count=3;return true;
}
'''
code += "\n".join(re.findall(r"^#define S3_.*", source, re.M)) + "\n"
code += re.search(r"enum \{\s*S3_ACTION_AI_STOP.*?\};", source, re.S)[0]
code += re.search(r"typedef enum \{\s*STARTER_BINDING_CHECKING.*?\} starter_product_binding_state_t;",
                  header, re.S)[0]
code += r'''
static atomic_int s_binding_ui_state = STARTER_BINDING_READY;
static char s_previous_verification_code[17];
static bool platform_client_reconciling(void) {return false;}
static bool platform_client_known_unbound(void) {return false;}
'''
for name in ("set_bg", "make_label", "label_set_text_if_changed", "set_object_visible"):
    code += function(product, name)
code += function(ui, "s3_style_button")
code += function(product, "make_button")
code += function(ui, "s3_icon")
code += function(ui, "s3_view_stop_button")
code += function(ui, "s3_set_enabled")
code += function(product, "render_header_status")
code += function(ui, "s3_header")
code += (ROOT / "components/starter_product/src/starter_product_s3_room.inc").read_text()
code += function(ui, "s3_update_setup")
code += ("static void setup_tick(int64_t now, bool wifi_connected) {\n"
         "    bool session_idle = true, binding_now = false; const char *verification_code = NULL;\n"
         + setup_gate + "}\n")
code += r'''
static void on_action(lv_event_t *e) {
    unsigned action=(uintptr_t)lv_event_get_user_data(e);
    if(action==S3_ACTION_VIEW_STOP)view_stops++;
    else (void)s3_room_action(action);
}
static unsigned char pixels[240][320][3];
static lv_color_t buffer[320*TEST_DISPLAY_LINES];
static uint64_t flushed_pixels;
static void flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*c) {
    flushed_pixels+=(uint64_t)lv_area_get_width(a)*lv_area_get_height(a);
    for(int y=a->y1;y<=a->y2;y++) for(int x=a->x1;x<=a->x2;x++,c++) {
        lv_color32_t p={.full=lv_color_to32(*c)};
        assert(x>=0&&x<320&&y>=0&&y<240);
        pixels[y][x][0]=p.ch.red;pixels[y][x][1]=p.ch.green;pixels[y][x][2]=p.ch.blue;
    }
    lv_disp_flush_ready(d);
}
static void render_page(void) {
    s3_room_navigation();
    s3_room_release();s3_room.keyboard=NULL;s3_room.active_input=NULL;
    lv_obj_t *screen=lv_scr_act();lv_obj_clean(screen);lv_obj_remove_style_all(screen);
    lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    set_bg(screen,lv_color_hex(0x14181B));
    if(s_page==PAGE_ROOM){s3_room_render(screen);s3_room_refresh(1000,&product);}
    lv_obj_update_layout(screen);
}
static void check_objects(lv_obj_t *obj) {
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return;
    lv_area_t a;lv_obj_get_coords(obj,&a);
    if(!(a.x1>=0&&a.x2<320&&a.y1>=0&&a.y2<240))
        fprintf(stderr,"UI bounds: %d,%d..%d,%d text=%s\n",a.x1,a.y1,a.x2,a.y2,
            lv_obj_check_type(obj,&lv_label_class)?lv_label_get_text(obj):"[object]");
    assert(a.x1>=0&&a.x2<320&&a.y1>=0&&a.y2<240);
    /* Textarea children are intentionally clipped by the scrolling field. */
    if(lv_obj_check_type(obj,&lv_textarea_class))return;
    for(unsigned i=0;i<lv_obj_get_child_cnt(obj);i++)check_objects(lv_obj_get_child(obj,i));
}
static void screenshot(const char *dir,const char *name) {
    lv_obj_update_layout(lv_scr_act());lv_refr_now(NULL);
    assert(lv_mem_test()==LV_RES_OK);lv_mem_monitor_t m;lv_mem_monitor(&m);
    printf("UI %s: pool=%u used=%u largest=%u\n",name,m.total_size,m.total_size-m.free_size,m.free_biggest_size);
    if(!dir){check_objects(lv_scr_act());return;}
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
    assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels));fclose(f);
    check_objects(lv_scr_act());
}
static void keypad_press(uint16_t key) {
    lv_btnmatrix_set_selected_btn(s3_room.keyboard,key);
    lv_event_send(s3_room.keyboard,LV_EVENT_VALUE_CHANGED,NULL);
}
static void check_member_text(lv_obj_t *label,const char *expected) {
    assert(lv_label_get_long_mode(label)==LV_LABEL_LONG_DOT);
    const char *shown=lv_label_get_text(label);
    if(strcmp(shown,expected)) {
        const char *dots=strstr(shown,"...");
        assert(dots&&dots>shown&&!strncmp(shown,expected,(size_t)(dots-shown)));
        static bool reported;
        if(!reported){printf("UI ellipsis: shown=%s expected=%s\n",shown,expected);reported=true;}
    }
    /* LVGL 8 temporarily replaces text bytes with dots. Changing mode restores
     * its saved tail, so verify the full model text without widening the UI. */
    lv_label_set_long_mode(label,LV_LABEL_LONG_CLIP);
    assert(!strcmp(lv_label_get_text(label),expected));
    lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
}
static void check_keypad(void) {
    static const char *const expected[]={"1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"确认"};
    lv_obj_update_layout(s3_room.keyboard);
    lv_btnmatrix_t *matrix=(lv_btnmatrix_t *)s3_room.keyboard;
    assert(matrix->btn_cnt==12&&matrix->row_cnt==3);
    for(unsigned i=0;i<12;i++) {
        assert(!strcmp(lv_btnmatrix_get_btn_text(s3_room.keyboard,i),expected[i]));
        const lv_area_t *a=&matrix->button_areas[i];
        assert(lv_area_get_width(a)>=71&&lv_area_get_height(a)==48);
        assert(a->x1>=0&&a->x2<304&&a->y1>=0&&a->y2<152);
        if(i%4)assert(matrix->button_areas[i-1].x2<a->x1);
        if(i>=4)assert(matrix->button_areas[i-4].y2<a->y1);
        if(i!=10)assert(lv_btnmatrix_has_btn_ctrl(s3_room.keyboard,i,LV_BTNMATRIX_CTRL_NO_REPEAT));
    }
    assert(!lv_btnmatrix_has_btn_ctrl(s3_room.keyboard,10,LV_BTNMATRIX_CTRL_NO_REPEAT));
    assert(lv_btnmatrix_has_btn_ctrl(s3_room.keyboard,11,LV_BTNMATRIX_CTRL_CLICK_TRIG));
}
static void check_stable_room_refresh(void) {
    /* Includes unchanged long IDs, which LVGL stores with a mutable DOT tail. */
    s3_room.next_refresh=0;s3_room_refresh(3000,&product);
    lv_obj_update_layout(lv_scr_act());lv_refr_now(NULL);
    uint64_t before=flushed_pixels;
    for(unsigned n=1;n<=10;n++) {
        s3_room_refresh(3000+n*100,&product);lv_refr_now(NULL);
    }
    if(flushed_pixels!=before)
        fprintf(stderr,"Unchanged Room redraw: %llu pixels\n",
                (unsigned long long)(flushed_pixels-before));
    assert(flushed_pixels==before);
    lv_style_value_t color;
    assert(lv_obj_get_local_style_prop(s3_room.ptt,LV_STYLE_BG_COLOR,&color,0)==LV_RES_OK);
    assert(color.color.full==lv_color_hex(model.phase==STARTER_ROOM_SPEAKING?S3_ACCEPT:S3_SURFACE).full);
    s3_room.next_refresh=0;
}
int main(int argc,char **argv) {
    lv_init();lv_disp_draw_buf_t db;lv_disp_draw_buf_init(&db,buffer,NULL,320*TEST_DISPLAY_LINES);
    lv_disp_drv_t d;lv_disp_drv_init(&d);d.hor_res=320;d.ver_res=240;d.draw_buf=&db;d.flush_cb=flush;
    assert(lv_disp_drv_register(&d));const char *out=argc>1?argv[1]:NULL;
    lv_obj_t *stop=s3_view_stop_button(lv_scr_act());
    lv_obj_t *text=lv_obj_get_child(stop,0), *icon=lv_obj_get_child(stop,1);
    lv_obj_update_layout(lv_scr_act());
    assert(!strcmp(lv_label_get_text(text),"结束查看"));
    assert(!strcmp(lv_label_get_text(icon),LV_SYMBOL_STOP));
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(icon,0),&glyph,0xf04d,0));
    assert(!glyph.is_placeholder&&glyph.box_w>0&&glyph.box_h>0);
    static const uint32_t chinese[]={0x7ed3,0x675f,0x67e5,0x770b};
    for(unsigned i=0;i<4;i++) {
        assert(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(text,0),&glyph,chinese[i],0));
        assert(!glyph.is_placeholder&&glyph.box_w>0&&glyph.box_h>0);
    }
    screenshot(out,"00-view-stop");
    lv_area_t ta,ia;lv_obj_get_coords(text,&ta);lv_obj_get_coords(icon,&ia);
    assert(ia.x2<ta.x1&&ta.x2<230&&ia.y1>=194&&ia.y2<236);
    assert(lv_obj_get_width(stop)==140&&lv_obj_get_height(stop)==42);
    lv_event_send(stop,LV_EVENT_CLICKED,NULL);assert(view_stops==1);
    for(unsigned cycle=0;cycle<30;cycle++) {
        s_page=PAGE_ROOM;memset(&model,0,sizeof(model));s3_room.form=false;
        model.assignment_known=true;model.app_open=true;
        render_page();if(!cycle)screenshot(out,"01-lobby");
        model.assignment_known=false;s3_room.next_refresh=0;s3_room_refresh(1100,&product);
        assert(lv_obj_has_state(s3_room.create_button,LV_STATE_DISABLED));
        assert(lv_obj_has_state(s3_room.join_button,LV_STATE_DISABLED));
        model.assignment_known=true;s3_room.next_refresh=0;s3_room_refresh(1200,&product);
        assert(!lv_obj_has_state(s3_room.create_button,LV_STATE_DISABLED));
        s3_room_action(S3_ACTION_ROOM_REFRESH);assert(refreshed==cycle+1);
        s3_room_action(S3_ACTION_ROOM_JOIN);assert(s3_room.keyboard&&s3_room.entry);
        check_keypad();assert(s3_room.active_input==s3_room.entry);
        keypad_press(10);assert(!strcmp(lv_textarea_get_text(s3_room.entry),""));
        keypad_press(9);keypad_press(9);keypad_press(0);keypad_press(1);keypad_press(2);keypad_press(3);
        keypad_press(4);assert(!strcmp(lv_textarea_get_text(s3_room.entry),"001234"));
        keypad_press(10);assert(!strcmp(lv_textarea_get_text(s3_room.entry),"00123"));
        keypad_press(3);
        lv_event_send(s3_room.password,LV_EVENT_CLICKED,NULL);
        assert(s3_room.active_input==s3_room.password);
        assert(lv_obj_has_state(s3_room.password,LV_STATE_FOCUSED));
        assert(!lv_obj_has_state(s3_room.entry,LV_STATE_FOCUSED));
        keypad_press(9);keypad_press(0);keypad_press(1);keypad_press(2);keypad_press(3);
        assert(!strcmp(lv_textarea_get_text(s3_room.password),"0123"));
        keypad_press(10);assert(!strcmp(lv_textarea_get_text(s3_room.password),"012"));keypad_press(2);
        lv_event_send(s3_room.entry,LV_EVENT_CLICKED,NULL);
        assert(s3_room.active_input==s3_room.entry);
        lv_event_send(s3_room.password,LV_EVENT_FOCUSED,NULL);
        assert(s3_room.active_input==s3_room.password);
        if(!cycle)screenshot(out,"02-join");
        keypad_press(11);assert(submitted==cycle+1);
        s3_room_action(S3_ACTION_ROOM_CREATE);check_keypad();
        assert(s3_room.active_input==s3_room.password&&!s3_room.entry);
        assert(!strcmp(lv_textarea_get_text(s3_room.password),""));
        if(!cycle)screenshot(out,"03-create");
        s3_room_action(S3_ACTION_ROOM_BACK);
        model.assigned=true;model.phase=STARTER_ROOM_LISTENING;model.generation=7;model.snapshot_ready=true;
        model.member_count=4;model.count=3;strcpy(model.room_code,"001234");
        model.members[0].self=true;strcpy(model.members[1].device_id,"guest");
        strcpy(model.members[0].device_id,"self-device-123");
        strcpy(model.self_name,"书房");
        memset(model.members[2].device_id,'9',64);model.members[2].device_id[64]=0;
        model.members[1].speaking=true;
        product.contact_count=1;strcpy(product.contacts[0].id,"guest");strcpy(product.contacts[0].name,"客厅的小钛");
        render_page();if(!cycle)screenshot(out,"04-listening");
        check_stable_room_refresh();
        if(strcmp(lv_label_get_text(s3_room.members[1]),"客厅的小钛"))
            fprintf(stderr,"UI alias cycle=%u value=%s\n",cycle,lv_label_get_text(s3_room.members[1]));
        assert(!strcmp(lv_label_get_text(s3_room.members[1]),"客厅的小钛"));
        assert(!strcmp(lv_label_get_text(s3_room.members[0]),"书房"));
        assert(!lv_obj_has_flag(s3_room.self[0],LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(s3_room.leave,0)),"退出房间"));
        model.self_name[0]=0;s3_room.next_refresh=0;s3_room_refresh(1500,&product);
        check_member_text(s3_room.members[0],"self-device-123");
        check_member_text(s3_room.members[2],model.members[2].device_id);
        assert(!lv_obj_has_flag(s3_room.self[0],LV_OBJ_FLAG_HIDDEN));
        lv_event_send(s3_room.ptt,LV_EVENT_PRESSED,NULL);assert(s3_room.held_generation==7);
        model.phase=STARTER_ROOM_SPEAKING;s3_room.next_refresh=0;s3_room_refresh(2000,&product);
        lv_obj_update_layout(lv_scr_act());
        assert(!strcmp(lv_label_get_text(s3_room.status[0]),"讲话中"));
        if(!cycle)screenshot(out,"05-speaking");
        check_stable_room_refresh();
        lv_event_send(s3_room.ptt,LV_EVENT_PRESS_LOST,NULL);assert(!s3_room.held_generation);
        lv_event_send(s3_room.ptt,LV_EVENT_PRESSED,NULL);s3_room_action(S3_ACTION_ROOM_BACK);
        assert(!s3_room.held_generation&&s_page==PAGE_MENU);
        assert(lv_mem_test()==LV_RES_OK);
    }
    assert(pressed==60&&released==60);
    assert(opened==30&&closed==30);
    /* A short STA loss keeps the open Room; runtime owns media/lease recovery. */
    s_page=PAGE_ROOM;s3_room.form=false;model.phase=STARTER_ROOM_LISTENING;model.error=0;
    render_page();assert(opened==31&&closed==30);
    setup_tick(10000,false);
    assert(s_page==PAGE_ROOM&&!s3_ui.setup_active&&s3_room.offline_until==20000&&closed==30);
    s3_room_refresh(10000,&product);
    assert(!strcmp(lv_label_get_text(s3_room.state),"网络未连接"));
    assert(lv_obj_has_state(s3_room.leave,LV_STATE_DISABLED));
    model.phase=STARTER_ROOM_ERROR;model.error=259;s3_room.next_refresh=0;s3_room_refresh(12000,&product);
    assert(!strcmp(lv_label_get_text(s3_room.state),"网络未连接"));
    screenshot(out,"06-offline-hold");
    for(int64_t t=10000;t<20000;t+=100){setup_tick(t,false);assert(s_page==PAGE_ROOM&&closed==30);}
    setup_tick(19950,true);
    assert(s_page==PAGE_ROOM&&!s3_room.offline_until&&!s3_ui.setup_active&&closed==30);
    model.phase=STARTER_ROOM_LISTENING;s3_room.next_refresh=0;s3_room_refresh(20000,&product);
    assert(!strcmp(lv_label_get_text(s3_room.state),"收听中"));
    assert(!lv_obj_has_state(s3_room.leave,LV_STATE_DISABLED));
    /* A longer loss reaches the bounded deadline; the takeover then closes Room. */
    for(int64_t t=30000;t<40000;t+=100){setup_tick(t,false);assert(s_page==PAGE_ROOM&&closed==30);}
    setup_tick(40000,false);
    assert(s_page==PAGE_NETWORK&&s3_ui.setup_active&&closed==31);
    setup_tick(40100,false);assert(!s3_room.offline_until&&s_page==PAGE_NETWORK);
    setup_tick(50000,true);assert(s_page==PAGE_HOME_FACE&&!s3_ui.setup_active&&opened==31);
    /* Leaving during a hold ends it; the normal offline takeover follows. */
    s_page=PAGE_ROOM;render_page();assert(opened==32);
    setup_tick(51000,false);assert(s_page==PAGE_ROOM&&s3_room.offline_until);
    s3_room_action(S3_ACTION_ROOM_BACK);assert(s_page==PAGE_MENU&&closed==32);
    setup_tick(51100,false);assert(s_page==PAGE_NETWORK&&!s3_room.offline_until);
    setup_tick(52000,true);assert(s_page==PAGE_HOME_FACE);
    /* Lobby and an open form: network intents wait for the connection. */
    memset(&model,0,sizeof(model));model.assignment_known=true;model.app_open=true;
    s_page=PAGE_ROOM;s3_room.form=false;render_page();assert(opened==33);
    s3_room_action(S3_ACTION_ROOM_CREATE);assert(s3_room.form&&s3_room.active_input==s3_room.password);
    unsigned before=submitted, notices=feedbacks;
    setup_tick(60000,false);
    keypad_press(11);assert(submitted==before&&feedbacks==notices+1&&s3_room.form);
    setup_tick(60500,true);
    keypad_press(11);assert(submitted==before+1&&!s3_room.form);
    setup_tick(61000,false);s3_room_refresh(61000,&product);
    assert(!strcmp(lv_label_get_text(s3_room.state),"网络未连接"));
    assert(lv_obj_has_state(s3_room.create_button,LV_STATE_DISABLED));
    assert(lv_obj_has_state(s3_room.join_button,LV_STATE_DISABLED));
    if(out)screenshot(out,"07-lobby-offline-hold");
    setup_tick(62000,true);s3_room.next_refresh=0;s3_room_refresh(62000,&product);
    assert(!lv_obj_has_state(s3_room.create_button,LV_STATE_DISABLED));
    assert(!strcmp(lv_label_get_text(s3_room.state),"尚未加入房间")&&closed==32);
    assert(lv_mem_test()==LV_RES_OK);
    puts("PASS: production LVGL 320x240 bounds, 32 KiB pool, 4x3 keypad/editing/focus, aliases, PTT lost-touch/page teardown x30, "
         "bounded Room offline hold with inline status, reconnect without teardown, expiry/back takeover, offline submit refused");
    return 0;
}
'''
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
parser.add_argument("--buffer-lines", type=int, choices=(10, 20, 240), default=20)
args = parser.parse_args()
if args.output:
    args.output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="s3-room-ui-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(f"#define TEST_DISPLAY_LINES {args.buffer_lines}\n" + code)
    (temp / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 258\n")
    (temp / "lv_conf.h").write_text("#define LV_CONF_H\n#define LV_FONT_FMT_TXT_LARGE 1\n"
        "#define LV_COLOR_DEPTH 16\n#define LV_MEM_SIZE (32U * 1024U)\n"
        "#define LV_USE_LOG 1\n#define LV_LOG_PRINTF 1\n#define LV_USE_ASSERT_MALLOC 1\n"
        "#define LV_USE_ASSERT_STYLE 1\n#define LV_USE_ASSERT_OBJ 1\n"
        "#define LV_ASSERT_HANDLER assert(0);\n#define LV_ASSERT_HANDLER_INCLUDE <assert.h>\n")
    lvgl = ROOT / "managed_components/lvgl__lvgl"
    (temp / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(room_ui C)
set(LV_CONF_PATH "{temp}/lv_conf.h" CACHE STRING "" FORCE)
add_subdirectory("{lvgl}" lvgl)
add_executable(room_ui test.c "{ROOT}/components/starter_product/src/ui_font_cn_18.c"
    "{ROOT}/components/starter_product/src/ui_font_cn_18_supplement.c")
target_include_directories(room_ui PRIVATE "{temp}" "{ROOT}/components/starter_runtime/include")
target_link_libraries(room_ui PRIVATE lvgl m)
''')
    subprocess.run(["cmake", "-S", str(temp), "-B", str(temp / "out"), "-DCMAKE_BUILD_TYPE=Debug"], check=True, stdout=subprocess.DEVNULL)
    built = subprocess.run(["cmake", "--build", str(temp / "out"), "--target", "room_ui", "-j4"], capture_output=True, text=True)
    if built.returncode:
        print(built.stderr[-16000:])
        built.check_returncode()
    subprocess.run([str(temp / "out/room_ui"), *([str(args.output)] if args.output else [])], check=True, timeout=30)
