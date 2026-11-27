// SPDX-License-Identifier: MIT
#include "transfer_internal.h"
#include "signal_pool.h"

#define SIGNAL_TICK_US INT64_C(100000)

void xiaotai_platform_transfer_cancel(uint32_t id)
{
    if (!id || !platform.objects || !atomic_load(&platform.running)) return;
    platform_lock();
    for (size_t i = 0; i < platform.slot_count; ++i) {
        platform_slot_t *slot = &platform.slots[i];
        if (slot->id == id && slot->is_transfer && (slot->state == PLATFORM_QUEUED || slot->state == PLATFORM_RUNNING)) {
            slot->cancelled = true;
            if (slot == platform.io) xiaotai_http_cancel(platform.client);
            break;
        }
    }
    platform_unlock();
    platform_wake();
}

static xiaotai_err_t current(platform_transfer_io_t *io)
{
    const int64_t now = xiaotai_time_us();
    if (now >= io->next_signal_tick) {
        io->next_signal_tick = now + SIGNAL_TICK_US;
        /* Pump only nonblocking signaling work. The session step could
         * start another HTTP exchange on this worker and must wait. */
        (void)platform_signaling_step();
        (void)platform_signals_step();
        platform_events_flush();
    }
    platform_lock();
    const xiaotai_err_t error = io->slot->cancelled ? XIAOTAI_ERR_PLATFORM_CANCELLED :
        platform_slot_stale(io->slot) ? XIAOTAI_ERR_PLATFORM_STALE :
        !platform_remaining_ms(io->slot->deadline_us) ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    platform_unlock();
    return error;
}

static bool cancelled_before_io(void *ctx)
{
    const platform_transfer_io_t *io = ctx;
    platform_lock();
    const bool cancelled = io->slot->cancelled || platform_slot_stale(io->slot);
    platform_unlock();
    return cancelled;
}

static xiaotai_err_t stream_read(void *ctx, uint8_t *data, size_t capacity, size_t *size)
{
    platform_transfer_io_t *io = ctx;
    const xiaotai_err_t error = current(io);
    const xiaotai_platform_stream_t *stream = &io->slot->handler.transfer.stream;
    return error == XIAOTAI_OK ? stream->read(stream->ctx, data, capacity, size) : error;
}

static xiaotai_err_t stream_begin(platform_transfer_io_t *io)
{
    if (io->began) return XIAOTAI_OK;
    const xiaotai_err_t error = current(io);
    if (error != XIAOTAI_OK) return error;
    io->began = true;
    const xiaotai_platform_stream_t *stream = &io->slot->handler.transfer.stream;
    const xiaotai_http_capture_t *headers = io->captures;
    const xiaotai_platform_response_head_t head = {
        .http_status = io->response->status, .content_type = headers[0].value, .content_encoding = headers[1].value,
        .content_length = io->response->content_length, .content_type_repeated = headers[0].count > 1U,
        .content_encoding_repeated = headers[1].count > 1U, .truncated = headers[0].truncated || headers[1].truncated,
    };
    return stream->begin ? stream->begin(stream->ctx, &head) : XIAOTAI_OK;
}

static xiaotai_err_t stream_write(void *ctx, const uint8_t *data, size_t size)
{
    platform_transfer_io_t *io = ctx;
    xiaotai_err_t error = stream_begin(io);
    if (error == XIAOTAI_OK) error = current(io);
    const xiaotai_platform_stream_t *stream = &io->slot->handler.transfer.stream;
    if (error == XIAOTAI_OK && stream->write) {
        io->result->received += size; /* Includes a piece the consumer refuses. */
        error = stream->write(stream->ctx, data, size);
    }
    return error;
}

void platform_transfer_io_init(platform_transfer_io_t *io, platform_slot_t *slot,
    xiaotai_platform_transfer_result_t *result, xiaotai_http_request_t *http, xiaotai_http_response_t *response)
{
    io->slot = slot;
    io->result = result;
    io->response = response;
    io->next_signal_tick = xiaotai_time_us();
    io->captures[0] = (xiaotai_http_capture_t){.name = "Content-Type", .value = io->type, .capacity = sizeof(io->type)};
    io->captures[1] = (xiaotai_http_capture_t){.name = "Content-Encoding", .value = io->encoding,
                                              .capacity = sizeof(io->encoding)};
    http->source = slot->handler.transfer.source_size ? stream_read : NULL;
    http->source_ctx = io;
    http->source_size = slot->handler.transfer.source_size;
    http->io_timeout_ms = slot->handler.transfer.stall_ms;
    http->cancelled = cancelled_before_io;
    http->cancel_ctx = io;
    response->sink = stream_write;
    response->sink_ctx = io;
    response->sink_limit = slot->handler.transfer.response_max ? slot->handler.transfer.response_max : SIZE_MAX;
    response->captures = io->captures;
    response->capture_count = sizeof(io->captures) / sizeof(io->captures[0]);
}

xiaotai_err_t platform_transfer_io_finish(platform_transfer_io_t *io, xiaotai_err_t error)
{
    /* With no delivered piece, the port may still have completed a head
     * (empty response or body refused before the first piece). */
    if (io->response->status && (error == XIAOTAI_OK || error == XIAOTAI_ERR_INVALID_SIZE) && !io->began) {
        const xiaotai_err_t head = stream_begin(io);
        if (head != XIAOTAI_OK) return head;
    }
    return error == XIAOTAI_OK ? current(io) : error;
}
