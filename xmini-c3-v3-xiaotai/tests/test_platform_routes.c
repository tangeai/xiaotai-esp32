// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform_routes.h"

int main(void)
{
    assert(PLATFORM_PROFILE == 0 && PLATFORM_CONTACTS == 1 && PLATFORM_CALL == 2);
    assert(platform_route((platform_operation_t)-1) == NULL);
    assert(platform_route(PLATFORM_OPERATION_COUNT) == NULL);
    for (int i = 0; i < PLATFORM_OPERATION_COUNT; ++i) {
        const platform_route_t *route = platform_route((platform_operation_t)i);
        assert(route != NULL && strncmp(route->path, "/v1/", 4) == 0);
    }
    assert(!strcmp(platform_route(PLATFORM_PROFILE)->path, "/v1/device/profile"));
    assert(platform_route(PLATFORM_AI_TOKEN)->service == PLATFORM_SERVICE_AI);
    assert(platform_route(PLATFORM_AI_TOKEN)->method == HTTP_METHOD_GET);
    assert(!strcmp(platform_route(PLATFORM_DEVICE_CALL_REQUEST)->path, "/v1/call/request"));
    assert(platform_route(PLATFORM_DEVICE_CONTACT_REMARK)->method == HTTP_METHOD_PUT);
    assert(platform_route(PLATFORM_DEVICE_CONTACT_DELETE)->method == HTTP_METHOD_DELETE);
    assert(platform_route(PLATFORM_GROUP_ASSIGNMENT)->method == HTTP_METHOD_GET);
    assert(!strcmp(platform_route(PLATFORM_GROUP_CONNECT_TOKEN)->path, "/v1/call/group/device/connect-token"));
    puts("platform routes: all 22 operations, services, methods and legacy enum values passed");
    return 0;
}
