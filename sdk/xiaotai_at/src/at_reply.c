// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/os.h"

static xiaotai_mutex_t s_lock;
static bool s_lock_ready;
static const xiaotai_at_io_t *s_io;
static char *s_buffer;
static size_t s_capacity, s_used;
static bool s_write_failed;
static at_result_t s_failure, s_fault;
static at_long_output_t s_long;

/* Framework copies the reply into its job and may keep it through a
 * timeout. The context is an opaque generation, never a stack pointer. */
static uintptr_t s_generation;
static _Atomic uintptr_t s_live;

xiaotai_err_t at_reply_open(size_t capacity)
{
    s_buffer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity);
    if (!s_buffer) return XIAOTAI_ERR_NO_MEM;
    if (!s_lock_ready) {
        const xiaotai_err_t err = xiaotai_mutex_init(&s_lock);
        if (err != XIAOTAI_OK) {
            xiaotai_mem_free(s_buffer);
            s_buffer = NULL;
            return err;
        }
        s_lock_ready = true;
    }
    s_capacity = capacity;
    return XIAOTAI_OK;
}

void at_reply_close(void)
{
    if (!s_buffer) return;
    /* A timed-out owner might have passed the epoch check before reset.
     * Keep the static mutex alive so it can still lock and reject that epoch. */
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    xiaotai_mem_free(s_buffer);
    s_buffer = NULL;
    xiaotai_mutex_unlock(&s_lock);
}

/* The lock serializes whole records. A console timeout revokes live before
 * taking the lock, so the writer finishes at most its in-flight record. */
static void flush_locked(uintptr_t live)
{
    for (size_t offset = 0; !s_write_failed && offset < s_used;) {
        if (live && live != atomic_load(&s_live)) break;
        const char *start = s_buffer + offset;
        const char *end = memchr(start, '\n', s_used - offset);
        const size_t length = (size_t)(end - start) + 1;
        s_write_failed = !at_write_record(s_io, start, length);
        offset += length;
    }
    s_used = 0;
}

static void record_fault(xiaotai_at_output_fault_t fault)
{
    if (s_fault.error) return;
    s_fault = fault == XIAOTAI_AT_OUTPUT_TOO_LONG
        ? (at_result_t){XIAOTAI_ERR_INVALID_SIZE, "OUTPUT_TOO_LONG"}
        : (at_result_t){XIAOTAI_FAIL, "OUTPUT"};
}

static void write_lines(void *ctx, const char *text)
{
    if (!text) return;
    const uintptr_t generation = (uintptr_t)ctx;
    while (*text) {
        const char *end = strchr(text, '\n');
        const size_t length = end ? (size_t)(end - text) : strlen(text);
        if (length) {
            if (generation != atomic_load(&s_live)) {
                at_count(at_count_late_dropped);
            } else {
                (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
                if (generation != atomic_load(&s_live)) {
                    at_count(at_count_late_dropped);
                } else if (length > XIAOTAI_AT_OUT_LINE_MAX) {
                    record_fault(XIAOTAI_AT_OUTPUT_TOO_LONG);
                } else if (memchr(text, '\r', length) || (length == 2 && !memcmp(text, "OK", 2)) ||
                           (length >= 6 && !memcmp(text, "ERROR:", 6))) {
                    record_fault(XIAOTAI_AT_OUTPUT_MALFORMED);
                } else if (!s_write_failed) {
                    if (s_used + length + 2 > s_capacity) {
                        if (xiaotai_on_owner()) at_count(at_count_owner_flushes);
                        flush_locked(generation);
                    }
                    if (!s_write_failed && generation == atomic_load(&s_live)) {
                        memcpy(s_buffer + s_used, text, length);
                        memcpy(s_buffer + s_used + length, "\r\n", 2);
                        s_used += length + 2;
                    } else if (generation != atomic_load(&s_live)) {
                        at_count(at_count_late_dropped);
                    }
                }
                xiaotai_mutex_unlock(&s_lock);
            }
        }
        if (!end) break;
        text = end + 1;
    }
}

xiaotai_reply_t at_reply_begin(const xiaotai_at_io_t *io)
{
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    s_io = io;
    s_used = 0;
    s_write_failed = false;
    s_failure = s_fault = (at_result_t){0};
    s_long.kind = AT_LONG_NONE;
    if (++s_generation == 0) ++s_generation;
    atomic_store(&s_live, s_generation);
    xiaotai_mutex_unlock(&s_lock);
    return (xiaotai_reply_t){.write = write_lines, .ctx = (void *)s_generation};
}

at_result_t at_reply_finish(at_result_t result, bool timed_out, uint32_t stream_timeout_ms)
{
    atomic_store(&s_live, 0);
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    /* A live reply proves the current handler ran. Its BUSY is not the
     * busy status returned when command_run never acquired the old slot. */
    if (result.error == XIAOTAI_ERR_BUSY && s_used) result.reason = NULL;
    if (timed_out) s_used = 0;
    else flush_locked(0);
    if (s_write_failed) result = (at_result_t){XIAOTAI_FAIL, "OUTPUT"};
    else if (!timed_out && s_fault.error) result = s_fault;
    else if (!timed_out && s_failure.error && result.error == s_failure.error) result = s_failure;
    const at_long_output_t output = s_long;
    s_long.kind = AT_LONG_NONE;
    xiaotai_mutex_unlock(&s_lock);
    return at_stream_finish(s_io, &output, result, stream_timeout_ms);
}

static uintptr_t reply_generation(const xiaotai_reply_t *reply)
{
    reply = at_reply_original(reply);
    return reply && reply->write == write_lines ? (uintptr_t)reply->ctx : 0;
}

bool xiaotai_at_reply_is_console(const xiaotai_reply_t *reply)
{
    const uintptr_t generation = reply_generation(reply);
    return generation && generation == atomic_load(&s_live);
}

xiaotai_err_t at_reply_handover(const xiaotai_reply_t *reply, const at_long_output_t *output)
{
    const uintptr_t generation = reply_generation(reply);
    if (!generation) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (generation != atomic_load(&s_live)) return XIAOTAI_ERR_INVALID_STATE;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (generation == atomic_load(&s_live) && s_long.kind == AT_LONG_NONE) {
        s_long = *output;
        err = XIAOTAI_OK;
    }
    xiaotai_mutex_unlock(&s_lock);
    return err;
}

bool at_valid_reason(const char *reason)
{
    if (!reason || !*reason) return false;
    for (size_t i = 0; reason[i]; ++i) {
        const char c = reason[i];
        if (i == 31 || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

xiaotai_err_t xiaotai_at_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason)
{
    const uintptr_t generation = reply_generation(reply);
    if (error && generation && generation == atomic_load(&s_live) && at_valid_reason(reason)) {
        (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
        if (generation == atomic_load(&s_live)) s_failure = (at_result_t){error, reason};
        xiaotai_mutex_unlock(&s_lock);
    }
    if (!generation) at_stream_fail(reply, error, reason);
    return error;
}

void xiaotai_at_reply_output_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault)
{
    const uintptr_t generation = reply_generation(reply);
    if (generation && generation == atomic_load(&s_live)) {
        (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
        if (generation == atomic_load(&s_live)) record_fault(fault);
        xiaotai_mutex_unlock(&s_lock);
    }
    if (!generation) at_stream_fault(reply, fault);
}
