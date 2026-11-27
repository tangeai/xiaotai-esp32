// SPDX-License-Identifier: MIT
// Pictures of a product's screens for design review. The real product, theme
// and LVGL draw every page on the host; tools/ui_preview.py runs this scenario
// and turns the frames into PNG files. Nothing here asserts how a page looks.
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_ai.h"
#include "product_platform.h"
#include "product_screens.h"
#include "product_test.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"
#include "xiaotai_ui_preview_host.h"
#if PRODUCT_TEST_SCREENS_VIDEO
#include "xiaotai/video_backend.h"
#endif
#if PRODUCT_TEST_SCREENS_WECHAT
#include "xiaotai/modules/wechat.h"
#endif

enum { SETTLE_MS = 450, MAX_CONTROLS = 48, MAX_PAGES = 48, STEP_MS = 5, WAIT_TRIES = 1000 };
static const uintptr_t CALL_PEER = 0x5c01U;
static atomic_uint s_request;
static unsigned s_shot;
static char s_seen[MAX_PAGES][40];
static size_t s_seen_count;


static void call_http(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    static const char info[] = "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}";
    if (request->method == XIAOTAI_HTTP_POST && !strcmp(request->url, "http://fixture.invalid/call/v1/call/device/info")) {
        reply->status = 200;
        reply->body = (const uint8_t *)info;
        reply->body_size = sizeof(info) - 1;
    }
}

#if PRODUCT_TEST_SCREENS_VIDEO
/* The host has no codec: a board with an H.264 encoder shows what it offers
 * with one (it can stand at the door), so the pictures have one that never opens. */
static int h264_match(const xiaotai_video_encoder_config_t *config) { return config->codec == XIAOTAI_VIDEO_CODEC_H264; }

static xiaotai_err_t h264_open(const xiaotai_video_encoder_config_t *config, void **state)
{
    (void)config;
    *state = NULL;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t h264_encode(void *state, const xiaotai_video_image_t *image, bool key, xiaotai_video_packet_t *packet)
{
    (void)state; (void)image; (void)key; (void)packet;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static void h264_close(void *state) { (void)state; }
#endif

void product_screens_prepare(bool provisioned)
{
#if PRODUCT_TEST_SCREENS_VIDEO
    static const xiaotai_video_encoder_t h264 = {.name = "screens-h264", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
        .match = h264_match, .open = h264_open, .encode = h264_encode, .close = h264_close};
    (void)xiaotai_video_encoder_register(&h264);
#endif
    if (!provisioned) return;
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"前台\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"peer2\",\"remark\":\"会议室\",\"online\":false},"
        "{\"type\":\"voip\",\"device_id\":\"wx1\",\"remark\":\"张经理\",\"online\":true,"
        "\"wx_app_id\":\"wxfixture\",\"wx_model_id\":\"fixture\"}]}}");
    product_platform_http_responder(call_http, NULL);
}

static const char *out_dir(void)
{
    const char *dir = getenv("XIAOTAI_SCREENS_DIR");
    return dir && *dir ? dir : ".";
}

static void current_page(char *page, size_t capacity)
{
    xiaotai_ui_info_t ui;
    page[0] = '\0';
    if (xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK) snprintf(page, capacity, "%s", ui.page);
}

/* One PPM per scene plus a line in index.tsv: shot, name, page, controls. */
void product_screens_shoot(const char *name)
{
    xiaotai_delay_ms(SETTLE_MS);
    uint16_t width = 0, height = 0;
    const uint8_t *rgb = NULL;
    if (!xiaotai_ui_host_preview(0, &width, &height, &rgb)) {
        xiaotai_ui_info_t ui = {0};
        (void)xiaotai_ui_inspect(0, &ui);
        fprintf(stderr, "SCREENS no frame for %s: ready=%d %ux%u pixel=%d frames=%u page=%s\n", name, ui.ready,
                ui.width, ui.height, (int)ui.pixel, (unsigned)ui.frames, ui.page);
        return;
    }
    char path[512];
    ++s_shot;
    snprintf(path, sizeof(path), "%s/%02u-%s.ppm", out_dir(), s_shot, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    fwrite(rgb, 3, (size_t)width * height, file);
    fclose(file);
    char page[40];
    current_page(page, sizeof(page));
    xiaotai_ui_control_t controls[MAX_CONTROLS];
    size_t count = 0;
    (void)xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000);
    snprintf(path, sizeof(path), "%s/index.tsv", out_dir());
    file = fopen(path, "a");
    assert(file);
    fprintf(file, "%02u\t%s\t%s\t", s_shot, name, page);
    for (size_t i = 0; i < count && i < MAX_CONTROLS; ++i)
        fprintf(file, "%s[%s|%d,%d %ux%u]%s", i ? " " : "", controls[i].text, controls[i].x, controls[i].y,
                controls[i].width, controls[i].height, controls[i].intent);
    fprintf(file, "\n");
    fclose(file);
    printf("SCREENS %02u %s page=%s controls=%zu\n", s_shot, name, page, count);
    fflush(stdout);
}

#define shoot product_screens_shoot

static bool seen(const char *page)
{
    for (size_t i = 0; i < s_seen_count; ++i)
        if (!strcmp(s_seen[i], page)) return true;
    if (s_seen_count < MAX_PAGES) snprintf(s_seen[s_seen_count++], sizeof(s_seen[0]), "%s", page);
    return false;
}

static bool tap(const xiaotai_ui_control_t *control)
{
    const int16_t x = (int16_t)(control->x + control->width / 2);
    const int16_t y = (int16_t)(control->y + control->height / 2);
    for (unsigned i = 0; i < 100; ++i) {
        const xiaotai_err_t err = xiaotai_ui_tap(0, x, y);
        if (err == XIAOTAI_OK) {
            xiaotai_delay_ms(300);
            return true;
        }
        if (err != XIAOTAI_ERR_INVALID_STATE) return false;
        xiaotai_delay_ms(20);
    }
    return false;
}

static bool tap_intent(const char *prefix)
{
    xiaotai_ui_control_t controls[MAX_CONTROLS];
    size_t count = 0;
    if (xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000) != XIAOTAI_OK) return false;
    for (size_t i = 0; i < count && i < MAX_CONTROLS; ++i)
        if (!strncmp(controls[i].intent, prefix, strlen(prefix)) && !(controls[i].state & XIAOTAI_STATE_DISABLED))
            return tap(&controls[i]);
    return false;
}

