// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"
#include "xiaotai/platform.h"
#include "xiaotai/transport.h"

typedef struct {
    xiaotai_platform_reply_fn_t done;
    void *ctx;
    unsigned generation, flags;
    char path[80], body[256];
    xiaotai_platform_method_t method;
    bool pending;
} recorded_request_t;

typedef struct {
    bool online, external_memory, clock_synced;
    unsigned binding, notify_revision, limit, sent;
    const char *device;
    xiaotai_err_t token_admission_result;
    xiaotai_err_t connecting_admission_result;
    unsigned connecting_attempts;
    recorded_request_t requests[64];
    xiaotai_platform_requirement_t platform;
    xiaotai_transport_requirement_t transport;
    xiaotai_media_requirement_t media;
} fake_room_platform_t;

extern fake_room_platform_t fixture;

void fake_reset(bool external);
void fake_notify(uint32_t generation);
void fake_reply(unsigned index, xiaotai_err_t error, unsigned status, int32_t code, const char *body);
