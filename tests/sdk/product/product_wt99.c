// SPDX-License-Identifier: MIT
#include "product_ai.h"
#include "product_platform.h"
#include "product_portal.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai/ui.h"
#include "xiaotai_net_host.h"
#include "xiaotai_transport_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT 10000U
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "WT99 %s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

void product_wt99_prepare(void);
void product_wt99_presenter(void);
void product_wt99_binding(void);
void product_wt99_tap_fixture_peer(bool already_selected);
void product_wt99_tap_named_fixture_peer(const char *label, bool already_selected);

static xiaotai_signal_t report_waiting, report_answer;

static void response(void *context, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)context;
    const char *body = NULL;
    if (!strcmp(req->url, "http://fixture.invalid/device/v1/device/report")) {
        static const char grant[] = "{\"code\":200,\"msg\":\"ok\",\"data\":{\"code\":\"123456\","
            "\"temp_token\":\"fixture-guest-token\",\"temp_client_id\":\"" PRODUCT_FIXTURE_GUEST "\"}}";
        CHECK(req->method == XIAOTAI_HTTP_POST);
        xiaotai_signal_give(&report_waiting);
        CHECK(xiaotai_signal_take(&report_answer, WAIT));
        body = grant;
    } else if (req->method == XIAOTAI_HTTP_POST && !strcmp(req->url, "http://fixture.invalid/call/v1/call/reject")) {
        body = "{\"code\":200}";
    } else if (req->method == XIAOTAI_HTTP_GET && !strcmp(req->url, "http://fixture.invalid/call/v1/call/room")) {
        body = "{\"code\":200,\"data\":null}";
    }
    reply->status = body ? 200 : 404;
    if (!body) return;
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
}

void product_wt99_prepare(void)
{
    CHECK(xiaotai_signal_init(&report_waiting) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&report_answer) == XIAOTAI_OK);
    product_platform_http_responder(response, NULL);
}

typedef struct {
    const char *foreground;
    xiaotai_app_entry_t entry;
    xiaotai_app_door_status_t door;
    xiaotai_app_companion_status_t companion;
    xiaotai_call_status_t call;
    xiaotai_ai_talk_status_t talk;
} state_t;

static void inspect(void *ctx)
{
    state_t *out = ctx;
    out->foreground = xiaotai_app_foreground();
    out->entry = xiaotai_app_entry();
    xiaotai_app_door_status(&out->door);
    xiaotai_app_companion_status(&out->companion);
    xiaotai_call_status(&out->call);
    xiaotai_ai_talk_status(&out->talk);
}

static state_t show(const char *app, const char *page, xiaotai_view_kind_t kind)
{
    xiaotai_ui_info_t ui = {0};
    state_t status = {0};
    const bool door = !strcmp(app, "door");
    for (unsigned attempt = 0; attempt < WAIT / 10; ++attempt) {
        product_on_owner(inspect, &status);
        if (status.foreground && !strcmp(status.foreground, app) &&
            !strcmp(door ? status.door.page : status.companion.page, page) &&
            xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK && ui.ready &&
            !strcmp(ui.page, page) && ui.page_kind == kind) {
            CHECK(!strcmp(ui.theme, "lcd480") && ui.width == 1024 && ui.height == 600);
            CHECK(ui.frames && !ui.present_fault);
            CHECK(status.door.foreground == door && status.companion.foreground != door);
            CHECK(!(door ? status.companion.page : status.door.page)[0]);
            return status;
        }
        xiaotai_delay_ms(10);
    }
    fprintf(stderr, "expected %s/%s(%u), got %s door=%s companion=%s UI=%s(%u)\n",
            app, page, (unsigned)kind, status.foreground ? status.foreground : "-",
            status.door.page, status.companion.page, ui.page, (unsigned)ui.page_kind);
    abort();
}

static state_t door_home(void) { return show("door", XIAOTAI_APP_DOOR_PAGE_HOME, XIAOTAI_VIEW_CUSTOM); }
static state_t companion_home(xiaotai_view_kind_t kind)
{
    return show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME, kind);
}

