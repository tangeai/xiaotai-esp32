// SPDX-License-Identifier: MIT
#pragma once

#include "platform_internal.h"

typedef struct {
    platform_slot_t *slot;
    xiaotai_platform_transfer_result_t *result;
    xiaotai_http_response_t *response;
    int64_t next_signal_tick;
    char type[XIAOTAI_PLATFORM_CONTENT_TYPE_MAX], encoding[XIAOTAI_PLATFORM_CONTENT_TYPE_MAX];
    xiaotai_http_capture_t captures[2];
    bool began;
} platform_transfer_io_t;

/* Worker only, once per transfer. Storage belongs to its stack. The HTTP
 * port provides the piece buffer and calls the stream on that same worker. */
void platform_transfer_io_init(platform_transfer_io_t *io, platform_slot_t *slot,
    xiaotai_platform_transfer_result_t *result, xiaotai_http_request_t *http, xiaotai_http_response_t *response);
xiaotai_err_t platform_transfer_io_finish(platform_transfer_io_t *io, xiaotai_err_t error);
