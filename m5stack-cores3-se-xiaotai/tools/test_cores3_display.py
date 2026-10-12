"""Exercise the actual port's single-buffer SPI completion and product wait hook."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from test_voice_reliability import function

root = Path(__file__).resolve().parents[1]
product = "components/starter_product/src/starter_product.c"
port = "managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c"
config = (root / product).read_text(encoding="utf-8")
assert ".buffer_size = LCD_H_RES * LCD_DRAW_LINES" in config
assert ".trans_size = 0" in config
assert ".flags = {.buff_dma = true, .buff_spiram = false}" in config
assert "s_display->driver->wait_cb = core_display_wait" in config
assert "s_display->driver->rounder_cb = core_display_round" in config
assert "s_display->driver->flush_cb = core_display_flush" in config
assert "static EXT_RAM_BSS_ATTR starter_product_display_stats_t s_display_stats" in config

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE 1
#define LV_DRAW_COMPLEX 1
#define LV_OPA_COVER 255
#define LV_OPA_TRANSP 0
#define LV_IMG_ZOOM_NONE 256
#define LV_BLEND_MODE_NORMAL 0
#define LV_IMG_CF_TRUE_COLOR 0
typedef int BaseType_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef struct { uint16_t full; } lv_color_t;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef struct { const lv_area_t *clip_area, *buf_area; void *buf; } lv_draw_ctx_t;
typedef struct { unsigned angle, zoom, opa, recolor_opa, blend_mode; } lv_draw_img_dsc_t;
typedef unsigned lv_img_cf_t;
static bool image_mask;
static bool lv_draw_mask_is_any(const lv_area_t *area) { (void)area; return image_mask; }
static bool _lv_area_intersect(lv_area_t *out, const lv_area_t *a, const lv_area_t *b) {
    *out = (lv_area_t){a->x1>b->x1?a->x1:b->x1, a->y1>b->y1?a->y1:b->y1,
                       a->x2<b->x2?a->x2:b->x2, a->y2<b->y2?a->y2:b->y2};
    return out->x1<=out->x2 && out->y1<=out->y2;
}
static unsigned lv_area_get_size(const lv_area_t *area) {
    return (unsigned)((area->x2-area->x1+1)*(area->y2-area->y1+1));
}
static int lv_area_get_width(const lv_area_t *area) { return area->x2-area->x1+1; }
static int lv_area_get_height(const lv_area_t *area) { return area->y2-area->y1+1; }
static lv_color_t face_pixels[220*108];
static lv_color_t *s_face_canvas_buffer = face_pixels;
static void (*s_display_draw_image)(lv_draw_ctx_t *, const lv_draw_img_dsc_t *,
                                    const lv_area_t *, const uint8_t *, lv_img_cf_t);
typedef struct { bool flushing; } draw_buffer_t;
typedef struct {
    void *user_data;
    draw_buffer_t *draw_buf;
    bool direct_mode, full_refresh;
} lv_disp_drv_t;
typedef struct {
    uint32_t trans_size;
    int disp_type;
    void *trans_sem, *panel_handle;
    lv_color_t *trans_buf;
} lvgl_port_display_ctx_t;
#define LVGL_PORT_DISP_TYPE_RGB 1U
#define LVGL_PORT_DISP_TYPE_DSI 2U
#define portMAX_DELAY UINT32_MAX
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static uint16_t panel[240][320];
static lv_disp_drv_t *active;
static const lv_color_t *pending;
static unsigned waits, submitted, completed, remaining;
static bool reference_mode;
static unsigned completion_tokens;
static unsigned core_tokens, core_blocked, irq_yields;
static bool complete_before_wait, complete_at_take, waiting_core;
static void *s_display_completion = (void *)3;
#define portYIELD_FROM_ISR() (++irq_yields)
static lv_color_t staging[3200];
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static unsigned image_calls;
static lv_draw_ctx_t *expected_ctx;
static const lv_draw_img_dsc_t *expected_dsc;
static const lv_area_t *expected_coords;
static const uint8_t *expected_image;
static lv_img_cf_t expected_format;
static void draw_image(lv_draw_ctx_t *ctx, const lv_draw_img_dsc_t *dsc,
                       const lv_area_t *coords, const uint8_t *image, lv_img_cf_t format) {
    assert(ctx==expected_ctx && dsc==expected_dsc && coords==expected_coords);
    assert(image==expected_image && format==expected_format);
    ++image_calls; now_us+=13;
}
static unsigned materialize_calls, direct_calls;
static bool fast_face_mode;
static void s3_face_materialize(void) { ++materialize_calls; }
static bool s3_face_draw_surface(lv_draw_ctx_t *ctx, const lv_area_t *coords) {
    assert(ctx==expected_ctx && coords==expected_coords);
    if(!fast_face_mode || !ctx->buf || !ctx->buf_area) return false;
    ++direct_calls; now_us+=13; return true;
}
static void (*s_display_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static lv_area_t pending_area;
static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *, void *);
static bool core_display_complete(esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *, void *);
static void lv_disp_flush_ready(lv_disp_drv_t *driver) {
    assert(driver == active && !pending);
    assert(reference_mode || driver->draw_buf->flushing);
    driver->draw_buf->flushing = false; ++completed;
}
static bool lv_disp_flush_is_last(lv_disp_drv_t *driver) { (void)driver; return true; }
static int xSemaphoreGiveFromISR(void *sem, BaseType_t *awaken) {
    if(sem==s_display_completion) {
        assert(!reference_mode);
        *awaken=waiting_core;
        if(core_tokens) return pdFALSE;
        core_tokens=1; return pdTRUE;
    }
    assert(reference_mode && sem==(void *)2 && !completion_tokens);
    ++completion_tokens; *awaken=1; return 1;
}
static void vTaskDelay(unsigned ticks);
static int xSemaphoreTake(void *sem, unsigned timeout) {
    if(sem==s_display_completion) {
        assert(timeout==0 || timeout==portMAX_DELAY);
        if(timeout==0) {
            if(!core_tokens) return pdFALSE;
            core_tokens=0; return pdTRUE;
        }
        if(complete_at_take) {
            complete_at_take=false;
            while(pending) vTaskDelay(1);
        }
        if(!core_tokens) {
            ++core_blocked; waiting_core=true;
            while(!core_tokens) vTaskDelay(1);
            waiting_core=false;
        }
        core_tokens=0; return pdTRUE;
    }
    assert(reference_mode && sem==(void *)2 && timeout==portMAX_DELAY);
    while(!completion_tokens) vTaskDelay(1);
    --completion_tokens; return 1;
}
static int esp_lcd_panel_draw_bitmap(void *handle, int x1, int y1, int x2, int y2,
                                    const lv_color_t *pixels) {
    assert(handle == (void *)1 && !pending);
    assert(reference_mode || active->draw_buf->flushing);
    assert(!reference_mode || pixels==staging);
    assert(x1 >= 0 && x1 < x2 && x2 <= 320 && y1 >= 0 && y1 < y2 && y2 <= 240);
    assert((unsigned)((x2-x1)*(y2-y1)) <= 3200U);
    pending = pixels; pending_area = (lv_area_t){x1,y1,x2-1,y2-1};
    remaining = 3; ++submitted;
    if(complete_before_wait) while(pending) vTaskDelay(1);
    return 0;
}
static void vTaskDelay(unsigned ticks) {
    assert(ticks == 1 && pending);
    assert(reference_mode || active->draw_buf->flushing); ++waits;
    now_us += 1000;
    if (--remaining) return;
    const lv_color_t *pixels = pending;
    for (int y=pending_area.y1; y<=pending_area.y2; ++y)
        for (int x=pending_area.x1; x<=pending_area.x2; ++x)
            panel[y][x] = (pixels++)->full;
    pending = NULL;
    if(reference_mode) assert(!lvgl_port_flush_io_ready_callback(NULL,NULL,active));
    else assert(core_display_complete(NULL,NULL,active)==waiting_core);
}
'''
test = r'''
int main(void) {
    unsigned rounded_cases=0;
    for (int x1=0; x1<320; ++x1) for (int x2=x1; x2<320; ++x2) {
        lv_area_t area={x1,17,x2,21};
        core_display_round(NULL,&area);
        assert(area.x1>=0 && area.x2<320 && area.x1<=x1 && area.x2>=x2);
        assert(x1-area.x1<=1 && area.x2-x2<=1);
        assert(!(area.x1&1) && (area.x2&1) && area.y1==17 && area.y2==21);
        lv_area_t once=area;
        core_display_round(NULL,&area);
        assert(!memcmp(&area,&once,sizeof(area)));
        ++rounded_cases;
    }
    assert(rounded_cases==51360);
    puts("PASS: all 51360 screen X intervals remain covered, bounded, idempotent and word-aligned, unchanged Y");
    s_display_draw_image=draw_image;
    lv_area_t canvas={50,40,269,147}, clip={60,45,79,54};
    lv_draw_ctx_t image_ctx={.clip_area=&clip};
    lv_draw_img_dsc_t image_dsc={.zoom=256,.opa=255};
    expected_ctx=&image_ctx; expected_dsc=&image_dsc; expected_coords=&canvas;
    expected_image=(const uint8_t *)face_pixels; expected_format=LV_IMG_CF_TRUE_COLOR;
    for (unsigned n=0; n<2400; ++n)
        core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
    assert(image_calls==2400 && s_display_stats.face_blits==2400);
    assert(s_display_stats.face_blit_pixels==2400*200 && s_display_stats.face_blit_us==2400*13);
    assert(s_display_stats.face_blit_flags==0);
    expected_image=(const uint8_t *)panel;
    core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
    assert(image_calls==2401 && s_display_stats.face_blits==2400);
    expected_image=(const uint8_t *)face_pixels; expected_format=1;
    image_mask=true; image_dsc.angle=1; image_dsc.opa=128;
    image_dsc.recolor_opa=128; image_dsc.blend_mode=1;
    s_display_stats.face_blit_us=UINT32_MAX;
    s_display_stats.face_blit_pixels=UINT32_MAX;
    core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
    assert(s_display_stats.face_blit_flags==63);
    assert(s_display_stats.face_blit_us==(uint64_t)UINT32_MAX+13);
    assert(s_display_stats.face_blit_pixels==(uint64_t)UINT32_MAX+200);
    image_mask=false; image_dsc=(lv_draw_img_dsc_t){.zoom=256,.opa=255};
    expected_format=LV_IMG_CF_TRUE_COLOR;
    for (unsigned n=0; n<3000; ++n) {
        unsigned width=1+n%220, height=1+n%14;
        int x=50+(int)(n*19%(221-width)), y=40+(int)(n*23%(109-height));
        clip=(lv_area_t){x,y,x+(int)width-1,y+(int)height-1};
        lv_area_t buffer_area=clip;
        image_ctx.buf_area=&buffer_area; image_ctx.buf=panel;
        unsigned byte_rows=0;
        const unsigned source=(unsigned)((y-40)*220+x-50);
        for (unsigned row=0; row<height; ++row)
            if ((((uintptr_t)face_pixels+(source+row*220)*2) ^
                 ((uintptr_t)panel+row*width*2)) & 3U) ++byte_rows;
        uint64_t before=s_display_stats.face_byte_pixels;
        uint64_t before_rows=s_display_stats.face_byte_rows;
        core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
        assert(s_display_stats.face_byte_rows-before_rows==byte_rows);
        assert(s_display_stats.face_byte_pixels-before==(uint64_t)byte_rows*width);
        core_display_round(NULL,&buffer_area);
        before=s_display_stats.face_byte_pixels;
        before_rows=s_display_stats.face_byte_rows;
        core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
        assert(s_display_stats.face_byte_rows==before_rows);
        assert(s_display_stats.face_byte_pixels==before);
    }
    image_ctx.buf_area=NULL; image_ctx.buf=NULL;
    clip=(lv_area_t){0,0,10,10};
    uint64_t invisible_before=s_display_stats.face_blit_pixels;
    core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
    assert(s_display_stats.face_blit_pixels==invisible_before);
    clip=(lv_area_t){60,45,79,54};
    lv_area_t direct_buffer=clip;
    image_ctx.buf_area=&direct_buffer; image_ctx.buf=panel;
    fast_face_mode=true;
    unsigned image_before=image_calls, fallback_before=materialize_calls;
    uint64_t byte_rows_before=s_display_stats.face_byte_rows;
    uint64_t byte_pixels_before=s_display_stats.face_byte_pixels;
    core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
    assert(direct_calls==1 && image_calls==image_before && materialize_calls==fallback_before);
    assert(s_display_stats.face_byte_rows==byte_rows_before);
    assert(s_display_stats.face_byte_pixels==byte_pixels_before);
    for(unsigned n=0;n<7;++n) {
        image_dsc=(lv_draw_img_dsc_t){.zoom=256,.opa=255};
        image_mask=(n==0); expected_format=LV_IMG_CF_TRUE_COLOR;
        if(n==1) image_dsc.angle=1;
        if(n==2) image_dsc.zoom=257;
        if(n==3) image_dsc.opa=128;
        if(n==4) image_dsc.recolor_opa=128;
        if(n==5) image_dsc.blend_mode=1;
        if(n==6) expected_format=1;
        core_display_draw_image(&image_ctx,&image_dsc,&canvas,expected_image,expected_format);
        assert(image_calls==image_before+n+1 && materialize_calls==fallback_before+n+1 && direct_calls==1);
    }
    image_mask=false; fast_face_mode=false;
    puts("PASS: canvas draw probe preserves arguments, isolates other images, clips pixels, records all paths and 64-bit time");
    puts("PASS: direct face drawing and seven standard LVGL fallback conditions, no duplicate draw");
    lv_color_t buffer[3200];
    draw_buffer_t draw = {0};
    lvgl_port_display_ctx_t context = {.panel_handle=(void *)1};
    lv_disp_drv_t driver = {.user_data=&context, .draw_buf=&draw};
    active = &driver;
    s_display_flush = lvgl_port_flush_callback;
    memset(panel,0x5a,sizeof(panel));
    for (unsigned n=0; n<1200; ++n) {
        unsigned width = 1+n%320;
        unsigned height = 3200/width;
        if (height > 240) height = 240;
        int x=(int)(n*19%(321-width)), y=(int)(n*23%(241-height));
        lv_area_t area = {x,y,x+(int)width-1,y+(int)height-1};
        for (unsigned i=0; i<width*height; ++i) buffer[i].full=(uint16_t)(n^i);
        draw.flushing = true;
        core_display_flush(&driver,&area,buffer);
        assert(pending==buffer && draw.flushing && completed==n);
        core_display_wait(&driver);
        assert(!pending && !draw.flushing && completed==n+1 && submitted==n+1);
        unsigned before=waits; core_display_wait(&driver); assert(waits==before);
        core_display_monitor(&driver, 3, width*height);
        for (unsigned row=0; row<height; ++row)
            for (unsigned column=0; column<width; ++column)
                assert(panel[y+(int)row][x+(int)column] == buffer[row*width+column].full);
    }
    assert(waits==3600 && submitted==completed);
    assert(s_display_stats.refreshes==1200 && s_display_stats.refresh_ms==3600);
    assert(s_display_stats.flushes==1200 && s_display_stats.waits==1200);
    assert(s_display_stats.wait_us==3600000 && s_display_stats.flush_bytes==2*s_display_stats.pixels);
    s_display_stats.flush_bytes=UINT32_MAX;
    draw.flushing=true;
    lv_area_t area={0,0,0,0};
    core_display_flush(&driver,&area,buffer);
    assert(s_display_stats.flush_bytes==(uint64_t)UINT32_MAX+2);
    while(draw.flushing) core_display_wait(&driver);
    reference_mode=true;
    context.trans_size=3200; context.trans_buf=staging; context.trans_sem=(void *)2;
    static lv_color_t reference_pixels[320*240];
    uint64_t expected_bytes=s_display_stats.flush_bytes;
    for(unsigned n=0; n<1200; ++n) {
        unsigned width=1+n%320, height=1+n%240;
        int x=(int)(n*19%(321-width)), y=(int)(n*23%(241-height));
        lv_area_t area={x,y,x+(int)width-1,y+(int)height-1};
        for(unsigned i=0; i<width*height; ++i) reference_pixels[i].full=(uint16_t)(n^i);
        unsigned chunks=(height+(3200/width)-1)/(3200/width);
        unsigned before=submitted, before_done=completed;
        draw.flushing=true;
        core_display_flush(&driver,&area,reference_pixels);
        assert(!pending && !draw.flushing && !completion_tokens);
        assert(submitted==before+chunks && completed==before_done+chunks);
        expected_bytes+=width*height*2;
        assert(s_display_stats.flush_bytes==expected_bytes);
        for(unsigned row=0; row<height; ++row)
            for(unsigned column=0; column<width; ++column)
                assert(panel[y+(int)row][x+(int)column]==reference_pixels[row*width+column].full);
    }
    assert(s_display_stats.flushes==2401 && submitted==completed);
    puts("PASS: 1200 partial SPI areas, exact pixel order, <=6400-byte DMA, single IO completion, no early reuse or busy spin");
    puts("PASS: 1200 PSRAM regions split into <=6400-byte DMA chunks, ordered copies, exact pixels and completion-before-reuse");
    reference_mode=false;
    context.trans_size=0; context.trans_sem=NULL;
    unsigned initial_blocked=core_blocked, initial_yields=irq_yields;
    for(unsigned n=0; n<900; ++n) {
        /* Early IRQ, completion in the check-to-block gap, and delayed IRQ. */
        complete_before_wait=(n%3==0);
        complete_at_take=(n%3==1);
        draw.flushing=true;
        core_display_flush(&driver,&area,buffer);
        if(complete_before_wait) {
            assert(!pending && !draw.flushing && core_tokens==1);
            core_display_wait(&driver);
            assert(core_tokens==1);
        } else {
            assert(pending && draw.flushing && !core_tokens);
            core_display_wait(&driver);
            assert(!pending && !draw.flushing && !core_tokens);
        }
    }
    assert(core_blocked-initial_blocked==300 && irq_yields-initial_yields==300);
    complete_before_wait=complete_at_take=false;
    draw.flushing=true;
    core_display_flush(&driver,&area,buffer);
    /* A previous IRQ may give its token just after the next flush drained it.
     * LVGL still tests flushing, so that token cannot release this DMA early. */
    core_tokens=1;
    core_display_wait(&driver);
    assert(pending && draw.flushing && !core_tokens);
    core_display_wait(&driver);
    assert(!pending && !draw.flushing && !core_tokens);
    assert(submitted==completed);
    puts("PASS: 900 early/check-to-block/delayed IRQs and late stale token preserve single-buffer ownership; ISR yields only for blocked waiter");
}
'''
header = (root / "components/starter_product/include/starter_product.h").read_text(encoding="utf-8")
stats = re.search(r"typedef struct \{[^}]*\} starter_product_display_stats_t;", header).group()
code = prefix + stats + "\nstatic starter_product_display_stats_t s_display_stats;\n"
code += function(port, "lvgl_port_flush_io_ready_callback") + function(port, "lvgl_port_flush_callback")
code += function(product, "core_display_complete")
code += function(product, "core_display_flush") + function(product, "core_display_monitor")
code += function(product, "core_display_round")
code += function(product, "core_display_wait") + function(product, "core_display_draw_image")
release_test = r'''
int main(void) {
    lv_color_t buffer[3200];
    draw_buffer_t draw={0};
    lvgl_port_display_ctx_t context={.panel_handle=(void *)1};
    lv_disp_drv_t driver={.user_data=&context,.draw_buf=&draw};
    active=&driver; s_display_flush=lvgl_port_flush_callback;
    lv_area_t area={0,0,319,9};
    for(unsigned n=0;n<1200;++n) {
        complete_before_wait=(n%3==0); complete_at_take=(n%3==1);
        for(unsigned i=0;i<3200;++i) buffer[i].full=(uint16_t)(n^i);
        draw.flushing=true;
        core_display_flush(&driver,&area,buffer);
        while(draw.flushing) core_display_wait(&driver);
        assert(!pending && completed==n+1);
        for(unsigned y=0;y<10;++y) for(unsigned x=0;x<320;++x)
            assert(panel[y][x]==buffer[y*320+x].full);
    }
    assert(submitted==completed && !s_display_stats.flushes && !s_display_stats.waits);
    puts("PASS: console-disabled DMA notifications preserve 1200 frames and exact pixels without diagnostic state updates");
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-display-") as temporary:
    directory = Path(temporary)
    for enabled, main in ((1, test), (0, release_test)):
        source = code.replace("#define CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE 1",
                              f"#define CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE {enabled}")
        (directory / "test.c").write_text(source + main, encoding="utf-8")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare", "-fsanitize=address,undefined",
                        str(directory / "test.c"), "-o", str(directory / "test")], check=True)
        subprocess.run([str(directory / "test")], check=True)
