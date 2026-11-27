// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
// Replaces legacy: tests/regression/test_environment_control.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/platform.h"
#include "xiaotai_net_host.h"
#include "xiaotai_transport_host.h"

static const char BASE[] = "http://fixture.invalid/environment";
static atomic_uint assignment_requests;
static atomic_bool release_assignment;
static atomic_bool control_opened, release_control, control_done, repair_opened, release_repair;
static atomic_uint dial_requests;

static void wait_for(const atomic_bool *flag)
{
    for (unsigned tries = 0; tries < 400 && !atomic_load(flag); ++tries) xiaotai_delay_ms(5);
    assert(atomic_load(flag));
}

static void respond(xiaotai_http_host_reply_t *reply, const char *body)
{
    reply->status = 200;
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
}

static void room_http(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    static const char assignment[] =
        "{\"code\":200,\"data\":{\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
        "\",\"desired_state\":\"left\",\"assignment_version\":1}}";
    reply->status = 404;
    if (request->method == XIAOTAI_HTTP_GET &&
        !strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/assignment")) {
        atomic_fetch_add(&assignment_requests, 1U);
        wait_for(&release_assignment);
        respond(reply, assignment);
    } else if (request->method == XIAOTAI_HTTP_GET &&
               !strcmp(request->url, "http://fixture.invalid/call/fixture/hold")) {
        atomic_store(&control_opened, true);
        wait_for(&release_control);
        respond(reply, "{\"code\":200,\"data\":null}");
    } else if (request->method == XIAOTAI_HTTP_POST &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/request")) {
        atomic_fetch_add(&dial_requests, 1U);
        respond(reply, "{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}");
    } else if (request->method == XIAOTAI_HTTP_GET &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/room")) {
        atomic_store(&repair_opened, true);
        wait_for(&release_repair);
        respond(reply, "{\"code\":200,\"data\":null}");
    } else if (request->method == XIAOTAI_HTTP_POST &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/reject")) {
        respond(reply, "{\"code\":200,\"data\":null}");
    }
}

static void check_pending_room(void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner() && !xiaotai_activity_current(NULL) && !xiaotai_room_settled());
    xiaotai_room_status_t room_before, room_after;
    xiaotai_room_status(&room_before);
    assert(room_before.attending && !room_before.assignment_known && !room_before.generation);
    const xiaotai_err_t result = xiaotai_environment_set(BASE);
    assert(result == XIAOTAI_ERR_BUSY);
    xiaotai_room_status(&room_after);
    assert(room_after.revision == room_before.revision && !xiaotai_room_settled());

    xiaotai_environment_status_t environment;
    xiaotai_platform_status_t platform;
    xiaotai_environment_status(&environment);
    xiaotai_platform_status(&platform);
    assert(environment.phase == XIAOTAI_ENVIRONMENT_IDLE && environment.error == XIAOTAI_ERR_BUSY);
    assert(!environment.write_attempted && !xiaotai_restart_pending());
    assert(!xiaotai_quiesced() && !net_gate_maintenance_held() && !platform.gated);
    assert(platform.phase == XIAOTAI_PLATFORM_ONLINE && atomic_load(&assignment_requests) == 1);
    assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 0);
    printf("room http in flight: env=0x%x unchanged revision=%u count=1 no write or RTC teardown\n",
           (unsigned)result, room_after.revision);
}

static void assignment_settled(void *ctx)
{
    xiaotai_room_status_t status;
    xiaotai_room_status(&status);
    *(bool *)ctx = xiaotai_room_settled() && status.assignment_known && !status.assigned;
}

