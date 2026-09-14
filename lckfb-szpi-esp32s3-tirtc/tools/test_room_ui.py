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
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lvgl.h"
#include "starter_runtime.h"
#define EXT_RAM_BSS_ATTR
#define PRODUCT_MODERN_UI 1
#define PRODUCT_TEXT_FONT ui_font_cn_18
extern const lv_font_t ui_font_cn_18;
typedef unsigned product_action_t;
enum {PAGE_MENU, PAGE_ROOM};
static unsigned s_page=PAGE_ROOM;
static lv_obj_t *s_clock, *s_wifi_signal, *s_header_title, *s_wifi_signal_bars[4];
static struct {lv_style_t button_style, pressed_style, disabled_style; bool styles_ready;} s3_ui;
static starter_room_view_t model;
static starter_runtime_product_snapshot_t product;
static unsigned pressed, released, submitted, refreshed;
static unsigned opened, closed;
static void note_interaction(void) {}
static void s3_feedback(const char *s) {assert(s);}
static bool enter_page(unsigned p) {s_page=p;return true;}
static void render_page(void);
static bool s3_room_action(product_action_t);
static void on_action(lv_event_t *e) {(void)s3_room_action((uintptr_t)lv_event_get_user_data(e));}
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
for name in ("set_bg", "make_label", "label_set_text_if_changed"):
    code += function(product, name)
code += function(ui, "s3_style_button")
code += function(product, "make_button")
code += function(ui, "s3_icon")
code += function(ui, "s3_set_enabled")
code += function(product, "render_header_status")
code += function(ui, "s3_header")
code += (ROOT / "components/starter_product/src/starter_product_s3_room.inc").read_text()
code += r'''
static unsigned char pixels[240][320][3];
static lv_color_t buffer[320*20];
static void flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*c) {
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
int main(int argc,char **argv) {
    lv_init();lv_disp_draw_buf_t db;lv_disp_draw_buf_init(&db,buffer,NULL,320*20);
    lv_disp_drv_t d;lv_disp_drv_init(&d);d.hor_res=320;d.ver_res=240;d.draw_buf=&db;d.flush_cb=flush;
    assert(lv_disp_drv_register(&d));const char *out=argc>1?argv[1]:NULL;
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
        if(strcmp(lv_label_get_text(s3_room.members[1]),"客厅的小钛"))
            fprintf(stderr,"UI alias cycle=%u value=%s\n",cycle,lv_label_get_text(s3_room.members[1]));
        assert(!strcmp(lv_label_get_text(s3_room.members[1]),"客厅的小钛"));
        assert(!strcmp(lv_label_get_text(s3_room.members[0]),"书房"));
        assert(!lv_obj_has_flag(s3_room.self[0],LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(s3_room.leave,0)),"退出房间"));
        model.self_name[0]=0;s3_room.next_refresh=0;s3_room_refresh(1500,&product);
        assert(!strcmp(lv_label_get_text(s3_room.members[0]),"self-device-123"));
        assert(!strcmp(lv_label_get_text(s3_room.members[2]),model.members[2].device_id));
        lv_event_send(s3_room.ptt,LV_EVENT_PRESSED,NULL);assert(s3_room.held_generation==7);
        model.phase=STARTER_ROOM_SPEAKING;s3_room.next_refresh=0;s3_room_refresh(2000,&product);
        lv_obj_update_layout(lv_scr_act());
        assert(!strcmp(lv_label_get_text(s3_room.status[0]),"讲话中"));
        if(!cycle)screenshot(out,"05-speaking");
        lv_event_send(s3_room.ptt,LV_EVENT_PRESS_LOST,NULL);assert(!s3_room.held_generation);
        lv_event_send(s3_room.ptt,LV_EVENT_PRESSED,NULL);s3_room_action(S3_ACTION_ROOM_BACK);
        assert(!s3_room.held_generation&&s_page==PAGE_MENU);
        assert(lv_mem_test()==LV_RES_OK);
    }
    assert(pressed==60&&released==60);
    assert(opened==30&&closed==30);
    puts("PASS: production LVGL 320x240 bounds, 32 KiB pool, 4x3 keypad/editing/focus, aliases, PTT lost-touch/page teardown x30");
    return 0;
}
'''
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args = parser.parse_args()
if args.output:
    args.output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="s3-room-ui-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(code)
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