/* A control by its exact intent (the list cuts intents to fit), its text, or
 * both; NULL matches anything. Disabled controls are not operated. */
static bool find_control(const char *intent, const char *text, xiaotai_ui_control_t *out)
{
    xiaotai_ui_control_t controls[MAX_CONTROLS];
    size_t count = 0;
    if (xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000) != XIAOTAI_OK) return false;
    for (size_t i = 0; i < count && i < MAX_CONTROLS; ++i) {
        const xiaotai_ui_control_t *c = &controls[i];
        if (c->state & XIAOTAI_STATE_DISABLED) continue;
        if (intent && strncmp(c->intent, intent, sizeof(c->intent) - 1U)) continue;
        if (text && strcmp(c->text, text)) continue;
        *out = *c;
        return true;
    }
    return false;
}

static bool tap_control(const char *intent, const char *text)
{
    xiaotai_ui_control_t control;
    return find_control(intent, text, &control) && tap(&control);
}

static bool on_page(const char *id)
{
    char page[40];
    current_page(page, sizeof(page));
    return !strcmp(page, id);
}

static bool wait_page(const char *id, unsigned ms)
{
    for (unsigned waited = 0; waited < ms; waited += 50) {
        if (on_page(id)) return true;
        xiaotai_delay_ms(50);
    }
    return on_page(id);
}

static void back_to(const char *home)
{
    for (unsigned i = 0; i < 6; ++i) {
        char page[40];
        current_page(page, sizeof(page));
        if (!strcmp(page, home)) return;
        if (!tap_intent("companion.back") && !tap_intent("ui.back")) break;
    }
}

static bool is_back(const char *intent) { return strstr(intent, ".back") != NULL; }

static void scroll_down(void)
{
    xiaotai_ui_info_t ui;
    if (xiaotai_ui_inspect(0, &ui) != XIAOTAI_OK || !ui.width) return;
    /* Near the left edge: a drag that starts on a slider or a choice does not scroll. */
    const int16_t x = (int16_t)(ui.width / 8), top = (int16_t)(ui.height / 4), bottom = (int16_t)(ui.height * 3 / 4);
    for (unsigned i = 0; i < 100 && xiaotai_ui_swipe(0, x, bottom, x, top, 300) == XIAOTAI_ERR_INVALID_STATE; ++i)
        xiaotai_delay_ms(20);
    xiaotai_delay_ms(700);
}

static void scroll_up(void)
{
    xiaotai_ui_info_t ui;
    if (xiaotai_ui_inspect(0, &ui) != XIAOTAI_OK || !ui.width) return;
    const int16_t x = (int16_t)(ui.width / 8), top = (int16_t)(ui.height / 4), bottom = (int16_t)(ui.height * 3 / 4);
    for (unsigned i = 0; i < 100 && xiaotai_ui_swipe(0, x, top, x, bottom, 300) == XIAOTAI_ERR_INVALID_STATE; ++i)
        xiaotai_delay_ms(20);
    xiaotai_delay_ms(700);
}

