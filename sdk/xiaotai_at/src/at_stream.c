// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/os.h"

/* Stream callbacks, including their writes, all execute on the one console
 * task. The generation refuses a saved reply after next() has returned. */
static struct {
    const xiaotai_at_io_t *io;
    int64_t progress_us;
    uint32_t timeout_ms;
    at_result_t result;
} s_writer;
static uintptr_t s_generation;
static _Atomic uintptr_t s_live;

static void stream_write(void *ctx, const char *text);

static bool current(const xiaotai_reply_t *reply)
{
    reply = at_reply_original(reply);
    return reply && reply->write == stream_write && reply->ctx && (uintptr_t)reply->ctx == atomic_load(&s_live);
}

static bool can_write(void)
{
    if (!s_writer.result.error && xiaotai_time_us() - s_writer.progress_us >= (int64_t)s_writer.timeout_ms * 1000) {
        s_writer.result = (at_result_t){XIAOTAI_ERR_TIMEOUT, "STREAM"};
    }
    return !s_writer.result.error;
}

static bool send_record(char *record, size_t length)
{
    if (!can_write()) return false;
    memcpy(record + length, "\r\n", 2);
    if (!at_write_record(s_writer.io, record, length + 2)) {
        s_writer.result = (at_result_t){XIAOTAI_FAIL, "OUTPUT"};
        return false;
    }
    /* Successful transport return is the end of the no-progress interval. */
    if (!can_write()) return false;
    s_writer.progress_us = xiaotai_time_us();
    return true;
}

void at_stream_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason)
{
    if (error && current(reply) && can_write()) {
        s_writer.result = (at_result_t){error, at_valid_reason(reason) ? reason : NULL};
    }
}

void at_stream_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault)
{
    if (current(reply) && can_write()) {
        s_writer.result = fault == XIAOTAI_AT_OUTPUT_TOO_LONG
            ? (at_result_t){XIAOTAI_ERR_INVALID_SIZE, "OUTPUT_TOO_LONG"}
            : (at_result_t){XIAOTAI_FAIL, "OUTPUT"};
    }
}

static void stream_write(void *ctx, const char *text)
{
    if (!text || (uintptr_t)ctx != atomic_load(&s_live) || !ctx) return;
    char record[XIAOTAI_AT_OUT_LINE_MAX + 2];
    for (; *text && can_write();) {
        const char *end = strchr(text, '\n');
        const size_t size = end ? (size_t)(end - text) : strlen(text);
        if (size > XIAOTAI_AT_OUT_LINE_MAX) {
            s_writer.result = (at_result_t){XIAOTAI_ERR_INVALID_SIZE, "OUTPUT_TOO_LONG"};
        } else if (memchr(text, '\r', size) || (size == 2 && !memcmp(text, "OK", 2)) ||
                   (size >= 6 && !memcmp(text, "ERROR:", 6))) {
            s_writer.result = (at_result_t){XIAOTAI_FAIL, "OUTPUT"};
        } else if (size) {
            memcpy(record, text, size);
            if (!send_record(record, size)) break;
        }
        if (!end) break;
        text = end + 1;
    }
}

