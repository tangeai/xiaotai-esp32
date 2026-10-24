// SPDX-License-Identifier: MIT
#pragma once
#include "demo_platform.h"
#include "esp_http_client.h"

typedef enum { PLATFORM_SERVICE_DEVICE, PLATFORM_SERVICE_VOIP, PLATFORM_SERVICE_AI, PLATFORM_SERVICE_CALL } platform_service_t;
typedef struct {
    const char *path;
    esp_http_client_method_t method;
    platform_service_t service;
} platform_route_t;

static inline const platform_route_t *platform_route(platform_operation_t operation)
{
    static const platform_route_t routes[PLATFORM_OPERATION_COUNT] = {
        [PLATFORM_PROFILE] = {"/v1/device/profile", HTTP_METHOD_POST, PLATFORM_SERVICE_DEVICE},
        [PLATFORM_CONTACTS] = {"/v1/voip/device/contacts", HTTP_METHOD_GET, PLATFORM_SERVICE_VOIP},
        [PLATFORM_CALL] = {"/v1/voip/device/call", HTTP_METHOD_POST, PLATFORM_SERVICE_VOIP},
        [PLATFORM_AI_TOKEN] = {"/v1/ai/token", HTTP_METHOD_GET, PLATFORM_SERVICE_AI},
        [PLATFORM_DEVICE_CONTACTS] = {"/v1/call/device/contacts", HTTP_METHOD_GET, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_REQUEST] = {"/v1/call/request", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_INFO] = {"/v1/call/device/info", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_REJECT] = {"/v1/call/reject", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_CANCEL] = {"/v1/call/cancel", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_HANGUP] = {"/v1/call/hangup", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CALL_ROOM] = {"/v1/call/room", HTTP_METHOD_GET, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CONTACTS_PENDING] = {"/v1/call/device/contacts/pending", HTTP_METHOD_GET, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CONTACT_REQUEST] = {"/v1/call/device/contacts/request", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CONTACT_RESPOND] = {"/v1/call/device/contacts/respond", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CONTACT_REMARK] = {"/v1/call/device/contacts/remark", HTTP_METHOD_PUT, PLATFORM_SERVICE_CALL},
        [PLATFORM_DEVICE_CONTACT_DELETE] = {"/v1/call/device/contacts", HTTP_METHOD_DELETE, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_ASSIGNMENT] = {"/v1/call/group/device/assignment", HTTP_METHOD_GET, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_CREATE] = {"/v1/call/group/device/create", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_JOIN] = {"/v1/call/group/device/join", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_LEAVE] = {"/v1/call/group/device/leave", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_CONNECT_TOKEN] = {"/v1/call/group/device/connect-token", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
        [PLATFORM_GROUP_PRESENCE] = {"/v1/call/group/device/presence", HTTP_METHOD_POST, PLATFORM_SERVICE_CALL},
    };
    return operation >= PLATFORM_PROFILE && operation < PLATFORM_OPERATION_COUNT ? &routes[operation] : NULL;
}
