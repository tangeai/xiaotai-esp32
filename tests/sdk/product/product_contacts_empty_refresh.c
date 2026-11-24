// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/xmini/test_ui.c::ui
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/http.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

#define WAIT_MS 5000U
#define CONTACT_URL "http://fixture.invalid/call/v1/call/device/contacts"
#define REQUEST_URL CONTACT_URL "/pending"
#define DIRECTORY_ROW "companion.contact.row"

static atomic_uint directory_requests[2];
static atomic_int last_row;
static atomic_uint initial_sequence, initial_denied;
static atomic_bool initial_release;
static bool initial_error, initial_unavailable, initial_reconnect;

xiaotai_err_t __real_xiaotai_http_perform(xiaotai_http_t *, const xiaotai_http_request_t *, xiaotai_http_response_t *);
xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *, const xiaotai_http_request_t *, xiaotai_http_response_t *);
xiaotai_err_t __real_xiaotai_intent_post(const char *, const xiaotai_args_t *);
xiaotai_err_t __wrap_xiaotai_intent_post(const char *, const xiaotai_args_t *);
xiaotai_err_t __real_xiaotai_platform_request(const xiaotai_platform_request_t *, xiaotai_platform_reply_fn_t, void *);
xiaotai_err_t __wrap_xiaotai_platform_request(const xiaotai_platform_request_t *, xiaotai_platform_reply_fn_t, void *);

xiaotai_err_t __wrap_xiaotai_platform_request(const xiaotai_platform_request_t *request,
                                              xiaotai_platform_reply_fn_t callback, void *context)
{
    if (initial_unavailable && !strcmp(request->service, "call") &&
        !strcmp(request->path, "/v1/call/device/contacts/pending")) {
        atomic_fetch_add(&initial_denied, 1U);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    return __real_xiaotai_platform_request(request, callback, context);
}

xiaotai_err_t __wrap_xiaotai_intent_post(const char *name, const xiaotai_args_t *args)
{
    if (args && !strcmp(name, DIRECTORY_ROW)) atomic_store(&last_row, args->value);
    return __real_xiaotai_intent_post(name, args);
}

xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *request,
                                       xiaotai_http_response_t *response)
{
    int list = -1;
    if (!strcmp(request->url, CONTACT_URL)) list = 0;
    else if (!strcmp(request->url, REQUEST_URL)) list = 1;
    if (list >= 0) {
        bool authorized = false;
        assert(request->method == XIAOTAI_HTTP_GET && !request->body && !request->body_size);
        for (size_t i = 0; i < request->header_count; ++i) {
            if (!strcasecmp(request->headers[i].name, "Authorization"))
                authorized = !strncmp(request->headers[i].value, "Bearer ", 7U) && request->headers[i].value[7];
        }
        assert(authorized);
        if (atomic_load(&initial_sequence)) {
            const unsigned expected = list ? 3U : 2U;
            assert(atomic_fetch_add(&initial_sequence, 1U) == expected);
        }
        atomic_fetch_add(&directory_requests[list], 1U);
    }
    return __real_xiaotai_http_perform(client, request, response);
}

static void respond(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    if (strcmp(request->url, REQUEST_URL)) { reply->status = 404; return; }
    static const char json[] = "{\"code\":200,\"data\":{\"pending\":[]}}";
    reply->status = 200;
    reply->body = (const uint8_t *)json;
    reply->body_size = sizeof(json) - 1U;
}

void product_contacts_empty_prepare(void)
{
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":[]}}");
    product_platform_http_responder(respond, NULL);
}

static void observe_initial_profile(const xiaotai_http_host_request_t *request)
{
    assert(request->method == XIAOTAI_HTTP_POST);
    assert(atomic_fetch_add(&initial_sequence, 1U) == 1U);
}

static void initial_confirmation_reply(void *ctx, const xiaotai_http_host_request_t *request,
                                       xiaotai_http_host_reply_t *reply)
{
    if (strcmp(request->url, REQUEST_URL)) { reply->status = 404; return; }
    for (unsigned i = 0; i < WAIT_MS / 5U && !atomic_load(&initial_release); ++i)
        xiaotai_delay_ms(5U);
    assert(atomic_load(&initial_release));
    if (initial_error) {
        static const char failed[] = "{\"code\":44001,\"data\":{}}";
        reply->status = 200;
        reply->body = (const uint8_t *)failed;
        reply->body_size = sizeof(failed) - 1U;
        return;
    }
    respond(ctx, request, reply);
}

void product_contacts_initial_view_prepare(const char *scenario)
{
    initial_error = !strcmp(scenario, "error");
    initial_unavailable = !strcmp(scenario, "unavailable");
    initial_reconnect = !strcmp(scenario, "reconnect");
    assert(initial_error || initial_unavailable || initial_reconnect || !strcmp(scenario, "success"));
    atomic_store(&initial_sequence, 1U);
    product_contacts_empty_prepare();
    product_platform_profile_observer(observe_initial_profile);
    product_platform_http_responder(initial_confirmation_reply, NULL);
}

