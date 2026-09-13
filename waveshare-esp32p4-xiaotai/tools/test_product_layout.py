#!/usr/bin/env python3
"""Test actual logical-to-physical LVGL boundary functions without a board."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/starter_product/src/starter_product.c").read_text(encoding="utf-8")
background = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/starter_product/src/starter_product_p4_background.inc").read_text(encoding="utf-8")
start = source.index("static lv_coord_t product_x(")
end = source.index("#define lv_obj_set_pos product_set_pos", start)
body = r'''
#include <assert.h>
#include <stdint.h>
typedef int16_t lv_coord_t;
typedef struct {int x,y,w,h;} lv_obj_t;
#define LCD_H_RES 480
#define LCD_V_RES 320
static void lv_obj_set_pos(lv_obj_t *o,int x,int y) {o->x=x;o->y=y;}
static void lv_obj_set_size(lv_obj_t *o,int w,int h) {o->w=w;o->h=h;}
static void lv_obj_set_width(lv_obj_t *o,int w) {o->w=w;}
static void lv_obj_set_height(lv_obj_t *o,int h) {o->h=h;}
'''
body += source[start:end]
body += background[background.index("static uint8_t p4_home_background_weight("):
                   background.index("static void p4_home_background_reset(")]
body += r'''
int main(void) {
    lv_obj_t o={0};
    assert(product_x(320)==480 && product_y(240)==320);
    assert(product_x(160)==240 && product_y(120)==160);
    product_set_pos(&o,110,194); product_set_size(&o,100,36);
    assert(o.x+o.w/2==240 && o.y+o.h<=320); /* centered back button */
    product_set_pos(&o,10,40); product_set_size(&o,300,188);
    assert(o.x==15 && o.w==450 && o.y+o.h<=320); /* AI history */
    product_set_pos(&o,170,125); product_set_size(&o,132,48);
    assert(o.x==255 && o.x+o.w==453 && o.y+o.h<=320); /* video button */
    product_set_width(&o,304); product_set_height(&o,45);
    assert(o.w==456 && o.h==60);
    int zoom=256*320/240;
    int visible=220*zoom/256;
    int face_left=product_x(50)+(product_x(220)-visible)/2;
    assert(face_left+visible/2>=239 && face_left+visible/2<=240);
    int face_top=product_y((240-108)/2);
    int face_height=108*zoom/256;
    assert(face_top+face_height/2>=159 && face_top+face_height/2<=160);
    int caption_height=2*20+4; /* ui_font_cn_18 line height + line spacing */
    int caption_bottom=LCD_V_RES-product_y(6);
    int caption_top=caption_bottom-caption_height;
    assert(caption_height==44 && caption_bottom==312);
    assert(face_top+face_height<caption_top);
    product_set_pos(&o,282,156); product_set_size(&o,32,32);
    assert(o.w==48 && o.h==42); /* smaller visible WeChat shortcut */
    int icon_width=42, icon_height=42;
    int icon_left=o.x+(o.w-icon_width)/2, icon_top=o.y+(o.h-icon_height)/2;
    assert(icon_left>=o.x && icon_left+icon_width<=o.x+o.w);
    assert(icon_top>=o.y && icon_top+icon_height<=o.y+o.h);
    int wechat_hit_left=o.x-14, wechat_hit_top=o.y-14;
    int wechat_hit_bottom=o.y+o.h+14;
    assert(o.x+o.w+14>=LCD_H_RES);
    assert(LCD_H_RES-wechat_hit_left==71 && wechat_hit_bottom-wechat_hit_top==70);
    assert(wechat_hit_top>=product_y(94)+product_y(52)); /* below right arrow */
    assert(o.x+o.w<=LCD_H_RES && wechat_hit_bottom<caption_top);
    assert(face_left+visible<wechat_hit_left);
    assert(product_x(48)+product_x(224)<wechat_hit_left); /* face-tap boundary */
    product_set_pos(&o,280,6); product_set_size(&o,32,24);
    assert(o.w==48 && o.h==32);
    int menu_hit_left=o.x-20, menu_hit_bottom=o.y+o.h+20;
    assert(o.x+o.w+20>=LCD_H_RES && o.y-20<=0);
    assert(LCD_H_RES-menu_hit_left==80 && menu_hit_bottom==60);
    assert(menu_hit_bottom<face_top && menu_hit_bottom<product_y(94));
    assert(menu_hit_left>product_x(244)+product_x(22)); /* WiFi before menu */
    assert(product_x(200)+product_x(40)<product_x(244)); /* clock before WiFi */
    assert(product_x(80)+product_x(100)<product_x(200)); /* state before clock */
    int header_center=product_y(18);
    int header_text_top=header_center-20/2;
    assert(header_text_top==14 && header_text_top+20/2==24);
    assert(product_y(6)+product_y(24)/2==header_center); /* WiFi/menu centre */
    assert(p4_home_background_weight(240,160)==255);
    for (int y=0;y<LCD_V_RES;y++) {
        assert(p4_home_background_weight(0,y)==0);
        assert(p4_home_background_weight(LCD_H_RES-1,y)==0);
        for (int x=0;x<LCD_H_RES;x++) {
            int w=p4_home_background_weight(x,y);
            assert(w==p4_home_background_weight(LCD_H_RES-1-x,y));
            assert(w==p4_home_background_weight(x,LCD_V_RES-1-y));
            if(x>LCD_H_RES/2) assert(w<=p4_home_background_weight(x-1,y));
        }
    }
    for (int x=0;x<LCD_H_RES;x++) {
        assert(p4_home_background_weight(x,0)==0);
        assert(p4_home_background_weight(x,LCD_V_RES-1)==0);
    }
    /* The opaque canvas and its zoom sampling margin stay on exactly the
     * same centre colour as the background, without a rectangular seam. */
    for(int y=face_top-2;y<=face_top+face_height+2;y++)
        for(int x=face_left-2;x<=face_left+visible+2;x++)
            assert(p4_home_background_weight(x,y)==255);
    for (int lines=1; lines<=20; ++lines) {
        int text_height=lines*20+(lines-1)*4;
        int text_top=caption_bottom-text_height;
        assert(text_top+(lines-1)*24==caption_bottom-20);
        if (lines>=2) assert(text_top+(lines-2)*24==caption_top);
        if (lines>=3) assert(text_top+(lines-3)*24+20<=caption_top);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-layout-") as directory:
    test = Path(directory)
    (test / "test.c").write_text(body, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(test / "test.c"), "-o", str(test / "test")], check=True)
    subprocess.run([str(test / "test")], check=True)
assert '"视频通话", 330' not in source
assert "lv_obj_set_pos(s_wifi_signal, LCD_H_RES" not in source
video_tick = source[source.index("static void product_video_tick("):source.index("static void product_tick(")]
assert "p4_video_ui_tick(lv_scr_act(), s_page == PAGE_CALL)" in video_tick
assert source.count("p4_video_ui_tick(") == 1
assert "lv_timer_create(product_tick, 100, NULL)" in source
assert "lv_timer_create(product_video_tick, 10, NULL)" in source
ui = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/starter_product/src/starter_product_s3_ui.inc").read_text(encoding="utf-8")
home = ui[ui.index("static void s3_render_home("):ui.index("static void s3_render_menu(")]
draw = ui[ui.index("static void s3_draw_wechat("):ui.index("static lv_obj_t *s3_scroller(")]
refresh = ui[ui.index("static void s3_refresh_ui(int64_t now,"):]
assert "create_expression_face(screen, (320 - FACE_CANVAS_W) / 2," in home
assert "(240 - FACE_CANVAS_H) / 2" in home
assert 'make_button(screen, "", 276, 150, 44, 44,' in home
assert '#if CONFIG_IDF_TARGET_ESP32P4\n    s3_ui.wechat_button = make_button(screen, "", 282, 156, 32, 32,' in home
assert 'lv_obj_set_ext_click_area(s3_ui.wechat_button, 14);' in home
assert "lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, 0);" in home
assert "lv_obj_set_style_height(s3_ui.caption_panel, caption_height, 0);" in home
assert "2 * lv_font_get_line_height(" in home
assert "lv_obj_get_style_text_line_space(s_subtitle, LV_PART_MAIN)" in home
assert "LV_SCROLLBAR_MODE_OFF" in home
assert "LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |" in home
# S3 now shares the home behavior; retain P4's physical bottom inset.
assert "const lv_point_t eyes[]" not in draw
assert "&p4_phone_shortcut_image" in draw
assert "lv_obj_init_draw_img_dsc(button, LV_PART_MAIN, &d);" in draw
assert "lv_obj_set_style_bg_opa(s3_ui.wechat_button, LV_OPA_TRANSP, 0);" in home
assert "lv_obj_set_style_img_opa(s3_ui.wechat_button, LV_OPA_80, LV_STATE_PRESSED);" in home
assert '#if CONFIG_IDF_TARGET_ESP32P4\n#include "starter_product_p4_phone.inc"' in ui
assert 's3_ui.ai_action' not in ui
assert 'S3_ACTION_AI_STOP' not in home
assert 'S3_ACTION_AI_TOGGLE' not in home
# Shared source keeps separate coordinates for the two physical displays.
assert 'lv_obj_set_pos(s_wifi_signal, 238, 7);' in home
assert '#if CONFIG_IDF_TARGET_ESP32P4\n    lv_obj_set_pos(s_wifi_signal, 244, 6);' in home
assert 'lv_obj_set_pos(s_clock, 200, 8);' in home
assert 'lv_obj_set_width(s_clock, 40);' in home
assert 'lv_obj_set_style_text_color(s_clock, lv_color_hex(S3_ACCENT), 0);' in home
assert 'lv_obj_t *header_labels[] = {s_header_title, s_state, s_clock};' in home
assert 'lv_obj_set_style_y(header_labels[i], product_y(18) - line_height / 2, 0);' in home
assert '#if CONFIG_IDF_TARGET_ESP32P4\n    lv_obj_move_foreground(s_clock);' in home
assert 'fill.bg_color = lv_color_white();' in draw
assert 'fill.bg_opa = d.opa;' in draw
assert 'fill.radius = LV_RADIUS_CIRCLE;' in draw
assert draw.index('lv_draw_rect(lv_event_get_draw_ctx(event), &fill, &backing);') < draw.index('lv_draw_img(lv_event_get_draw_ctx(event), &d, &area, icon);')
assert 'LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET' in home
assert '280, 6, 32, ACTION_MENU);' in home
assert 'lv_obj_set_height(more, 24);' in home
assert 'lv_obj_set_ext_click_area(more, 20);' in home
assert 'lv_obj_set_style_bg_opa(more, LV_OPA_20, 0);' in home
assert 'lv_obj_set_style_bg_opa(more, LV_OPA_30, LV_STATE_PRESSED);' in home
assert "LV_STATE_SCROLLED" not in refresh
assert "lv_obj_scroll_to_y(s3_ui.caption_panel" not in refresh
assert "const lv_coord_t bottom_inset = product_y(6);" in home
assert "lv_obj_add_event_cb(tap, on_home_tap, LV_EVENT_CLICKED, NULL);" in home
assert "create_expression_face(screen, 50, 32);" not in home
assert '#if CONFIG_IDF_TARGET_ESP32P4\n    p4_home_background_show(screen);' in home
assert '#if CONFIG_IDF_TARGET_ESP32P4 && PRODUCT_MODERN_UI\n    p4_home_background_reset(screen);' in source
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in background
assert 'if (s_p4_home_background.pixels == NULL)' in background
assert 'lv_timer_create' not in background and 'xTaskCreate' not in background
face = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/starter_product/src/starter_product_s3_face.inc").read_text(encoding="utf-8")
assert 'const lv_color_t background = product_face_background_color();' in face
assert 'lv_color_mix(lv_color_hex(rgb), product_face_background_color(),' in face
print("PASS: P4 geometry, two-line caption tail, home controls and video cadence")