static void intent(const char *name, int32_t value)
{
    CHECK(xiaotai_intent_post(name, &(xiaotai_args_t){.value = value}) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
}

static void step_on_owner(void *ctx)
{
    const int direction = *(const int *)ctx;
    CHECK(xiaotai_app_page_step(direction) == XIAOTAI_OK);
}

static void step(int direction) { product_on_owner(step_on_owner, &direction); }

static xiaotai_ui_control_t labeled_control(const char *intent_name, const char *label)
{
    xiaotai_ui_control_t controls[32];
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        size_t count = 0;
        CHECK(xiaotai_ui_controls(0, controls, sizeof(controls) / sizeof(*controls), &count, 500) == XIAOTAI_OK);
        const size_t seen = count < sizeof(controls) / sizeof(*controls) ? count : sizeof(controls) / sizeof(*controls);
        for (size_t i = 0; i < seen; ++i) {
            if (!strcmp(controls[i].intent, intent_name) && controls[i].width && controls[i].height &&
                (!label || !strcmp(controls[i].text, label))) return controls[i];
        }
        xiaotai_delay_ms(10);
    }
    fprintf(stderr, "WT99 real door widget missing: %s (%s)\n", intent_name, label ? label : "any label");
    abort();
}

static xiaotai_ui_control_t control(const char *intent_name) { return labeled_control(intent_name, NULL); }

static void tap_control(xiaotai_ui_control_t row)
{
    CHECK(xiaotai_ui_tap(0, (int16_t)(row.x + row.width / 2), (int16_t)(row.y + row.height / 2)) == XIAOTAI_OK);
}

void product_wt99_tap_named_fixture_peer(const char *label, bool already_selected)
{
    (void)door_home();
    tap_control(control(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    (void)show("door", XIAOTAI_APP_DOOR_PAGE_DEVICES, XIAOTAI_VIEW_CUSTOM);
    const xiaotai_ui_control_t peer = labeled_control(XIAOTAI_INTENT_APP_DOOR_SELECT, label);
    CHECK(peer.element == XIAOTAI_UI_ELEMENT_ITEM);
    CHECK(((peer.state & XIAOTAI_STATE_CHECKED) != 0U) == already_selected);
    tap_control(peer);
    (void)door_home();
}

void product_wt99_tap_fixture_peer(bool already_selected)
{
    product_wt99_tap_named_fixture_peer("Peer", already_selected);
}

static void navigation(void)
{
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_DEVICES, XIAOTAI_VIEW_CUSTOM);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    door_home();
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_MONITOR, XIAOTAI_VIEW_CUSTOM);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    door_home();

    step(1);
    CHECK(companion_home(XIAOTAI_VIEW_HOME).entry == XIAOTAI_APP_ENTRY_NEXT);
    step(1);
    companion_home(XIAOTAI_VIEW_CLOCK);
    step(1);
    door_home();
    step(-1);
    CHECK(companion_home(XIAOTAI_VIEW_CLOCK).entry == XIAOTAI_APP_ENTRY_PREVIOUS);
    step(-1);
    companion_home(XIAOTAI_VIEW_HOME);
    step(-1);
    door_home();

    (void)control(XIAOTAI_INTENT_SYSTEM_MENU);
    intent(XIAOTAI_INTENT_SYSTEM_MENU, 0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_MENU, XIAOTAI_VIEW_LIST);
    /* The same valid action must not mount a door view in its background. */
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_MENU, XIAOTAI_VIEW_LIST);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    door_home();

    const xiaotai_ui_control_t wifi = control(XIAOTAI_INTENT_SYSTEM_NETWORK);
    CHECK(xiaotai_ui_tap(0, (int16_t)(wifi.x + wifi.width / 2), (int16_t)(wifi.y + wifi.height / 2)) == XIAOTAI_OK);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_NETWORK, XIAOTAI_VIEW_NOTICE);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    door_home();
}