/* A control of a scrolled list, where it is now. A list longer than two screens does not come back to the
 * place its first scroll left it at, so the control is looked up by what it is: here, further down, and
 * then from the top. */
static bool find_scrolled(const xiaotai_ui_control_t *wanted, xiaotai_ui_control_t *out)
{
    for (unsigned i = 0; i < 3; ++i) {
        if (find_control(wanted->intent, wanted->text, out)) return true;
        scroll_down();
    }
    for (unsigned i = 0; i < 6; ++i) {
        scroll_up();
        if (find_control(wanted->intent, wanted->text, out)) return true;
    }
    return false;
}

static bool runs_past_bottom(const xiaotai_ui_control_t *controls, size_t count)
{
    xiaotai_ui_info_t ui;
    if (xiaotai_ui_inspect(0, &ui) != XIAOTAI_OK) return false;
    for (size_t i = 0; i < count; ++i)
        if (controls[i].y + controls[i].height >= ui.height - ui.height / 12) return true;
    return false;
}

/* Taps every control that leads somewhere, two levels deep, once per page;
 * a list that runs past the bottom is scrolled once and explored again. */
static void explore_at(const char *from, unsigned depth, bool scrolled);
static void explore(const char *from, unsigned depth) { explore_at(from, depth, false); }

static void explore_at(const char *from, unsigned depth, bool scrolled)
{
    xiaotai_ui_control_t controls[MAX_CONTROLS];
    size_t count = 0;
    if (scrolled) scroll_down();
    if (xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000) != XIAOTAI_OK) {
        fprintf(stderr, "SCREENS no control list on %s\n", from);
        return;
    }
    if (count > MAX_CONTROLS) count = MAX_CONTROLS;
    if (scrolled) {
        char name[64];
        snprintf(name, sizeof(name), "%s-scrolled", from);
        shoot(name);
    }
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_ui_control_t c = controls[i];
        if (!c.intent[0] || (c.state & XIAOTAI_STATE_DISABLED) || is_back(c.intent)) continue;
        if (c.element == XIAOTAI_UI_ELEMENT_FIELD) continue;   /* a field changes a value, it leads nowhere */
        /* Leave calls, sessions and destructive actions to the scripted scenes. */
        if (strstr(c.intent, "call") || strstr(c.intent, "talk") || strstr(c.intent, "reset") ||
            strstr(c.intent, "unbind") || strstr(c.intent, "reboot") || strstr(c.intent, "network.leave") ||
            strstr(c.intent, "provision") || strstr(c.intent, "binding") || strstr(c.intent, "stream") ||
            strstr(c.intent, "room") || strstr(c.intent, "wechat") || strstr(c.intent, "contact.action") ||
            strstr(c.intent, "contact.first") || strstr(c.intent, "home.start") ||
            !strcmp(c.intent, "companion.home")) continue;
        xiaotai_ui_control_t at = c;
        if (scrolled && !find_scrolled(&c, &at)) {
            fprintf(stderr, "SCREENS control gone on %s: %s\n", from, c.intent);
            continue;
        }
        if (!tap(&at)) {
            fprintf(stderr, "SCREENS tap refused on %s: %s\n", from, c.intent);
            continue;
        }
        char page[40];
        /* The page follows the tap when the owner gets to it. */
        for (unsigned waited = 0; waited < 1200; waited += 50) {
            current_page(page, sizeof(page));
            if (strcmp(page, from)) break;
            xiaotai_delay_ms(50);
        }
        if (strcmp(page, from) && !seen(page)) {
            char name[64];
            snprintf(name, sizeof(name), "%s", page);
            for (char *p = name; *p; ++p) if (*p == ' ' || *p == '/') *p = '_';
            shoot(name);
            if (depth > 1) explore(page, depth - 1);
        }
        back_to(from);
        if (scrolled) scroll_down();
    }
    /* The home screen has nothing to scroll; its controls just reach the edge. */
    if (!scrolled && strcmp(from, "home") && runs_past_bottom(controls, count)) explore_at(from, depth, true);
}

/* The home screen's own pages (face, clock and the like) behind next. */
static void home_pages(const char *home)
{
    for (unsigned n = 2; n <= 4; ++n) {
        if (!tap_control("companion.home", "next")) return;
        char page[40];
        current_page(page, sizeof(page));
        if (strcmp(page, home)) {
            back_to(home);
            return;
        }
        char name[16];
        snprintf(name, sizeof(name), "home-page-%u", n);
        shoot(name);
    }
}

static void call_connect(void *ctx, xiaotai_link_kind_t kind, uint32_t request, const char *peer, const char *token)
{
    (void)ctx; (void)kind; (void)peer; (void)token;
    atomic_store(&s_request, request);
}

static void read_call(void *ctx) { xiaotai_call_status(ctx); }

