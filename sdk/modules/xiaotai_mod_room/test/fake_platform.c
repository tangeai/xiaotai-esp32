// SPDX-License-Identifier: MIT
#include "fake_platform.h"

#include <assert.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/link.h"

fake_room_platform_t fixture;
static xiaotai_platform_signal_fn_t s_signal;
static void *s_signal_ctx;

void fake_reset(bool external)
{
    fixture = (fake_room_platform_t){.external_memory = external, .binding = 1, .device = "self", .limit = 2};
    s_signal = NULL;
    s_signal_ctx = NULL;
}

bool __wrap_xiaotai_mem_has_external(void) { return fixture.external_memory; }

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *requirement)
{
    fixture.platform = *requirement;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_require_service(const char *name)
{
    assert(!strcmp(name, "call"));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *requirement)
{
    if (requirement->command_pool_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES) return XIAOTAI_ERR_INVALID_SIZE;
    fixture.transport = *requirement;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *requirement)
{
    fixture.media = *requirement;
    return XIAOTAI_OK;
}

uint32_t xiaotai_identity_generation(void) { return fixture.binding; }

xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    const size_t size = strlen(fixture.device) + 1;
    assert(capacity >= size);
    memcpy(out, fixture.device, size);
    return XIAOTAI_OK;
}

void xiaotai_platform_status(xiaotai_platform_status_t *status)
{
    *status = (xiaotai_platform_status_t){.phase = fixture.online ? XIAOTAI_PLATFORM_ONLINE : XIAOTAI_PLATFORM_STOPPED,
        .generation = fixture.binding, .notify_revision = fixture.notify_revision, .clock_synced = fixture.clock_synced};
}

xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                               xiaotai_platform_signal_fn_t handler, void *ctx)
{
    assert(!strcmp(type, "room_assignment_changed") && !channel && !s_signal);
    s_signal = handler;
    s_signal_ctx = ctx;
    return XIAOTAI_OK;
}

void fake_notify(uint32_t generation)
{
    assert(xiaotai_on_owner());
    assert(s_signal);
    const xiaotai_platform_signal_t signal = {.type = "room_assignment_changed", .generation = generation};
    s_signal(&signal, s_signal_ctx);
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request,
                                      xiaotai_platform_reply_fn_t done, void *ctx)
{
    assert(xiaotai_on_owner());
    const bool connecting = !strcmp(request->path, "/v1/call/group/device/presence") &&
        request->body && strstr(request->body, "\"state\":\"connecting\"");
    assert(!strcmp(request->service, "call") && request->timeout_ms == (connecting ? 6000U : 8000U));
    assert(request->auth == XIAOTAI_PLATFORM_AUTH_SESSION && !request->bearer);
    if (!fixture.online) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    if (fixture.token_admission_result != XIAOTAI_OK &&
        !strcmp(request->path, "/v1/call/group/device/connect-token")) return fixture.token_admission_result;
    if (connecting) {
        ++fixture.connecting_attempts;
        if (fixture.connecting_admission_result != XIAOTAI_OK) return fixture.connecting_admission_result;
    }
    unsigned inflight = 0;
    for (unsigned i = 0; i < fixture.sent; ++i) inflight += fixture.requests[i].pending;
    if (inflight >= fixture.limit) return XIAOTAI_ERR_NO_MEM;
    assert(fixture.sent < sizeof(fixture.requests) / sizeof(fixture.requests[0]));
    recorded_request_t *next = &fixture.requests[fixture.sent++];
    next->done = done;
    next->ctx = ctx;
    next->generation = fixture.binding;
    next->method = request->method;
    next->flags = request->flags;
    next->pending = true;
    assert(strlen(request->path) < sizeof(next->path));
    strcpy(next->path, request->path);
    if (request->body) {
        assert(strlen(request->body) < sizeof(next->body));
        strcpy(next->body, request->body);
    }
    return XIAOTAI_OK;
}

void fake_reply(unsigned index, xiaotai_err_t error, unsigned status, int32_t code, const char *body)
{
    assert(xiaotai_on_owner());
    assert(index < fixture.sent && fixture.requests[index].pending);
    recorded_request_t *request = &fixture.requests[index];
    const xiaotai_platform_reply_t reply = {.result = error, .http_status = (int)status, .code = code,
        .body = body, .size = body ? strlen(body) : 0U, .generation = request->generation};
    request->pending = false;
    request->done(&reply, request->ctx);
}
