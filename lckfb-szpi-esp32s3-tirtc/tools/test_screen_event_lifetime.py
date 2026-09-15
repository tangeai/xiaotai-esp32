#!/usr/bin/env python3
"""Replay actual page setup and LVGL add-event logic past the 6-bit limit."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
product=(root/"components/starter_product/src/starter_product.c").read_text()
lvroot=root/"managed_components/lvgl__lvgl/src"
events=(lvroot/"core/lv_event.c").read_text()
obj=(lvroot/"core/lv_obj.h").read_text()
count=re.search(r"uint8_t event_dsc_cnt\s*:\s*\d+;",obj).group()
start=events.index("struct _lv_event_dsc_t * lv_obj_add_event_cb(")
add=events[start:events.index("\n}",start)+2]
start=product.index("static void render_page(void)\n{")
page=product[start:product.index("    s_clock = NULL;",start)]+"}\n"
start=product.index("    if (!lvgl_port_lock(1000))")
startup=product[start:product.index("    (void)lv_timer_create",start)]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define LV_ASSERT_OBJ(...) ((void)0)
#define LV_ASSERT_MALLOC(x) assert(x)
#define LV_EVENT_ALL 0
#define LV_OBJ_FLAG_SCROLLABLE 0
#define HOME_BACKGROUND_COLOR 0
#define ESP_ERR_TIMEOUT -1
typedef void (*lv_event_cb_t)(void *);
typedef int lv_event_code_t;
typedef struct _lv_event_dsc_t {lv_event_cb_t cb; int filter; void *user_data;} lv_event_dsc_t;
typedef struct {lv_event_dsc_t *event_dsc;
'''+count+r'''
} attr_t;
typedef struct {attr_t *spec_attr;} lv_obj_t;
static attr_t attr;
static lv_obj_t screen_obj={&attr};
static lv_event_dsc_t descriptors[65], zero_region[2];
static unsigned zero_allocations;
static void *lv_mem_realloc(void *old, size_t size) {
    (void)old;
    if(!size) {++zero_allocations; return &zero_region[1];}
    return descriptors;
}
static void lv_obj_allocate_spec_attr(lv_obj_t *o) {(void)o;}
static lv_obj_t *lv_scr_act(void) {return &screen_obj;}
static void lv_obj_clean(lv_obj_t *o) {(void)o;} /* Only children, not screen events. */
static void set_bg(lv_obj_t *o,int color) {(void)o;(void)color;}
static int lv_color_hex(int color) {return color;}
static void lv_obj_clear_flag(lv_obj_t *o,int flag) {(void)o;(void)flag;}
static void on_screen_event(void *e) {(void)e;}
static int lvgl_port_lock(int timeout) {(void)timeout;return 1;}
static void s3_reset_page(void) {}
static void s3_face_detach(void) {}
'''+add+page+"static int start_screen(void) {\n"+startup+"return 0;\n}\n"+r'''
int main(void) {
    assert(start_screen()==0);
    for(unsigned i=0;i<512;i++) render_page();
    assert(zero_allocations==0); /* No 64th-event zero-size realloc/underflow. */
    assert(zero_region[0].cb==NULL);
    assert(attr.event_dsc_cnt==1 && descriptors[0].cb==on_screen_event);
}
'''
with tempfile.TemporaryDirectory(prefix="screen-events-") as tmp:
    p=Path(tmp); (p/"test.c").write_text(code)
    subprocess.run(["cc","-Wall","-Wextra","-Werror",str(p/"test.c"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
print("PASS: 512 actual page setups keep one screen event; no LVGL count wrap or sentinel overwrite")