static void check_initial_view(unsigned epoch)
{
    product_platform_online();
    unsigned terminal = initial_unavailable ? 3U : 4U;
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        if (atomic_load(&initial_sequence) == terminal &&
            (!initial_unavailable || atomic_load(&initial_denied))) break;
        xiaotai_delay_ms(5U);
    }
    product_check_owner();
    xiaotai_stream_status_t stream;
    xiaotai_stream_status(&stream);
    printf("initial profile -> contacts -> pending: sequence=%u lists=%u pending_GET=%u denied=%u phase=%u closed=%u\n",
           atomic_load(&initial_sequence), atomic_load(&directory_requests[0]),
           atomic_load(&directory_requests[1]), atomic_load(&initial_denied), stream.phase, stream.closed);
    fflush(stdout);
    assert(atomic_load(&initial_sequence) == terminal && atomic_load(&directory_requests[0]) == epoch);
    assert(stream.phase == XIAOTAI_STREAM_CLOSED && !stream.generation);
    xiaotai_contacts_pending_status_t pending;
    xiaotai_contacts_pending_status(&pending);
    if (initial_unavailable) {
        assert(atomic_load(&initial_denied) == 1U && atomic_load(&directory_requests[1]) == 0U);
        assert(!pending.loading && !pending.fresh && pending.error == XIAOTAI_ERR_NOT_SUPPORTED);
        puts("pending request not accepted: explicit error and H5 listener remains closed");
        return;
    }
    assert(atomic_load(&directory_requests[1]) == epoch && pending.loading && !pending.fresh);
    atomic_store(&initial_release, true);
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        xiaotai_stream_status(&stream);
        if (stream.phase == XIAOTAI_STREAM_LISTENING) break;
        xiaotai_delay_ms(5U);
    }
    xiaotai_contacts_pending_status(&pending);
    assert(stream.phase == XIAOTAI_STREAM_LISTENING && !pending.loading && !pending.total);
    if (initial_error) assert(!pending.fresh && pending.error != XIAOTAI_OK && pending.code == 44001);
    else assert(pending.fresh && pending.error == XIAOTAI_OK);
    assert(atomic_load(&directory_requests[1]) == epoch);
    printf("pending completed: fresh=%u error=%d code=%d, H5 listener open\n",
           (unsigned)pending.fresh, pending.error, pending.code);
}

void product_contacts_initial_view(void)
{
    check_initial_view(1U);
    if (!initial_reconnect) return;
    xiaotai_platform_status_t before, after;
    xiaotai_stream_status_t stream;
    xiaotai_platform_status(&before);
    xiaotai_platform_set_network(false, NULL);
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        xiaotai_stream_status(&stream);
        if (stream.phase == XIAOTAI_STREAM_CLOSED) break;
        xiaotai_delay_ms(5U);
    }
    assert(stream.phase == XIAOTAI_STREAM_CLOSED);
    atomic_store(&initial_sequence, 1U);
    atomic_store(&initial_release, false);
    static const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    xiaotai_platform_set_network(true, mac);
    check_initial_view(2U);
    xiaotai_platform_status(&after);
    assert(before.generation && before.generation == after.generation);
    puts("same binding reconnect: new profile/list/pending cycle before H5 arms again");
}

static uint32_t empty_revision(bool requests, int32_t filter)
{
    if (requests) {
        xiaotai_contacts_pending_status_t p;
        xiaotai_contacts_pending_status(&p);
        return p.fresh && !p.loading && !p.error && !p.page && !p.page_count ? p.revision : 0U;
    }
    int kind;
    assert(xiaotai_contacts_kind_find(filter == XIAOTAI_APP_COMPANION_CONTACTS_WECHAT ? "wechat" : "device",
                                      &kind) == XIAOTAI_OK);
    xiaotai_contacts_status_t p;
    xiaotai_contacts_status(&p);
    return p.fresh && p.kind == kind && !p.loading && !p.error && !p.page && !p.page_count ? p.revision : 0U;
}

static xiaotai_ui_info_t wait_empty(const char *page, bool requests, int32_t filter, unsigned wire, uint32_t revision)
{
    xiaotai_ui_info_t ui;
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && !ui.overlay[0] && !strcmp(ui.page, page) && empty_revision(requests, filter) > revision &&
            atomic_load(&directory_requests[requests]) >= wire)
            return ui;
        xiaotai_delay_ms(5U);
    }
    fprintf(stderr, "empty %s: requested=%u actual=%u revision=%lu saw=%s\n", page, wire,
            atomic_load(&directory_requests[requests]), (unsigned long)empty_revision(requests, filter), ui.page);
    assert(false);
    return ui;
}

