// SPDX-License-Identifier: MIT
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/os.h"

static xiaotai_critical_t s_lock;
static atomic_bool s_initialized;
static uint8_t *s_ring;
static size_t s_capacity, s_used, s_read, s_write;
/* The single receipt is indexed by FIFO position, not by output counters.
 * No extra bytes are charged to an ordinary queued record. */
static xiaotai_signal_t s_signal;
static uint32_t s_enqueued, s_taken, s_ticket;
static int64_t s_receipt_due_us;
static bool s_waiting, s_complete;
static xiaotai_err_t s_outcome;

xiaotai_err_t at_notify_open(uint16_t bytes)
{
    uint8_t *ring = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (!ring) return XIAOTAI_ERR_NO_MEM;
    if (!atomic_load(&s_initialized)) {
        const xiaotai_err_t err = xiaotai_signal_init(&s_signal);
        if (err != XIAOTAI_OK) {
            xiaotai_mem_free(ring);
            return err;
        }
        xiaotai_critical_init(&s_lock);
        atomic_store(&s_initialized, true);
    }
    xiaotai_critical_enter(&s_lock);
    s_ring = ring;
    s_capacity = bytes;
    s_used = s_read = s_write = 0;
    xiaotai_critical_exit(&s_lock);
    return XIAOTAI_OK;
}

void at_notify_close(void)
{
    if (!atomic_load(&s_initialized)) return;
    xiaotai_critical_enter(&s_lock);
    uint8_t *ring = s_ring;
    s_ring = NULL;
    s_capacity = s_used = s_read = s_write = 0;
    s_taken = s_enqueued;
    const bool wake = s_waiting && !s_complete;
    if (wake) {
        s_complete = true;
        s_outcome = XIAOTAI_ERR_INVALID_STATE;
    }
    xiaotai_critical_exit(&s_lock);
    if (wake) xiaotai_signal_give(&s_signal);
    xiaotai_mem_free(ring);
}

static void copy_in(const void *data, size_t bytes)
{
    const size_t end = s_capacity - s_write;
    const size_t first = bytes < end ? bytes : end;
    memcpy(s_ring + s_write, data, first);
    memcpy(s_ring, (const uint8_t *)data + first, bytes - first);
    s_write = (s_write + bytes) % s_capacity;
}

static void copy_out(void *data, size_t bytes)
{
    const size_t end = s_capacity - s_read;
    const size_t first = bytes < end ? bytes : end;
    memcpy(data, s_ring + s_read, first);
    memcpy((uint8_t *)data + first, s_ring, bytes - first);
    s_read = (s_read + bytes) % s_capacity;
}