static void switch_without_room_work(void *unused)
{
    (void)unused;
    assert(xiaotai_room_settled() && !xiaotai_activity_current(NULL));
    assert(net_gate_coprocessor_write_begin() == XIAOTAI_OK);
    assert(xiaotai_environment_set(BASE) == XIAOTAI_ERR_INVALID_STATE && !xiaotai_quiesced());
    xiaotai_environment_status_t refused;
    xiaotai_environment_status(&refused);
    assert(refused.phase == XIAOTAI_ENVIRONMENT_IDLE && !refused.write_attempted && !xiaotai_restart_pending());
    net_gate_coprocessor_end();
    assert(xiaotai_environment_set(BASE) == XIAOTAI_OK && xiaotai_quiesced());
    assert(net_gate_coprocessor_write_begin() == XIAOTAI_ERR_INVALID_STATE);
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_create(NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_join("123456", NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_leave() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_refresh() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

static void companion_released(void *unused)
{
    (void)unused;
    assert(net_gate_coprocessor_write_begin() == XIAOTAI_OK);
    net_gate_coprocessor_end();
    puts("companion maintenance: both leases exclude each other, no environment write");
}

static void refresh_contacts(void *unused)
{
    (void)unused;
    assert(xiaotai_contacts_refresh() == XIAOTAI_OK);
}

static void read_peer(void *ctx)
{
    int kind;
    xiaotai_contact_t entry;
    assert(xiaotai_contacts_kind_find(XIAOTAI_CALL_CONTACT_KIND, &kind) == XIAOTAI_OK);
    *(bool *)ctx = xiaotai_contacts_find("peer", kind, 0, &entry) == XIAOTAI_OK;
}

static void request_done(const xiaotai_platform_reply_t *reply, void *unused)
{
    (void)unused;
    assert(reply->result == XIAOTAI_OK && reply->http_status == 200);
    atomic_store(&control_done, true);
}

static void begin_held(void *ctx)
{
    uint32_t *refused = ctx;
    assert(!xiaotai_quiesced() && xiaotai_room_settled());
    assert(xiaotai_environment_set(BASE) == XIAOTAI_OK && xiaotai_quiesced());
    uint32_t session = UINT32_MAX;
    assert(xiaotai_call_dial("peer", 0, &session) == XIAOTAI_ERR_BUSY && !session);
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    *refused = call.refused_busy;
}

static void cancel_held(void *ctx)
{
    const uint32_t refused = *(const uint32_t *)ctx;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    assert(call.refused_busy == refused + 1U && !call.session && !xiaotai_activity_current(NULL));
    assert(!atomic_load(&dial_requests) && !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    xiaotai_environment_status_t env;
    xiaotai_environment_status(&env);
    assert(xiaotai_quiesced() && env.phase == XIAOTAI_ENVIRONMENT_WAITING && !env.write_attempted);
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

static void start_call(void *ctx)
{
    uint32_t *session = ctx;
    assert(!xiaotai_quiesced());
    assert(xiaotai_call_dial("peer", 0, session) == XIAOTAI_OK && *session);
}

static void attempt_during_repair(void *ctx)
{
    const uint32_t session = *(const uint32_t *)ctx;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    assert(call.state == XIAOTAI_CALL_OUTGOING && call.session == session && atomic_load(&dial_requests) == 1);
    assert(xiaotai_environment_set(BASE) == XIAOTAI_ERR_BUSY);
    xiaotai_environment_status_t env;
    xiaotai_environment_status(&env);
    xiaotai_call_status(&call);
    assert(env.phase == XIAOTAI_ENVIRONMENT_IDLE && !env.write_attempted && !xiaotai_quiesced());
    assert(call.state == XIAOTAI_CALL_OUTGOING && call.session == session && atomic_load(&dial_requests) == 1);
    assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    assert(xiaotai_call_hangup(session) == XIAOTAI_OK);
}

static void check_cancelled_repair(void *unused)
{
    (void)unused;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    assert(call.state == XIAOTAI_CALL_IDLE && !call.session && !xiaotai_activity_current(NULL));
    assert(atomic_load(&dial_requests) == 1 && !xiaotai_restart_pending());
}

static void check_call_maintenance(void)
{
    bool fresh = false;
    product_on_owner(refresh_contacts, NULL);
    for (unsigned tries = 0; tries < 400 && !fresh; ++tries) {
        product_on_owner(read_peer, &fresh);
        if (!fresh) xiaotai_delay_ms(5);
    }
    assert(fresh);
    /* Keep one ordinary exchange in flight so the real environment task
     * waits for an HTTP drain and never reaches storage during the checks. */
    static const xiaotai_platform_request_t block = {
        .method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_NONE,
        .service = XIAOTAI_PLATFORM_SERVICE_CALL, .path = "/fixture/hold",
    };
    assert(xiaotai_platform_request(&block, request_done, NULL) == XIAOTAI_OK);
    wait_for(&control_opened);
    uint32_t refused;
    product_on_owner(begin_held, &refused);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_held\",\"caller_id\":\"peer\"}}");
    for (unsigned tries = 0; tries < 400; ++tries) {
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        if (call.refused_busy != refused) break;
        xiaotai_delay_ms(5);
    }
    product_on_owner(cancel_held, &refused);
    atomic_store(&release_control, true);
    wait_for(&control_done);
    puts("call during ENV: outgoing/incoming refused, no RTC teardown or settings write");

    uint32_t session = 0;
    product_on_owner(start_call, &session);
    wait_for(&repair_opened);
    product_on_owner(attempt_during_repair, &session);
    atomic_store(&release_repair, true);
    for (unsigned tries = 0; tries < 400 && xiaotai_platform_exchanges_busy(); ++tries) xiaotai_delay_ms(5);
    assert(!xiaotai_platform_exchanges_busy());
    product_on_owner(check_cancelled_repair, NULL);
    puts("ENV during live 40202 room query: busy, same outbound session remains until explicit hangup");
}

void product_room_maintenance_prepare(void)
{
    product_platform_prepare(true);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"Peer\",\"online\":true}]}}");
    product_platform_http_responder(room_http, NULL);
}

void product_room_maintenance(void)
{
    product_platform_online();
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    for (unsigned tries = 0; tries < 200 && !atomic_load(&assignment_requests); ++tries) xiaotai_delay_ms(5);
    assert(atomic_load(&assignment_requests) == 1);
    product_on_owner(check_pending_room, NULL);

    atomic_store(&release_assignment, true);
    bool settled = false;
    for (unsigned tries = 0; tries < 200 && !settled; ++tries) {
        product_on_owner(assignment_settled, &settled);
        if (!settled) xiaotai_delay_ms(5);
    }
    assert(settled && atomic_load(&assignment_requests) == 1);
    product_on_owner(switch_without_room_work, NULL);
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    product_on_owner(companion_released, NULL);
    puts("room assignment finished: ENV admitted, create/join/leave/refresh refused while held");
    check_call_maintenance();
}