static bool call_wait(xiaotai_call_state_t wanted, xiaotai_call_status_t *out)
{
    for (unsigned i = 0; i < WAIT_TRIES; ++i) {
        product_on_owner(read_call, out);
        if (out->state == wanted) return true;
        xiaotai_delay_ms(STEP_MS);
    }
    return false;
}

static void accept_call(void *ctx) { (void)xiaotai_call_accept(*(const uint32_t *)ctx); }
static void hangup_call(void *ctx) { (void)xiaotai_call_hangup(*(const uint32_t *)ctx); }

static void scene_call(const char *home)
{
    xiaotai_transport_host_connect_hook(call_connect, NULL);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_room\",\"caller_id\":\"peer\",\"caller_name\":\"客厅\"}}");
    xiaotai_call_status_t status;
    if (!call_wait(XIAOTAI_CALL_INCOMING, &status)) {
        fprintf(stderr, "SCREENS no incoming call\n");
        xiaotai_transport_host_connect_hook(NULL, NULL);
        return;
    }
    shoot("call-incoming");
    uint32_t session = status.session;
    product_on_owner(accept_call, &session);
    for (unsigned i = 0; i < WAIT_TRIES && !atomic_load(&s_request); ++i) xiaotai_delay_ms(STEP_MS);
    shoot("call-connecting");
    if (atomic_load(&s_request)) {
        xiaotai_transport_host_connected(atomic_load(&s_request), CALL_PEER, XIAOTAI_LINK_END_PEER, 0);
        if (call_wait(XIAOTAI_CALL_ACTIVE, &status)) {
            shoot("call-active");
            if (tap_control("companion.call.mute", NULL)) {
                shoot("call-muted");
                (void)tap_control("companion.call.mute", NULL);   /* the mute is a saved setting: put it back */
                xiaotai_delay_ms(300);
            }
        }
    }
    product_on_owner(hangup_call, &session);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    (void)call_wait(XIAOTAI_CALL_IDLE, &status);
    xiaotai_delay_ms(800);
    shoot("call-ended");
    if (on_page("result")) (void)tap_control(NULL, "确认");   /* a result waits to be read */
    xiaotai_delay_ms(1500);
    back_to(home);
}

static bool open_menu_entry(const char *home, const char *label, const char *page)
{
    back_to(home);
    if (!tap_control(NULL, "menu") || !wait_page("menu", 2000)) return false;
    for (unsigned i = 0; i < 4 && !tap_control(NULL, label); ++i) scroll_down();
    return wait_page(page, 2000);
}

/* A contact's page, a call placed from it, and how the call ends when nobody
 * answers the request (the fixture has no call server behind it). */
static void scene_dial(const char *home)
{
    if (!open_menu_entry(home, "通讯录", "contacts")) { back_to(home); return; }
    xiaotai_delay_ms(800);
    if (tap_control(NULL, "张经理") && wait_page("contact", 2000)) {
        shoot("contact-wechat");
        (void)tap_control("companion.back", NULL);
        (void)wait_page("contacts", 2000);
    }
    if (tap_control(NULL, "前台") && wait_page("contact", 2000)) {
        shoot("contact-device");
        if (tap_control(NULL, "语音呼叫")) {
            xiaotai_delay_ms(400);
            if (on_page("contact")) {   /* the owner queue was busy: once more */
                xiaotai_delay_ms(XIAOTAI_TOAST_DEFAULT_MS);
                (void)tap_control(NULL, "语音呼叫");
                xiaotai_delay_ms(400);
            }
            shoot("call-outgoing");
            for (unsigned i = 0; i < 300 && !on_page("result"); ++i) xiaotai_delay_ms(100);
            shoot("call-outgoing-ended");
            if (on_page("call")) (void)tap_control("companion.call.hangup", NULL);
            xiaotai_delay_ms(1500);
            if (on_page("result")) (void)tap_control(NULL, "确认");
        }
    }
    xiaotai_delay_ms(1000);
    back_to(home);
}

static bool field_button(const char *label, bool last, xiaotai_ui_control_t *out)
{
    xiaotai_ui_control_t controls[MAX_CONTROLS];
    size_t count = 0;
    bool found = false;
    if (xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000) != XIAOTAI_OK) return false;
    for (size_t i = 0; i < count && i < MAX_CONTROLS; ++i) {
        if (controls[i].element != XIAOTAI_UI_ELEMENT_FIELD || strcmp(controls[i].text, label)) continue;
        if (!found || (last ? controls[i].x > out->x : controls[i].x < out->x)) *out = controls[i];
        found = true;
    }
    return found;
}

/* Settings: both ends of the volume, the rest of the form, an open choice and
 * the confirmation before set-up starts again. */