static bool valid_data_line(const char *line, size_t length)
{
    if (length < 4 || line[0] != '+') return false;
    size_t i = 1;
    for (; i < length && line[i] != ':'; ++i) {
        const char c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    if (i == 1 || i + 1 >= length || line[i + 1] != ' ') return false;
    for (i += 2; i < length; ++i) {
        const unsigned char c = (unsigned char)line[i];
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static xiaotai_err_t enqueue(const char *line, uint32_t *ticket, int64_t due_us)
{
    if (!at_console_has_source()) return XIAOTAI_ERR_INVALID_STATE;
    if (!line) return XIAOTAI_ERR_INVALID_ARG;
    const size_t length = strnlen(line, XIAOTAI_AT_NOTIFY_MAX + 1);
    if (length > XIAOTAI_AT_NOTIFY_MAX) {
        at_count(at_count_notify_dropped);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (!valid_data_line(line, length)) return XIAOTAI_ERR_INVALID_ARG;
    const size_t needed = length + sizeof(uint16_t);
    const uint16_t len = (uint16_t)length;
    xiaotai_critical_enter(&s_lock);
    xiaotai_err_t err = XIAOTAI_OK;
    if (!s_ring) err = XIAOTAI_ERR_INVALID_STATE;
    else if (ticket && s_waiting) err = XIAOTAI_ERR_BUSY;
    else if (needed > s_capacity) err = XIAOTAI_ERR_INVALID_SIZE;
    else if (needed > s_capacity - s_used) err = XIAOTAI_ERR_NO_MEM;
    else {
        copy_in(&len, sizeof(len));
        copy_in(line, len);
        s_used += needed;
        ++s_enqueued;
        if (ticket) {
            *ticket = s_ticket = s_enqueued;
            s_receipt_due_us = due_us;
            s_waiting = true;
            s_complete = false;
        }
    }
    xiaotai_critical_exit(&s_lock);
    if (err == XIAOTAI_ERR_INVALID_SIZE || err == XIAOTAI_ERR_NO_MEM) at_count(at_count_notify_dropped);
    return err;
}

xiaotai_err_t xiaotai_at_notify(const char *line) { return enqueue(line, NULL, 0); }

xiaotai_err_t xiaotai_at_notifyf(const char *format, ...)
{
    if (!at_console_has_source()) return XIAOTAI_ERR_INVALID_STATE;
    if (!format) return XIAOTAI_ERR_INVALID_ARG;
    char line[XIAOTAI_AT_NOTIFY_MAX + 1];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) return XIAOTAI_ERR_INVALID_ARG;
    if ((size_t)length > XIAOTAI_AT_NOTIFY_MAX) {
        at_count(at_count_notify_dropped);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    /* Reject an embedded NUL produced with %c. */
    if (memchr(line, 0, (size_t)length)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_at_notify(line);
}

size_t at_notify_queued(void)
{
    xiaotai_critical_enter(&s_lock);
    const size_t used = s_used;
    xiaotai_critical_exit(&s_lock);
    return used;
}

size_t at_notify_take(char record[XIAOTAI_AT_NOTIFY_MAX + 2], uint32_t *ticket)
{
    uint16_t length = 0;
    xiaotai_critical_enter(&s_lock);
    if (s_used) {
        copy_out(&length, sizeof(length));
        copy_out(record, length);
        s_used -= length + sizeof(length);
        *ticket = ++s_taken;
    }
    xiaotai_critical_exit(&s_lock);
    if (length) memcpy(record + length, "\r\n", 2);
    return length;
}

void at_notify_delivered(uint32_t ticket, bool delivered)
{
    xiaotai_critical_enter(&s_lock);
    const bool wake = s_waiting && !s_complete && ticket == s_ticket;
    if (wake) {
        s_complete = true;
        s_outcome = xiaotai_time_us() >= s_receipt_due_us ? XIAOTAI_ERR_TIMEOUT :
                    delivered ? XIAOTAI_OK : XIAOTAI_FAIL;
    }
    xiaotai_critical_exit(&s_lock);
    if (wake) xiaotai_signal_give(&s_signal);
}

xiaotai_err_t xiaotai_at_notify_wait(const char *line, uint32_t timeout_ms)
{
    if (!timeout_ms || timeout_ms > 5000) return XIAOTAI_ERR_INVALID_ARG;
    if (xiaotai_on_owner() || xiaotai_in_isr()) return XIAOTAI_ERR_INVALID_STATE;
    const int64_t now = xiaotai_time_us();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;
    const int64_t due = now > INT64_MAX - timeout_us ? INT64_MAX : now + timeout_us;
    uint32_t ticket;
    const xiaotai_err_t err = enqueue(line, &ticket, due);
    if (err != XIAOTAI_OK) return err;
    for (;;) {
        const int64_t remaining = due - xiaotai_time_us();
        xiaotai_critical_enter(&s_lock);
        if (s_ticket != ticket || !s_waiting) {
            xiaotai_critical_exit(&s_lock);
            return XIAOTAI_ERR_INVALID_STATE;
        }
        if (s_complete || remaining <= 0) {
            const xiaotai_err_t result = s_complete ? s_outcome : XIAOTAI_ERR_TIMEOUT;
            s_waiting = false;
            xiaotai_critical_exit(&s_lock);
            return result;
        }
        xiaotai_critical_exit(&s_lock);
        /* A give from an earlier timed-out wait cannot complete this ticket. */
        (void)xiaotai_signal_take(&s_signal, (uint32_t)((remaining + 999) / 1000));
    }
}
