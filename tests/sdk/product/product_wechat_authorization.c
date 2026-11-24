// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::wechat_authorization_refresh
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char contact[] = "fixture-openid";
static atomic_uint call_requests, rtc_requests;

static void deny_call(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    if (request->method != XIAOTAI_HTTP_POST || !strstr(request->url, "/v1/voip/device/call")) {
        reply->status = 404;
        return;
    }
    static const char denial[] = "{\"code\":40205,\"data\":{}}";
    atomic_fetch_add(&call_requests, 1);
    reply->status = 200;
    reply->body = (const uint8_t *)denial;
    reply->body_size = sizeof(denial) - 1U;
}

void product_wechat_authorization_prepare(void)
{
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"voip\",\"device_id\":\"fixture-openid\",\"wx_app_id\":\"fixture-app\"}]}}");
    product_platform_http_responder(deny_call, NULL);
}

static void request_rtc(void *unused, xiaotai_link_kind_t kind, uint32_t id, const char *peer, const char *token)
{
    (void)unused; (void)kind; (void)id; (void)peer; (void)token;
    atomic_fetch_add(&rtc_requests, 1);
}

static void read_wechat(void *ctx) { xiaotai_wechat_status(ctx); }

static void dial_contact(void *ctx)
{
    assert(xiaotai_wechat_dial(contact, 0, ctx) == XIAOTAI_OK);
}

static xiaotai_contacts_status_t wait_contacts(uint32_t min_fetches)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    xiaotai_contacts_status_t status;
    do {
        xiaotai_contacts_stats_t stats;
        xiaotai_contacts_stats(&stats);
        xiaotai_contacts_status(&status);
        if (stats.fetches >= min_fetches && status.fresh && !status.loading && status.cached == 1)
            return status;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "contact cache: fresh=%u loading=%u cached=%u error=%d\n",
            status.fresh, status.loading, status.cached, status.error);
    assert(false);
    return status;
}

static void wait_result(bool present)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    xiaotai_ui_info_t ui;
    do {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && (strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_RESULT) == 0) == present &&
            (present || !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_HOME))) return;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "expected result=%u, got page=%s\n", (unsigned)present, ui.page);
    assert(false);
}

static void check_not_refreshing(uint32_t fetches)
{
    xiaotai_contacts_stats_t stats;
    xiaotai_contacts_status_t state;
    xiaotai_contacts_stats(&stats);
    xiaotai_contacts_status(&state);
    assert(stats.fetches == fetches && !state.loading && state.cached == 1);
    assert(atomic_load(&call_requests) == 1U && !atomic_load(&rtc_requests));
}

void product_wechat_authorization(bool use_back)
{
    product_platform_online();
    const xiaotai_contacts_status_t original = wait_contacts(1U);
    xiaotai_contacts_stats_t before, after;
    xiaotai_contacts_stats(&before);
    xiaotai_transport_host_connect_hook(request_rtc, NULL);
    uint32_t session = 0;
    product_on_owner(dial_contact, &session);
    wait_result(true);

    xiaotai_wechat_status_t status;
    product_on_owner(read_wechat, &status);
    assert(session && !status.session && status.authorization_result == session &&
           status.end == XIAOTAI_WECHAT_END_CONTACT);
    check_not_refreshing(before.fetches);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_END, &(xiaotai_args_t){.value = (int32_t)(session - 1U)})
           == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
    wait_result(true);
    check_not_refreshing(before.fetches);

    /* A single key only tries to advance focus. Let the real click window close. */
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    xiaotai_delay_ms(550U);
    wait_result(true);
    check_not_refreshing(before.fetches);
    assert(xiaotai_interaction_inject("main", use_back ? 0U : 2U, use_back ? 1400U : 0U) == XIAOTAI_OK);
    wait_result(false);
    const xiaotai_contacts_status_t current = wait_contacts(before.fetches + 1U);
    xiaotai_contacts_stats(&after);
    product_on_owner(read_wechat, &status);
    assert(!status.authorization_result && after.fetches == before.fetches + 1U &&
           current.generation == original.generation && current.revision != original.revision &&
           !atomic_load(&rtc_requests));
    xiaotai_transport_host_connect_hook(NULL, NULL);
    printf("WeChat authorization: %s closed exact result and refreshed contacts once\n",
           use_back ? "long key" : "double key");
}