static void scene_settings(const char *home)
{
    if (!open_menu_entry(home, "设置", "settings")) { back_to(home); return; }
    xiaotai_ui_control_t control;
    for (unsigned i = 0; i < 12 && field_button("音量", false, &control) && !(control.state & XIAOTAI_STATE_DISABLED); ++i)
        (void)tap(&control);
    shoot("settings-volume-min");
    for (unsigned i = 0; i < 12 && field_button("音量", true, &control) && !(control.state & XIAOTAI_STATE_DISABLED); ++i)
        (void)tap(&control);
    shoot("settings-volume-max");
    for (unsigned i = 0; i < 3 && field_button("音量", false, &control); ++i) (void)tap(&control);
    if (field_button("麦克风", false, &control) && tap(&control)) shoot("settings-mic-off");
    if (field_button("麦克风", false, &control)) (void)tap(&control);
    for (unsigned i = 0; i < 4 && !field_button("自动息屏", false, &control); ++i) scroll_down();
    shoot("settings-more");
    if (field_button("自动息屏", false, &control) && tap(&control)) {
        xiaotai_delay_ms(400);
        shoot("settings-choice-open");
        xiaotai_ui_info_t ui;
        if (xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK) (void)xiaotai_ui_tap(0, (int16_t)(ui.width / 2), 6);
        xiaotai_delay_ms(500);
    }
    /* Until the last row shows whole, not just its top edge. */
    for (unsigned i = 0; i < 4 && !(find_control(NULL, "重新配网", &control) && control.height >= 24); ++i)
        scroll_down();
    shoot("settings-end");
    if (tap_control(NULL, "重新配网")) {
        xiaotai_delay_ms(400);
        shoot("settings-reprovision-confirm");
        (void)tap_control(NULL, "取消");
        xiaotai_delay_ms(400);
    }
    back_to(home);
}

static void scene_ai(const char *home)
{
    product_ai_prepare();
    const uint32_t session = product_ai_request();
    shoot("ai-connecting");
    product_ai_connect(session);
    shoot("ai-listening");
    /* The user's words while they speak, the pause while the AI thinks, a
     * short answer and one long enough to fill the caption area. */
    product_ai_peer_message("{\"method\":\"caption\",\"params\":{\"caption_type\":0,\"seq_num\":1,"
        "\"utterance_id\":1,\"text\":\"明天上海天气怎么样\",\"is_final\":false}}");
    shoot("ai-user-caption");
    product_ai_peer_message("{\"method\":\"caption\",\"params\":{\"caption_type\":0,\"seq_num\":2,"
        "\"utterance_id\":1,\"text\":\"明天上海天气怎么样？\",\"is_final\":true}}");
    shoot("ai-thinking");
    product_ai_peer_message("{\"method\":\"caption\",\"params\":{\"caption_type\":1,\"seq_num\":1,"
        "\"utterance_id\":2,\"text\":\"明天上海多云转晴，12 到 19 度。\",\"emotion\":\"开心\",\"is_final\":false}}");
    shoot("ai-speaking");
    product_ai_peer_message("{\"method\":\"caption\",\"params\":{\"caption_type\":1,\"seq_num\":1,"
        "\"utterance_id\":3,\"text\":\"好的，已经帮你把 Wi-Fi 音箱的音量调到 60%，另外提醒一下："
        "明天早上 7:30 的闹钟还开着，需要我顺便把 XiaoTai 的勿扰模式也打开吗？\",\"is_final\":false}}");
    shoot("ai-speaking-long");
    product_ai_stop(session);
    shoot("ai-ended");
    xiaotai_delay_ms(3000);
    back_to(home);
}

/* Applications are reached by intent name, so this file names no product and
 * needs no application header: a scene runs only where its intent exists. */
static void post(const char *intent, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    (void)xiaotai_intent_post(intent, &args);
    xiaotai_delay_ms(SETTLE_MS);
}

static void switch_home_app(void *ctx) { (void)xiaotai_app_switch(xiaotai_app_home()); (void)ctx; }

/* The alarm clock: clock screen, a new alarm, the list, ringing, stop. */
static void scene_alarm(void)
{
    if (!xiaotai_intent_exists("alarm.home")) return;
    post("alarm.home", 0, NULL);
    shoot("alarm-home");
    post("alarm.edit", 0, NULL);
    post("alarm.minute", 30, NULL);
    shoot("alarm-edit");
    post("alarm.save", 0, NULL);
    shoot("alarm-list");
    post("alarm.back", 0, NULL);
    post("alarm.back", 0, NULL);
    shoot("alarm-home-set");
    post("alarm.timer", 0, NULL);
    shoot("alarm-timer");
    post("alarm.timer.start", 0, NULL);
    shoot("alarm-timer-running");
    post("alarm.timer.cancel", 0, NULL);
    post("alarm.back", 0, NULL);
    post("alarm.settings", 0, NULL);
    shoot("alarm-settings");
    post("alarm.back", 0, NULL);
    post("clock.countdown", 1, "该起床了");   /* the clock module fires it: the real ringing path */
    xiaotai_delay_ms(1600);
    shoot("alarm-ringing");
    post("alarm.snooze", 0, NULL);
    shoot("alarm-snoozed");
    post("alarm.timer", 0, NULL);
    post("alarm.timer.cancel", 0, NULL);   /* the snooze countdown */
    post("alarm.back", 0, NULL);
    product_on_owner(switch_home_app, NULL);
}

