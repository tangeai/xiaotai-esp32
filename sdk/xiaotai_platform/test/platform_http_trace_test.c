// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/platform_http_metadata_fixture.c
// Replaces legacy: tests/regression/test_platform_http_trace.py
#include <assert.h>
#include <string.h>
#include "platform_internal.h"

int main(void)
{
    char line[224];
    platform_slot_t request = {.service = "recognition", .method = XIAOTAI_PLATFORM_GET, .is_transfer = true,
        .path = "/v1/recognition/requests/fixture-room/audio/fixture-device?token=fixture-secret"};
    xiaotai_http_response_t response = {.status = 200, .timing = {
        .resolve_ms = 1, .connect_ms = 2, .send_ms = 3, .wait_ms = 4, .receive_ms = 5, .total_ms = 2999}};
    assert(!platform_http_trace_format(&request, &response, XIAOTAI_OK, line, sizeof(line)));
    ++response.timing.total_ms;
    assert(platform_http_trace_format(&request, &response, XIAOTAI_OK, line, sizeof(line)));
    assert(!strcmp(line, "recognition GET /v1/recognition/requests status=200 error=0 resolve=1 connect=2 send=3 wait=4 recv=5 total=3000"));
    request.is_transfer = false;
    strcpy(request.path, "/v1/custom/fixture-device?secret=fixture-secret");
    response.status = 429;
    response.timing.total_ms = 0;
    assert(platform_http_trace_format(&request, &response, XIAOTAI_OK, line, sizeof(line)));
    assert(strstr(line, "recognition GET - status=429") && !strstr(line, "fixture"));
    request.method = XIAOTAI_PLATFORM_POST;
    strcpy(request.path, "/v1/device/token-secret");
    response.status = 0;
    assert(platform_http_trace_format(&request, &response, XIAOTAI_FAIL, line, sizeof(line)));
    assert(strstr(line, "recognition POST - status=0") && !strstr(line, "secret"));

    char protected_body[] = "{\"room_id\":\"private-request-body\"}";
    strcpy(request.service, "call");
    strcpy(request.path, "/v1/call/group/device/presence/private-device?token=private-token");
    request.body = protected_body;
    request.body_size = strlen(protected_body);
    response.status = 503;
    assert(platform_http_trace_format(&request, &response, XIAOTAI_OK, line, sizeof(line)));
    assert(strstr(line, "call POST /v1/call/group status=503") && !strstr(line, "private-"));
    return 0;
}