static bool valid_blob(const char *name, const char *header, const uint8_t *data, size_t size)
{
    if (!name || !*name || !header || !data || !size) return false;
    /* Leave room for the longest chunk, a size_t index, and record syntax. */
    const size_t name_max = XIAOTAI_AT_OUT_LINE_MAX - 2 * XIAOTAI_AT_BLOB_CHUNK - 41;
    for (size_t i = 0; name[i]; ++i) {
        const char c = name[i];
        if (i == name_max || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    for (size_t i = 0; header[i]; ++i) {
        const unsigned char c = (unsigned char)header[i];
        if (i == XIAOTAI_AT_BLOB_HEADER_MAX || c < 32 || c > 126) return false;
    }
    return true;
}

static xiaotai_err_t write_blob(const char *name, const char *header, const uint8_t *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < size; ++i) {
        if (!(i & 4095U) && !can_write()) return s_writer.result.error;
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    }
    char line[XIAOTAI_AT_OUT_LINE_MAX + 2];
    int n = snprintf(line, sizeof(line), "+%s: %s%sbytes=%zu fnv=%08" PRIx32, name, header, *header ? " " : "",
                     size, hash);
    if (!send_record(line, (size_t)n)) return s_writer.result.error;
    size_t chunks = 0;
    for (size_t offset = 0; offset < size;) {
        const size_t count = size - offset < XIAOTAI_AT_BLOB_CHUNK ? size - offset : XIAOTAI_AT_BLOB_CHUNK;
        n = snprintf(line, sizeof(line), "+%s-DATA: seq=%zu hex=", name, chunks++);
        const size_t hex = xiaotai_at_hex_encode(data + offset, count, line + n, sizeof(line) - (size_t)n);
        if (!send_record(line, (size_t)n + hex)) return s_writer.result.error;
        offset += count;
    }
    n = snprintf(line, sizeof(line), "+%s-END: chunks=%zu fnv=%08" PRIx32, name, chunks, hash);
    return send_record(line, (size_t)n) ? XIAOTAI_OK : s_writer.result.error;
}

xiaotai_err_t xiaotai_at_stream(const xiaotai_reply_t *reply, const xiaotai_at_stream_t *stream)
{
    if (!stream || !stream->next) return XIAOTAI_ERR_INVALID_ARG;
    const at_long_output_t output = {.kind = AT_LONG_STREAM, .as.stream = *stream};
    return at_reply_handover(reply, &output);
}

xiaotai_err_t xiaotai_at_blob(const xiaotai_reply_t *reply, const xiaotai_at_blob_t *blob)
{
    if (!blob || !valid_blob(blob->name, blob->header, blob->data, blob->size)) return XIAOTAI_ERR_INVALID_ARG;
    at_long_output_t output = {.kind = AT_LONG_BLOB, .as.blob = *blob};
    output.as.blob.header = NULL; /* Point at the copied header only when writing. */
    strcpy(output.header, blob->header);
    return at_reply_handover(reply, &output);
}

xiaotai_err_t xiaotai_at_blob_write(const xiaotai_reply_t *reply, const char *name, const char *header,
                                  const uint8_t *data, size_t size)
{
    if (!current(reply)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!valid_blob(name, header, data, size)) return XIAOTAI_ERR_INVALID_ARG;
    return write_blob(name, header, data, size);
}

xiaotai_err_t xiaotai_at_stream_result(const xiaotai_reply_t *reply)
{
    if (!current(reply)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return can_write() ? XIAOTAI_OK : s_writer.result.error;
}

at_result_t at_stream_finish(const xiaotai_at_io_t *io, const at_long_output_t *output,
                            at_result_t result, uint32_t timeout_ms)
{
    if (output->kind == AT_LONG_NONE) return result;
    const bool emit = !result.error;
    if (emit) {
        if (io->bulk) io->bulk(io->self, true);
        s_writer.io = io;
        s_writer.timeout_ms = timeout_ms;
        s_writer.progress_us = xiaotai_time_us();
        s_writer.result = (at_result_t){0};
        if (output->kind == AT_LONG_STREAM) {
            while (can_write()) {
                if (++s_generation == 0) ++s_generation;
                const xiaotai_reply_t reply = {.write = stream_write, .ctx = (void *)s_generation};
                atomic_store(&s_live, s_generation);
                const bool more = output->as.stream.next(output->as.stream.ctx, &reply);
                atomic_store(&s_live, 0);
                if (!can_write() || !more) break;
            }
        } else {
            const xiaotai_at_blob_t *blob = &output->as.blob;
            (void)write_blob(blob->name, output->header, blob->data, blob->size);
        }
        result = s_writer.result;
        if (!result.error) at_count(at_count_streams);
    }
    if (output->kind == AT_LONG_STREAM) {
        if (output->as.stream.release) output->as.stream.release(output->as.stream.ctx);
    } else if (output->as.blob.release) {
        output->as.blob.release(output->as.blob.ctx);
    }
    if (emit && io->bulk) io->bulk(io->self, false);
    return result;
}