static void emit_call(const char *name, const xiaotai_call_status_t *status)
{
    const xiaotai_args_t args = {.value = (int32_t)status->state};
    (void)xiaotai_event_emit(name, &args, status, sizeof(*status));
    xiaotai_delay_ms(SETTLE_MS);
}

static void emit_message(uint32_t session, uint8_t type, const void *data, uint8_t size)
{
    xiaotai_call_message_t message = {.session = session, .type = type, .size = size};
    if (size) memcpy(message.data, data, size);
    const xiaotai_args_t args = {.value = (int32_t)session};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_CALL_MESSAGE, &args, &message, sizeof(message));
    xiaotai_delay_ms(SETTLE_MS);
}

static void switch_door_app(void *ctx) { (void)xiaotai_app_switch("door"); (void)ctx; }

/* Bring the door application forward with no session running, store the
 * role, and choose the first device contact ("客厅", id "peer") as the
 * other station. */
static void door_prepare(int32_t role)
{
    if (xiaotai_intent_exists("talk.stop")) post("talk.stop", 0, NULL);
    xiaotai_delay_ms(1500);
    product_on_owner(switch_door_app, NULL);
    post("door.home", 0, NULL);
    char page[40];
    current_page(page, sizeof(page));
    if (!strcmp(page, "door.mode")) {
        shoot("door-mode");
        post("door.mode", 1, NULL);
        xiaotai_delay_ms(XIAOTAI_TOAST_DEFAULT_MS);
        shoot("door-home-nothing-chosen");
    }
    post("door.role", role, NULL);
    post("door.settings", 0, NULL);
    post("door.devices", 0, NULL);
    xiaotai_delay_ms(800);
    shoot(role == 2 ? "access-choose" : role ? "station-choose" : "door-choose");
    post("door.select", 0, NULL);
    xiaotai_delay_ms(XIAOTAI_TOAST_DEFAULT_MS);   /* let the "selected" notice go */
}

/* The look's page as the application shows it while it runs (the fixture has
 * no call server a look could reach). */
static void show_look(void *ctx)
{
    const bool rung = ctx != NULL;
    xiaotai_ui_info_t info = {0};
    (void)xiaotai_ui_inspect(0, &info);
    const xiaotai_view_action_t looking[] = {
        {.label = "对讲", .intent = "door.look.talk", .value = 1, .icon = XIAOTAI_ICON_MIC, .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "音量", .intent = "door.volume", .value = 1, .icon = XIAOTAI_ICON_VOLUME},
        {.label = "开门", .intent = "door.open", .value = 1, .icon = XIAOTAI_ICON_KEY},
        {.label = "切换", .intent = "door.look.next", .value = 1, .icon = XIAOTAI_ICON_NEXT},
        {.label = "结束", .intent = "door.look.end", .value = 1, .icon = XIAOTAI_ICON_HANGUP,
         .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK},
    };
    const xiaotai_view_action_t ringing[] = {
        {.label = "接听", .intent = "door.look.talk", .value = 1, .icon = XIAOTAI_ICON_ACCEPT,
         .style = XIAOTAI_STYLE_POSITIVE, .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "忽略", .intent = "door.look.ignore", .value = 1, .icon = XIAOTAI_ICON_STOP},
        {.label = "开门", .intent = "door.open", .value = 1, .icon = XIAOTAI_ICON_KEY},
        {.label = "结束", .intent = "door.look.end", .value = 1, .icon = XIAOTAI_ICON_HANGUP,
         .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK},
    };
    const bool wide = info.width >= 800;
    const xiaotai_view_t view = {
        .kind = wide ? XIAOTAI_VIEW_CUSTOM : XIAOTAI_VIEW_CALL, .custom = wide ? "door.monitor" : NULL, .id = "door.monitor",
        .title = rung ? "门口有人按铃" : "查看门口", .subtitle = "室外机", .text = wide ? "正在等待门口画面" : "正在查看",
        .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 12, .actions = rung ? ringing : looking,
        .action_count = rung ? 4U : 5U,
    };
    xiaotai_view_show(&view);
}

