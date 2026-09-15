#!/usr/bin/env python3
"""Exercise the production S3 router and teardown without hardware or networking."""
import re
from test_voice_reliability import ROOT, function, run

product_path = "components/starter_product/src/starter_product.c"
ui_path = "components/starter_product/src/starter_product_s3_ui.inc"
product = (ROOT / product_path).read_text()
ui = (ROOT / ui_path).read_text()
pages = re.search(r"typedef enum \{\s*PAGE_HOME_FACE.*?\} product_page_t;", product, re.S)[0]
actions = re.search(r"typedef enum \{\s*ACTION_AI.*?\} product_action_t;", product, re.S)[0]
ui_state = re.search(r"static EXT_RAM_BSS_ATTR struct \{.*?\} s3_ui;", ui, re.S)[0]
page_setup = function(product_path, "render_page")
page_pointers = re.findall(r"^    (s_\w+) = NULL;", page_setup, re.M)
code = pages + actions + r'''
#define EXT_RAM_BSS_ATTR
#define HOME_BACKGROUND_COLOR 0x141719
#define S3_TEXT 0xF1F3F4
#define LV_OBJ_FLAG_SCROLLABLE 1
#define LV_OBJ_FLAG_HIDDEN 2
typedef struct {unsigned unused;} lv_obj_t;
typedef struct {unsigned unused;} lv_style_t;
static lv_obj_t screen_object, toast_object;
static product_page_t s_page;
static unsigned teardown_stage, draws, refreshes, headers, faces, diagnostics;
static unsigned errors, toast_hides;
static product_page_t rendered_page;
static uint32_t s_wifi_signal_revision;
static int s_wifi_signal_style;
static lv_obj_t *s_wifi_signal_bars[4];
static char s_rendered_expression[16], s_diagnostics_text[1400], s_call_result[33];
static int64_t s_diagnostics_due_ms;
static const char *const s_emoji_names[]={"neutral"}, *const s_emoji_keys[]={"neutral"};
static unsigned s_preview_emoji;
static struct {lv_obj_t *keyboard,*active_input;} s3_room;
static int64_t monotonic_ms(void) {return 1234;}
static lv_obj_t *lv_scr_act(void) {return &screen_object;}
static unsigned lv_color_hex(unsigned color) {return color;}
static void set_bg(lv_obj_t *o,unsigned color) {assert(o==&screen_object);assert(color==HOME_BACKGROUND_COLOR);}
static void lv_obj_clear_flag(lv_obj_t *o,unsigned flag) {assert(o==&screen_object);assert(flag==1);}
static void lv_obj_add_flag(lv_obj_t *o,unsigned flag) {assert(o==&toast_object);assert(flag==2);toast_hides++;}
static void s3_room_navigation(void) {assert(teardown_stage==0);teardown_stage=1;}
static void s3_room_release(void) {assert(teardown_stage==1);teardown_stage=2;}
static void s3_face_detach(void) {assert(teardown_stage==2);teardown_stage=3;}
static void lv_obj_clean(lv_obj_t *o) {assert(o==&screen_object);assert(teardown_stage==3);teardown_stage=4;}
static void draw_page(lv_obj_t *o,product_page_t page) {
    assert(o==&screen_object);assert(teardown_stage==4);draws++;rendered_page=page;
}
static void s3_render_home(lv_obj_t *o) {draw_page(o,s_page);}
static void s3_header(lv_obj_t *o,const char *text,product_action_t a) {
    assert(text&&a);headers++;draw_page(o,s_page);
}
static void create_expression_face(lv_obj_t *o,int x,int y) {assert(o&&x==50&&y==48);faces++;}
static void apply_expression(const char *key,starter_ai_ui_phase_t p,uint8_t level) {
    assert(!strcmp(key,"neutral")&&p==STARTER_AI_UI_IDLE&&level==1);
}
static lv_obj_t *make_button(lv_obj_t *o,const char *t,int x,int y,int w,int h,product_action_t a) {
    assert(o&&t&&a&&x>=0&&y>=0&&x+w<=320&&y+h<=240);return o;
}
static lv_obj_t *make_label(lv_obj_t *o,const char *t,int x,int y,int w,unsigned c) {
    assert(o&&t&&x>=0&&y>=0&&x+w<=320&&c==S3_TEXT);return o;
}
static lv_obj_t *s3_scroller(lv_obj_t *o,int y,int h) {assert(o&&y+h<=240);return o;}
static void refresh_diagnostics(int64_t now) {assert(now==1234);diagnostics++;}
starter_runtime_status_t starter_runtime_status(void) {return (starter_runtime_status_t){0};}
starter_runtime_product_snapshot_t starter_runtime_product_snapshot(void) {
    return (starter_runtime_product_snapshot_t){0};
}
static void s3_refresh_ui(int64_t now,starter_runtime_status_t r,
                          const starter_runtime_product_snapshot_t *p) {
    (void)r;assert(now==1234&&p);refreshes++;
}
#undef ESP_LOGE
#define ESP_LOGE(...) (++errors)
'''
code += "static lv_obj_t " + ",".join("*" + p for p in page_pointers) + ";\n"
code += ui_state
for target, page in (("menu", "MENU"), ("settings", "SETTINGS"), ("contacts", "CONTACTS"),
                     ("contact_detail", "CONTACT_DETAIL"), ("call", "CALL"),
                     ("network", "NETWORK"), ("binding", "BINDING"), ("emojis", "EMOJIS")):
    code += f"static void s3_render_{target}(lv_obj_t *o) {{draw_page(o,PAGE_{page});}}\n"