static void set_station(void *unused)
{
    (void)unused;
    CHECK(xiaotai_setting_set_text(XIAOTAI_SETTING_APP_DOOR_PEER, "station") == XIAOTAI_OK);
}

static void call_flow(void)
{
    product_on_owner(set_station, NULL);
    CHECK(door_home().door.station_selected);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_station\",\"caller_id\":\"station\"}}");
    state_t s = show("door", XIAOTAI_APP_DOOR_PAGE_CALL, XIAOTAI_VIEW_CUSTOM);
    CHECK(s.door.presenting_call && s.door.door_calls == 1 && s.call.state == XIAOTAI_CALL_INCOMING);
    product_platform_message("cmd", "{\"type\":\"call_cancel\",\"payload\":{\"room_id\":\"d_station\"}}");
    s = door_home();
    CHECK(s.call.state == XIAOTAI_CALL_IDLE && s.call.end == XIAOTAI_CALL_END_PEER_CANCEL);

    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_visitor\",\"caller_id\":\"visitor\"}}");
    s = show("companion", XIAOTAI_APP_COMPANION_PAGE_CALL, XIAOTAI_VIEW_CALL);
    CHECK(s.entry == XIAOTAI_APP_ENTRY_PRESENT && !s.door.presenting_call &&
          s.door.door_calls == 1 && s.call.state == XIAOTAI_CALL_INCOMING);
    product_platform_message("cmd", "{\"type\":\"call_cancel\",\"payload\":{\"room_id\":\"d_visitor\"}}");
    s = show("companion", XIAOTAI_APP_COMPANION_PAGE_RESULT, XIAOTAI_VIEW_NOTICE);
    CHECK(s.call.state == XIAOTAI_CALL_IDLE && s.call.end == XIAOTAI_CALL_END_PEER_CANCEL && !s.companion.error_held);
    intent(XIAOTAI_INTENT_APP_COMPANION_END, 0);
    CHECK(door_home().entry == XIAOTAI_APP_ENTRY_RETURN);
}

static void stop_ai(void *ctx)
{
    CHECK(xiaotai_ai_talk_stop(*(const uint32_t *)ctx) == XIAOTAI_OK);
}

static void ai_flow(void)
{
    product_ai_prepare();
    uint32_t session = product_ai_request();
    state_t s = companion_home(XIAOTAI_VIEW_HOME);
    CHECK(s.entry == XIAOTAI_APP_ENTRY_PRESENT && s.talk.phase == XIAOTAI_AI_TALK_CONNECTING);
    product_ai_connect(session);
    CHECK(companion_home(XIAOTAI_VIEW_HOME).talk.phase == XIAOTAI_AI_TALK_LISTENING);
    product_on_owner(stop_ai, &session);
    s = door_home();
    CHECK(s.entry == XIAOTAI_APP_ENTRY_RETURN && s.talk.phase == XIAOTAI_AI_TALK_IDLE && !s.talk.session);
}

static void provision_impl(void *cancel)
{
    CHECK((cancel ? xiaotai_provisioning_cancel() : xiaotai_provisioning_begin(0)) == XIAOTAI_OK);
}