/* Indoor station: a real door call from the station, open, the station's answer. */
static void scene_door_indoor(void)
{
    door_prepare(0);
    shoot("door-home");
    post("door.settings", 0, NULL);
    shoot("door-settings");
    post("door.mode", 2, NULL);
    shoot("door-mode-question");
    post("door.ask", 0, NULL);
    post("door.back", 0, NULL);
    /* Looking: the page while it connects, then the call's own end. */
    post("door.monitor", 0, NULL);
    shoot("door-look-connecting");
    post("door.look.end", 0, NULL);
    product_on_owner(show_look, (void *)(uintptr_t)0);
    xiaotai_delay_ms(SETTLE_MS);
    shoot("door-looking");
    product_on_owner(show_look, (void *)(uintptr_t)1);
    xiaotai_delay_ms(SETTLE_MS);
    shoot("door-looking-rung");
    xiaotai_delay_ms(1500);
    post("door.back", 0, NULL);
    atomic_store(&s_request, 0);
    xiaotai_transport_host_connect_hook(call_connect, NULL);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_door\",\"caller_id\":\"peer\",\"caller_name\":\"门口\"}}");
    xiaotai_call_status_t status;
    if (!call_wait(XIAOTAI_CALL_INCOMING, &status)) {
        fprintf(stderr, "SCREENS no door call\n");
        xiaotai_transport_host_connect_hook(NULL, NULL);
        return;
    }
    shoot("door-ringing");
    uint32_t session = status.session;
    product_on_owner(accept_call, &session);
    for (unsigned i = 0; i < WAIT_TRIES && !atomic_load(&s_request); ++i) xiaotai_delay_ms(STEP_MS);
    if (atomic_load(&s_request)) {
        xiaotai_transport_host_connected(atomic_load(&s_request), CALL_PEER, XIAOTAI_LINK_END_PEER, 0);
        if (call_wait(XIAOTAI_CALL_ACTIVE, &status)) {
            shoot("door-call");
            post("door.open", (int32_t)session, NULL);
            shoot("door-open-confirm");
            post("door.open.confirm", 1, NULL);
            shoot("door-opening");
            const uint8_t opened[4] = {0};   /* XIAOTAI_OK, little endian */
            emit_message(session, 2, opened, sizeof(opened));   /* the station's OPEN_RESULT */
            shoot("door-opened");
        }
    }
    product_on_owner(hangup_call, &session);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    (void)call_wait(XIAOTAI_CALL_IDLE, &status);
    xiaotai_delay_ms(3000);
    /* A visitor nobody answered. */
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_missed\",\"caller_id\":\"peer\",\"caller_name\":\"门口\"}}");
    if (call_wait(XIAOTAI_CALL_INCOMING, &status)) {
        product_platform_message("cmd", "{\"type\":\"call_cancel\",\"payload\":{\"room_id\":\"d_missed\"}}");
        (void)call_wait(XIAOTAI_CALL_IDLE, &status);
        xiaotai_delay_ms(1000);
        shoot("door-home-missed");
        post("door.log", 0, NULL);
        shoot("door-log");
        post("door.back", 0, NULL);
    }
}

/* Door station: the visitor calls, the resident answers and opens. The call
 * is shown from call events alone: the screens need no call server. */
static void scene_door_station(void)
{
    door_prepare(1);
    shoot("station-home");
    post("door.settings", 0, NULL);
    shoot("station-settings");
    post("door.back", 0, NULL);
    post("door.directory", 0, NULL);
    xiaotai_delay_ms(800);
    shoot("station-directory");
    post("door.dial", 0, NULL);   /* the first row: a WeChat user (the fixture's "张经理") */
    xiaotai_delay_ms(600);
    shoot("station-wechat-calling");
    if (xiaotai_intent_exists("wechat.hangup")) post("wechat.hangup", 0, NULL);
    xiaotai_delay_ms(3000);
    post("door.home", 0, NULL);
    xiaotai_call_status_t call = {.state = XIAOTAI_CALL_OUTGOING, .session = 0x7001U, .outgoing = true,
                                  .flags = XIAOTAI_CALL_VIDEO, .peer_id = "peer", .peer_name = "前台"};
    emit_call(XIAOTAI_EVENT_CALL_STATE, &call);
    shoot("station-calling");
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = 0x7002U;
    call.active_since_us = xiaotai_time_us();
    emit_call(XIAOTAI_EVENT_CALL_STATE, &call);
    shoot("station-talking");
    emit_message(call.session, 1, NULL, 0);   /* the indoor station's OPEN */
    shoot("station-opened");
    call.state = XIAOTAI_CALL_IDLE;
    emit_call(XIAOTAI_EVENT_CALL_STATE, &call);
    emit_call(XIAOTAI_EVENT_CALL_ENDED, &call);
    shoot("station-ended");
    xiaotai_delay_ms(3000);
    /* A chosen indoor station looks at the door. */
    atomic_store(&s_request, 0);
    xiaotai_transport_host_connect_hook(call_connect, NULL);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_look\",\"caller_id\":\"peer\",\"caller_name\":\"客厅\"}}");
    xiaotai_call_status_t status;
    for (unsigned i = 0; i < WAIT_TRIES && !atomic_load(&s_request); ++i) xiaotai_delay_ms(STEP_MS);
    if (atomic_load(&s_request)) {
        xiaotai_transport_host_connected(atomic_load(&s_request), CALL_PEER, XIAOTAI_LINK_END_PEER, 0);
        if (call_wait(XIAOTAI_CALL_ACTIVE, &status)) {
            uint32_t session = status.session;
            shoot("station-looked-at");
            post("door.ring", 0, NULL);
            shoot("station-looked-at-ringing");
            const uint8_t talks = 1;
            emit_message(session, 3, &talks, 1);   /* the indoor station's LOOK: it answers */
            shoot("station-looked-at-talking");
            product_on_owner(hangup_call, &session);
            (void)call_wait(XIAOTAI_CALL_IDLE, &status);
            xiaotai_delay_ms(3000);
        }
    }
    xiaotai_transport_host_connect_hook(NULL, NULL);
    post("door.role", 0, NULL);
}

