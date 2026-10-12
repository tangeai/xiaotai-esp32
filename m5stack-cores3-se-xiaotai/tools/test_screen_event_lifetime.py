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
start=product.index("static void core_display_round(")
rounder=product[start:product.index("\n}",start)+2]
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
#define ESP_OK 0
typedef int esp_err_t;
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
static unsigned unlocks;
static void lvgl_port_unlock(void) {++unlocks;}
static int s_lcd_io, s_display_completion_storage, s_display_completion;
static unsigned dma_barriers, callback_registrations;
static int barrier_error, registration_error;
static int esp_lcd_panel_io_tx_param(int io, int cmd, const void *param, size_t size) {
    assert(io==s_lcd_io && cmd==-1 && !param && !size);
    ++dma_barriers; return barrier_error;
}
static int xSemaphoreCreateBinaryStatic(int *storage) {
    assert(storage==&s_display_completion_storage && dma_barriers==1);
    return 3;
}
static void core_display_complete(void) {}
typedef struct {void (*on_color_trans_done)(void);} esp_lcd_panel_io_callbacks_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef struct {void (*draw_img_decoded)(void);} lv_draw_ctx_t;
static lv_draw_ctx_t draw_context;
typedef struct driver_t driver_t;
struct driver_t {void (*wait_cb)(void *);void (*rounder_cb)(driver_t *,lv_area_t *);void (*flush_cb)(void);lv_draw_ctx_t *draw_ctx;};
typedef driver_t lv_disp_drv_t;
static driver_t driver;
static struct {driver_t *driver;} display={&driver}, *s_display=&display;
static int esp_lcd_panel_io_register_event_callbacks(int io,
    const esp_lcd_panel_io_callbacks_t *callbacks, void *ctx) {
    assert(io==s_lcd_io && callbacks->on_color_trans_done==core_display_complete);
    assert(ctx==s_display->driver && dma_barriers==1 && s_display_completion==3);
    ++callback_registrations; return registration_error;
}
static void (*s_display_flush)(void);
static void (*s_display_draw_image)(void);
static void port_draw_image(void) {}
static void core_display_draw_image(void) {}
static void port_flush(void) {}
static void core_display_flush(void) {}
static void core_display_wait(void *unused) {(void)unused;}
typedef struct {void (*read_cb)(void);} touch_drv_t;
static touch_drv_t touch_driver;
static struct {touch_drv_t *driver;} touch_device={&touch_driver}, *touch_input=&touch_device;
static void port_touch_read(void) {}
static void core_touch_read(void) {}
static void (*s_touch_read)(void);
static void s3_reset_page(void) {}
static void s3_face_detach(void) {}
'''+add+page+rounder+"static int start_screen(void) {\n"+startup+"return 0;\n}\n"+r'''
int main(void) {
    driver.flush_cb=port_flush;
    touch_driver.read_cb=port_touch_read;
    driver.draw_ctx=&draw_context;
    draw_context.draw_img_decoded=port_draw_image;
    barrier_error=-2;
    assert(start_screen()==-2);
    assert(dma_barriers==1 && !callback_registrations && !s_display_completion);
    assert(driver.flush_cb==port_flush && unlocks==1 && !attr.event_dsc_cnt);
    assert(touch_driver.read_cb==port_touch_read && !s_touch_read);
    dma_barriers=0; barrier_error=0; registration_error=-3;
    assert(start_screen()==-3);
    assert(dma_barriers==1 && callback_registrations==1 && unlocks==2);
    assert(driver.flush_cb==port_flush && !attr.event_dsc_cnt);
    assert(touch_driver.read_cb==port_touch_read && !s_touch_read);
    dma_barriers=callback_registrations=0; registration_error=0;
    assert(start_screen()==0);
    assert(dma_barriers==1 && callback_registrations==1);
    assert(driver.flush_cb==core_display_flush && s_display_flush==port_flush);
    assert(driver.wait_cb==core_display_wait);
    assert(driver.rounder_cb==core_display_round);
    assert(draw_context.draw_img_decoded==core_display_draw_image && s_display_draw_image==port_draw_image);
    /* The dark-screen wake filter wraps the port's own touch reader once. */
    assert(touch_driver.read_cb==core_touch_read && s_touch_read==port_touch_read);
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
print("PASS: 512 actual page setups keep one screen event; no LVGL count wrap or sentinel overwrite; touch wake filter installed only on success")