static void provision_flow(void)
{
    const xiaotai_net_host_step_t leave = {
        .result = XIAOTAI_OK, .event_count = 1,
        .events = {{.kind = XIAOTAI_NET_DRIVER_LOST, .token = product_platform_network_token(),
                    .loss = XIAOTAI_NET_LOSS_REQUESTED}},
    };
    CHECK(leave.events[0].token && xiaotai_net_host_step(XIAOTAI_NET_HOST_LEAVE, &leave) == XIAOTAI_OK);
    product_on_owner(provision_impl, NULL);
    CHECK(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, WAIT));
    xiaotai_provisioning_status_t provision;
    for (unsigned attempt = 0; attempt < WAIT / 10; ++attempt) {
        xiaotai_provisioning_status(&provision);
        if (provision.phase == XIAOTAI_PROVISIONING_WAITING) break;
        xiaotai_delay_ms(10);
    }
    CHECK(provision.phase == XIAOTAI_PROVISIONING_WAITING && provision.provision.ap_ssid[0]);
    CHECK(show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP, XIAOTAI_VIEW_NOTICE).entry == XIAOTAI_APP_ENTRY_PRESENT);
    CHECK(product_portal_stack() > 0);
    product_on_owner(provision_impl, &provision);
    CHECK(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_STOP, 1, WAIT));
    CHECK(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 2, WAIT));
    product_platform_online();
    CHECK(door_home().entry == XIAOTAI_APP_ENTRY_RETURN);
}

static void cold_bound_start(void)
{
    /* Keep the real STARTED callback pending long enough to check that
     * discovery can finish without starting platform login or MQTT. */
    xiaotai_transport_host_auto_started(false);
    product_platform_link_ready();
    xiaotai_transport_status_t rtc;
    for (unsigned attempt = 0; attempt < WAIT / 10; ++attempt) {
        xiaotai_transport_status(&rtc);
        if (rtc.state == XIAOTAI_TRANSPORT_STARTING &&
            xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == 1) break;
        xiaotai_delay_ms(10);
    }
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    CHECK(rtc.state == XIAOTAI_TRANSPORT_STARTING && xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == 1);
    CHECK(platform.clock_synced && platform.discovered && platform.held && !platform.authorized &&
          !platform.signaling && !product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
    xiaotai_transport_host_started();
    product_platform_online();
    xiaotai_transport_host_auto_started(true);
}

void product_wt99_presenter(void)
{
    CHECK(door_home().entry == XIAOTAI_APP_ENTRY_START);
    cold_bound_start();
    navigation();
    call_flow();
    ai_flow();
    provision_flow();
    xiaotai_signal_deinit(&report_answer);
    xiaotai_signal_deinit(&report_waiting);
    puts("WT99: actual door/companion navigation, station/visitor MQTT call, AI RTC and provisioning passed");
}

void product_wt99_binding(void)
{
    CHECK(door_home().entry == XIAOTAI_APP_ENTRY_START);
    CHECK(!xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
    product_platform_link_ready();
    CHECK(xiaotai_signal_take(&report_waiting, WAIT));
    show("door", "door.binding", XIAOTAI_VIEW_PROGRESS);
    xiaotai_signal_give(&report_answer);
    const state_t s = show("companion", XIAOTAI_APP_COMPANION_PAGE_BINDING, XIAOTAI_VIEW_CODE);
    CHECK(s.entry == XIAOTAI_APP_ENTRY_PRESENT && s.companion.setup);
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    CHECK(binding.phase == XIAOTAI_BINDING_WAITING && !strcmp(binding.verification, "123456") && binding.seconds_left);
    CHECK(product_platform_has_client(PRODUCT_FIXTURE_GUEST));
    product_platform_message_to(PRODUCT_FIXTURE_GUEST, "cmd", "{\"type\":\"auth_grant\",\"payload\":{"
        "\"device_id\":\"" PRODUCT_FIXTURE_DEVICE "\",\"device_key\":\"" PRODUCT_FIXTURE_SECRET "\"}}");
    product_platform_online();
    CHECK(door_home().entry == XIAOTAI_APP_ENTRY_RETURN);
    xiaotai_binding_status(&binding);
    CHECK(binding.phase == XIAOTAI_BINDING_BOUND &&
          xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
    xiaotai_binding_stats_t stats;
    xiaotai_binding_stats(&stats);
    CHECK(stats.grants == 1 && stats.stores == 1 && stats.confirmations == 1 && stats.bound == 1);
    xiaotai_signal_deinit(&report_answer);
    xiaotai_signal_deinit(&report_waiting);
    puts("WT99: actual binding report, guest MQTT grant and return to door.home passed");
}