/* The access machine: the keypad, a number, the households. */
static void scene_door_access(void)
{
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"管理处\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"peer2\",\"remark\":\"3栋301 王先生\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"peer3\",\"remark\":\"3栋302 李女士\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"peer4\",\"remark\":\"4栋301\",\"online\":false},"
        "{\"type\":\"voip\",\"device_id\":\"wx1\",\"remark\":\"4栋502 张经理\",\"online\":true,"
        "\"wx_app_id\":\"wxfixture\",\"wx_model_id\":\"fixture\"}]}}");
    door_prepare(2);
    shoot("access-home");
    post("door.directory", 0, NULL);
    xiaotai_delay_ms(800);
    shoot("access-households");
    post("door.back", 0, NULL);
    static const uint8_t room[] = {3, 0, 1, 3, 0, 2};
    for (size_t i = 0; i < 3U; ++i) post("door.key.digit", room[i], NULL);
    shoot("access-typed");
    post("door.key.call", 0, NULL);
    shoot("access-which");
    post("door.back", 0, NULL);
    for (size_t i = 3; i < 6U; ++i) post("door.key.digit", room[i], NULL);
    xiaotai_call_status_t call = {.state = XIAOTAI_CALL_OUTGOING, .session = 0x7101U, .outgoing = true,
                                  .flags = XIAOTAI_CALL_VIDEO, .peer_id = "peer", .peer_name = "3栋302 李女士"};
    emit_call(XIAOTAI_EVENT_CALL_STATE, &call);
    shoot("access-calling");
    call.state = XIAOTAI_CALL_IDLE;
    emit_call(XIAOTAI_EVENT_CALL_STATE, &call);
    emit_call(XIAOTAI_EVENT_CALL_ENDED, &call);
    xiaotai_delay_ms(3000);
    post("door.home", 0, NULL);
    post("door.role", 0, NULL);
}

static void scene_door(void)
{
    if (!xiaotai_intent_exists("door.ring")) return;
    scene_door_indoor();
    scene_door_station();
    scene_door_access();
    product_on_owner(switch_home_app, NULL);
}

void product_screens_run(const char *scenario, bool provisioned)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/index.tsv", out_dir());
    remove(path);
    xiaotai_delay_ms(1500);
    if (!provisioned) {
        char page[40];
        current_page(page, sizeof(page));
        shoot("setup");
        (void)seen(page);
        explore(page, 1);
        return;
    }
    product_platform_online();
    xiaotai_delay_ms(1500);
    if (!strcmp(scenario, "screens-gallery")) {
        product_screens_gallery();
        return;
    }
    char home[40];
    current_page(home, sizeof(home));
    if (!strcmp(home, "door.mode")) {   /* a product whose home is the door asks for its mode first */
        shoot("door-mode");
        post("door.mode", 1, NULL);
        xiaotai_delay_ms(XIAOTAI_TOAST_DEFAULT_MS);
        current_page(home, sizeof(home));
    }
    shoot("home");
    (void)seen(home);
    /* Sessions first: pages explored later (recognition, rooms) may hold the
     * foreground and refuse a new session. */
    scene_ai(home);
    scene_call(home);
    explore(home, 3);
    back_to(home);
    scene_dial(home);
    scene_settings(home);
    home_pages(home);
    scene_alarm();
    scene_door();
}
