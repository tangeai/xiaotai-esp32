// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "panel_fake.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"

static atomic_bool release_ready;
static atomic_uint dials, release_replies;

static void call_http(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    const char *body = NULL;
    reply->status = 404;
    if (!strcmp(request->url, "http://fixture.invalid/call/v1/call/request")) {
        assert(request->method == XIAOTAI_HTTP_POST);
        reply->status = atomic_fetch_add(&dials, 1U) < 4U ? 201 : 200;
        body = "{\"code\":200,\"data\":{\"room_id\":\"retained-outgoing\"}}";
    } else if (!strcmp(request->url, "http://fixture.invalid/call/v1/call/room")) {
        assert(request->method == XIAOTAI_HTTP_GET);
        reply->status = 503;
        if (atomic_load(&release_ready)) {
            atomic_fetch_add(&release_replies, 1U);
            reply->status = 200;
            body = "{\"code\":200,\"data\":null}";
        }
    }
    if (body) {
        reply->body = (const uint8_t *)body;
        reply->body_size = strlen(body);
    }
}

void product_call_cleanup_prepare(void)
{
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"Peer\",\"online\":true}]}}");
    product_platform_http_responder(call_http, NULL);
}

typedef struct {
    xiaotai_call_status_t call;
    xiaotai_app_companion_status_t ui;
    xiaotai_platform_status_t platform;
} call_snapshot_t;

static void read_snapshot(void *ctx)
{
    call_snapshot_t *const s = ctx;
    xiaotai_call_status(&s->call);
    xiaotai_app_companion_status(&s->ui);
    xiaotai_platform_status(&s->platform);
}

static void dial_peer(void *ctx)
{
    *(xiaotai_err_t *)ctx = xiaotai_call_dial("peer", 0, NULL);
}

static void fill_release_slots(void)
{
    for (unsigned n = 1; n <= 4U; ++n) {
        xiaotai_err_t result = XIAOTAI_ERR_NOT_FOUND;
        for (unsigned i = 0; i < 400 && result == XIAOTAI_ERR_NOT_FOUND; ++i) {
            product_on_owner(dial_peer, &result);
            if (result == XIAOTAI_ERR_NOT_FOUND) xiaotai_delay_ms(5);
        }
        assert(result == XIAOTAI_OK);
        call_snapshot_t s;
        for (unsigned i = 0; i < 400; ++i) {
            product_on_owner(read_snapshot, &s);
            if (s.call.state == XIAOTAI_CALL_IDLE && s.ui.error_held) break;
            xiaotai_delay_ms(5);
        }
        assert(s.call.state == XIAOTAI_CALL_IDLE && s.call.end == XIAOTAI_CALL_END_FAILED &&
               !s.call.session && s.ui.error_held && s.call.releases_pending == n);
        assert(atomic_load(&dials) == n);
        assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_END, &(xiaotai_args_t){0}) == XIAOTAI_OK);
        product_on_owner(read_snapshot, &s);
        assert(!s.ui.error_held);
    }
}

static void wait_page(const char *page, const char *control)
{
    for (unsigned i = 0; i < 400; ++i) {
        xiaotai_ui_info_t ui;
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && !ui.present_fault && !strcmp(ui.page, page)) {
            if (!control) return;
            xiaotai_ui_control_t controls[16];
            size_t count = 0;
            assert(xiaotai_ui_controls(0, controls, 16, &count, 1000) == XIAOTAI_OK && count <= 16);
            for (size_t j = 0; j < count; ++j)
                if (!strcmp(controls[j].text, control) && !(controls[j].state & XIAOTAI_STATE_DISABLED)) return;
        }
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "missing Xmini page/control: %s / %s\n", page, control ? control : "-");
    assert(false);
}

static void count_footer_pixels(void *ctx)
{
    xiaotai_ui_info_t ui;
    assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK && ui.owner_timer &&
           ui.pixel == XIAOTAI_PIXEL_MONO1_VLSB && ui.width == 128 && ui.height == 64);
    const xt_oled_fake_t *panel = xt_oled_fake_state();
    assert(panel->on && panel->frame_bytes == 1024);
    unsigned lit = 0;
    /* The 128x64 theme's inverted toast covers its final eight pixel rows. */
    for (size_t i = 7U * ui.width; i < panel->frame_bytes; ++i)
        for (uint8_t bits = panel->frame[i]; bits; bits &= (uint8_t)(bits - 1U)) ++lit;
    *(unsigned *)ctx = lit;
}

static unsigned footer_pixels(void)
{
    unsigned lit = 0;
    product_on_owner(count_footer_pixels, &lit);
    return lit;
}

void product_call_cleanup(void)
{
    product_platform_online();
    fill_release_slots();
    const xiaotai_args_t contacts = {.text = XIAOTAI_APP_COMPANION_PAGE_CONTACTS,
                                    .value = XIAOTAI_APP_COMPANION_CONTACTS_DEVICE};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &contacts) == XIAOTAI_OK);
    wait_page(XIAOTAI_APP_COMPANION_PAGE_CONTACTS, "Peer");
    const unsigned plain_footer = footer_pixels();
    assert(plain_footer < 512U);

    call_snapshot_t before, after;
    product_on_owner(read_snapshot, &before);
    assert(before.call.releases_pending == 4 && !before.call.session && !before.call.releases_unconfirmed);
    const uint32_t rtc_before = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_CONNECT);
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    for (unsigned i = 0; i < 400; ++i) {
        product_on_owner(read_snapshot, &after);
        if (after.ui.refused > before.ui.refused) break;
        xiaotai_delay_ms(5);
    }
    assert(after.ui.refused == before.ui.refused + 1U && !after.call.session && after.call.releases_pending == 4);
    assert(atomic_load(&dials) == 4U && xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_CONNECT) == rtc_before);
    unsigned toast_footer = plain_footer;
    for (unsigned i = 0; i < 40 && toast_footer <= 512U; ++i) {
        toast_footer = footer_pixels();
        if (toast_footer <= 512U) xiaotai_delay_ms(5);
    }
    assert(toast_footer > 512U && "the declined dial must paint the inverted OLED toast");
    printf("fifth dial: pending=4 HTTP POST=4 RTC connect delta=0; OLED footer %u -> %u lit pixels\n",
           plain_footer, toast_footer);

    atomic_store(&release_ready, true);
    for (unsigned i = 0; i < 1000; ++i) {
        product_on_owner(read_snapshot, &after);
        if (!after.call.releases_pending) break;
        xiaotai_delay_ms(5);
    }
    assert(!after.call.releases_pending && !after.call.releases_unconfirmed && atomic_load(&release_replies) == 4U);
    assert(after.platform.generation == before.platform.generation && after.platform.phase == XIAOTAI_PLATFORM_ONLINE);
    wait_page(XIAOTAI_APP_COMPANION_PAGE_CONTACTS, "Peer");
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    wait_page(XIAOTAI_APP_COMPANION_PAGE_CALL, NULL);
    for (unsigned i = 0; i < 400 && atomic_load(&dials) != 5U; ++i) xiaotai_delay_ms(5);
    product_on_owner(read_snapshot, &after);
    assert(atomic_load(&dials) == 5U && after.call.state == XIAOTAI_CALL_OUTGOING && after.call.session);
    printf("same-binding cleanup confirmations=4 pending=0; redial reaches HTTP POST=5 and OUTGOING session=%u\n",
           after.call.session);
}