static void open_page(const char *page, int32_t filter)
{
    xiaotai_args_t args = {.value = filter};
    assert(strlen(page) < sizeof(args.text));
    strcpy(args.text, page);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &args) == XIAOTAI_OK);
}

static void click_confirm(void)
{
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        xiaotai_interaction_input_t input;
        assert(xiaotai_interaction_input(0, &input) == XIAOTAI_OK);
        if (!input.injected && !input.down && !input.clicks) return;
        xiaotai_delay_ms(5U);
    }
    assert(false && "Confirm gesture did not finish");
}

static bool has_empty_refresh(void)
{
    xiaotai_ui_control_t controls[6];
    size_t count;
    assert(xiaotai_ui_controls(0, controls, sizeof(controls) / sizeof(controls[0]), &count, WAIT_MS) == XIAOTAI_OK);
    for (size_t i = 0; i < count && i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (controls[i].element == XIAOTAI_UI_ELEMENT_ACTION && !strcmp(controls[i].intent, DIRECTORY_ROW)) return true;
    }
    return false;
}

static void owner_view(void *ctx) { xiaotai_app_companion_status(ctx); }

static void wait_buffered_click(void)
{
    /* Keys dispatch to the UI task and back to the owner; use a UI task
     * snapshot barrier to observe clicks even when there is no action. */
    size_t count;
    assert(xiaotai_ui_controls(0, NULL, 0, &count, WAIT_MS) == XIAOTAI_OK);
    product_check_owner();
}

void product_contacts_empty_refresh(const char *scenario)
{
    const bool requests = !strcmp(scenario, "requests");
    assert(requests || !strcmp(scenario, "wechat"));
    const char *page = requests ? XIAOTAI_APP_COMPANION_PAGE_REQUESTS : XIAOTAI_APP_COMPANION_PAGE_CONTACTS;
    const int32_t filter = XIAOTAI_APP_COMPANION_CONTACTS_WECHAT;
    product_platform_online();
    if (!requests) {
        open_page(page, XIAOTAI_APP_COMPANION_CONTACTS_DEVICE);
        (void)wait_empty(page, false, XIAOTAI_APP_COMPANION_CONTACTS_DEVICE, 1U, 0U);
        wait_buffered_click();
        const unsigned device = atomic_load(&directory_requests[0]);
        assert(!has_empty_refresh());
        click_confirm();
        wait_buffered_click();
        assert(atomic_load(&directory_requests[0]) == device);
        assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL) == XIAOTAI_OK);
        product_check_owner();
        puts("device: empty remains passive; Confirm produced zero HTTP requests");
    }
    open_page(page, requests ? 0 : filter);
    (void)wait_empty(page, requests, filter, 1U, 0U);
    wait_buffered_click();
    const unsigned before = atomic_load(&directory_requests[requests]);
    const unsigned other = atomic_load(&directory_requests[!requests]);
    const uint32_t revision = empty_revision(requests, filter);
    click_confirm();
    (void)wait_empty(page, requests, filter, before + 1U, revision);
    assert(atomic_load(&directory_requests[requests]) == before + 1U &&
           atomic_load(&directory_requests[!requests]) == other);
    printf("%s: one extra authenticated GET after Confirm on fresh empty page zero\n", scenario);

    uint32_t ticket = 0;
    assert(xiaotai_platform_gate_close(&ticket) == XIAOTAI_OK && ticket);
    xiaotai_app_companion_status_t held, after;
    product_on_owner(owner_view, &held);
    click_confirm();
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        product_on_owner(owner_view, &after);
        if (after.refused != held.refused) break;
        xiaotai_delay_ms(5U);
    }
    assert(after.refused == held.refused + 1U && atomic_load(&directory_requests[requests]) == before + 1U);
    assert(xiaotai_platform_gate_open(ticket) == XIAOTAI_OK);
    printf("%s: maintenance rejected Confirm without an HTTP request\n", scenario);

    const xiaotai_args_t old_action = {.value = atomic_load(&last_row)};
    assert(old_action.value > 0);
    xiaotai_platform_set_network(false, NULL);
    xiaotai_platform_status_t platform;
    for (unsigned i = 0; i < WAIT_MS / 5U; ++i) {
        xiaotai_platform_status(&platform);
        if (platform.phase != XIAOTAI_PLATFORM_ONLINE) break;
        xiaotai_delay_ms(5U);
    }
    assert(platform.phase != XIAOTAI_PLATFORM_ONLINE);
    assert(xiaotai_intent_post(DIRECTORY_ROW, &old_action) == XIAOTAI_OK);
    wait_buffered_click();
    assert(atomic_load(&directory_requests[requests]) == before + 1U &&
           atomic_load(&directory_requests[!requests]) == other);
    printf("%s: delayed Confirm from online view cannot refresh offline\n", scenario);
}
