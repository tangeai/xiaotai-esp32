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
#include "product_test.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"
#include "xiaotai_ui_preview_host.h"
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

void product_screens_prepare(bool provisioned)
{
    if (!provisioned) return;
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"客厅\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"peer2\",\"remark\":\"书房\",\"online\":false},"
        "{\"type\":\"wechat\",\"device_id\":\"wx1\",\"remark\":\"妈妈\",\"online\":true}]}}");
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
static void shoot(const char *name)
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
    const int16_t x = (int16_t)(ui.width / 2), top = (int16_t)(ui.height / 4), bottom = (int16_t)(ui.height * 3 / 4);
    for (unsigned i = 0; i < 100 && xiaotai_ui_swipe(0, x, bottom, x, top, 300) == XIAOTAI_ERR_INVALID_STATE; ++i)
        xiaotai_delay_ms(20);
    xiaotai_delay_ms(700);
}

static bool runs_past_bottom(const xiaotai_ui_control_t *controls, size_t count)
{
    xiaotai_ui_info_t ui;
    if (xiaotai_ui_inspect(0, &ui) != XIAOTAI_OK) return false;
    for (size_t i = 0; i < count; ++i)
        if (controls[i].y + controls[i].height >= ui.height - 8) return true;
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
    if (xiaotai_ui_controls(0, controls, MAX_CONTROLS, &count, 1000) != XIAOTAI_OK) return;
    if (count > MAX_CONTROLS) count = MAX_CONTROLS;
    if (scrolled) shoot("scrolled");
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_ui_control_t c = controls[i];
        if (!c.intent[0] || (c.state & XIAOTAI_STATE_DISABLED) || is_back(c.intent)) continue;
        /* Leave calls, sessions and destructive actions to the scripted scenes. */
        if (strstr(c.intent, "call") || strstr(c.intent, "talk") || strstr(c.intent, "reset") ||
            strstr(c.intent, "unbind") || strstr(c.intent, "reboot") || strstr(c.intent, "stream") ||
            strstr(c.intent, "room") || strstr(c.intent, "wechat") || strstr(c.intent, "contact.action") ||
            strstr(c.intent, "contact.first") || strstr(c.intent, "home.start") ||
            !strcmp(c.intent, "companion.home")) continue;
        if (!tap(&c)) continue;
        char page[40];
        current_page(page, sizeof(page));
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
    if (!scrolled && runs_past_bottom(controls, count)) explore_at(from, depth, true);
}

/* The home screen's own pages (face, clock and the like) behind next. */
static void home_pages(const char *home)
{
    for (unsigned n = 2; n <= 4; ++n) {
        if (!tap_intent("companion.home")) return;
        char page[40];
        current_page(page, sizeof(page));
        if (strcmp(page, home)) {
            back_to(home);
            return;
        }
        char name[16];
        snprintf(name, sizeof(name), "home-%u", n);
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
        if (call_wait(XIAOTAI_CALL_ACTIVE, &status)) shoot("call-active");
    }
    product_on_owner(hangup_call, &session);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    (void)call_wait(XIAOTAI_CALL_IDLE, &status);
    xiaotai_delay_ms(800);
    shoot("call-ended");
    xiaotai_delay_ms(7000);
    back_to(home);
}

static void scene_ai(const char *home)
{
    product_ai_prepare();
    const uint32_t session = product_ai_request();
    shoot("ai-connecting");
    product_ai_connect(session);
    shoot("ai-talking");
    product_ai_stop(session);
    xiaotai_delay_ms(3000);
    back_to(home);
}

void product_screens_run(bool provisioned)
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
    char home[40];
    current_page(home, sizeof(home));
    shoot("home");
    (void)seen(home);
    /* Sessions first: pages explored later (recognition, rooms) may hold the
     * foreground and refuse a new session. */
    scene_ai(home);
    scene_call(home);
    explore(home, 2);
    back_to(home);
    home_pages(home);
}