code += "static void s3_room_render(lv_obj_t *o) {draw_page(o,PAGE_ROOM);}\n"
code += function(ui_path, "s3_reset_page")
code += function(ui_path, "s3_render_page")
code += page_setup
code += r'''
static void prepare(void) {
    teardown_stage=draws=refreshes=headers=faces=diagnostics=0;
    s3_ui.caption_panel=s3_ui.wechat_qr=&screen_object;
    s3_ui.toast=&toast_object;
    s3_ui.pending_action=ACTION_AI;
    s3_ui.pending_until=9999;
    s3_room.keyboard=s3_room.active_input=&screen_object;
}
int main(void) {
    const product_page_t pages[]={PAGE_HOME_FACE,PAGE_HOME_CLOCK,PAGE_MENU,PAGE_ROOM,
        PAGE_SETTINGS,PAGE_CONTACTS,PAGE_CONTACT_DETAIL,PAGE_CALL,PAGE_NETWORK,
        PAGE_BINDING,PAGE_EMOJIS,PAGE_EMOJI_PREVIEW,PAGE_DIAGNOSTICS,PAGE_CALL_RESULT};
    for(unsigned round=0;round<100;round++)for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);i++) {
        prepare();s_page=pages[i];render_page();
        assert(draws==1&&refreshes==1&&rendered_page==s_page&&errors==0);
        assert(headers==(s_page==PAGE_EMOJI_PREVIEW||s_page==PAGE_DIAGNOSTICS||s_page==PAGE_CALL_RESULT));
        assert(faces==(s_page==PAGE_EMOJI_PREVIEW));
        assert(diagnostics==(s_page==PAGE_DIAGNOSTICS));
        assert(s3_ui.caption_panel==NULL&&s3_ui.wechat_qr==NULL);
        assert(s3_ui.toast==&toast_object&&s3_ui.pending_action==ACTION_AI&&s3_ui.pending_until==9999);
        assert(s3_room.keyboard==NULL&&s3_room.active_input==NULL);
        assert(s_wifi_signal_revision==UINT32_MAX&&s_wifi_signal_style==-1);
    }
    assert(toast_hides==100);
    const unsigned invalid[]={2,15,255,65535};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        prepare();s_page=(product_page_t)invalid[i];render_page();
        assert(errors==i+1&&s_page==PAGE_HOME_FACE&&rendered_page==PAGE_HOME_FACE);
        assert(draws==1&&refreshes==1);
    }
}
'''
run("S3-only 14 page routes x100, teardown order, pending/toast ownership, invalid-page diagnostics", code)
