// SPDX-License-Identifier: MIT
#include "console_internal.h"
#include "xiaotai/port/os.h"
#include <stdatomic.h>

static atomic_bool s_opened;
static atomic_uint_least32_t s_rx, s_overflows, s_tx, s_timeouts, s_dropped;
static bool s_rx_cr; /* One reader. */
static _Thread_local bool s_record, s_tx_cr;

static void count(atomic_uint_least32_t *counter, size_t amount)
{
    uint_least32_t old = atomic_load_explicit(counter, memory_order_relaxed);
    for (;;) {
        const uint_least32_t next = amount > UINT32_MAX - old ? UINT32_MAX : old + amount;
        if (atomic_compare_exchange_weak_explicit(counter, &old, next, memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

void console_note_rx_overflow(size_t bytes) { count(&s_overflows, bytes); }

xiaotai_err_t xiaotai_console_open(const xiaotai_console_config_t *config)
{
    if (atomic_load(&s_opened)) return XIAOTAI_OK;
    const size_t capacity = config && config->rx_buffer_bytes ? config->rx_buffer_bytes : CONSOLE_RX_DEFAULT;
    xiaotai_err_t err = console_io_open(capacity);
    if (err == XIAOTAI_OK) atomic_store(&s_opened, true);
    return err;
}

size_t xiaotai_console_read(char *data, size_t capacity, uint32_t timeout_ms)
{
    if (!data || !capacity || !atomic_load(&s_opened)) return 0;
    const int64_t began = xiaotai_time_us();
    const int64_t budget = (int64_t)timeout_ms * 1000;
    uint32_t wait_ms = timeout_ms;
    for (;;) {
        size_t received = console_io_read(data, capacity, wait_ms);
        if (!received) return 0;
        count(&s_rx, received);
        size_t kept = 0;
        for (size_t i = 0; i < received; ++i) {
            const char byte = data[i];
            if (!(byte == '\n' && s_rx_cr)) data[kept++] = byte == '\r' ? '\n' : byte;
            s_rx_cr = byte == '\r';
        }
        if (kept) return kept;
        const int64_t left = budget - (xiaotai_time_us() - began);
        /* If the entire read was a folded LF, also check already queued bytes. */
        wait_ms = left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
    }
}

void xiaotai_console_record_begin(void)
{
    if (s_record) return;
    console_io_lock();
    s_record = true;
    s_tx_cr = false;
}

void xiaotai_console_record_end(void)
{
    if (!s_record) return;
    s_tx_cr = s_record = false;
    console_io_unlock();
}

static void drop_tail(const char *data, size_t size, bool prev_cr)
{
    count(&s_dropped, size);
    size_t added = 0;
    for (size_t i = 0; i < size; ++i) {
        added += data[i] == '\n' && !prev_cr;
        prev_cr = data[i] == '\r';
    }
    count(&s_dropped, added);
}

xiaotai_err_t xiaotai_console_write(const char *data, size_t size)
{
    if (!atomic_load(&s_opened)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!data && size) return XIAOTAI_ERR_INVALID_ARG;
    if (!size) return XIAOTAI_OK;
    if (!s_record) console_io_lock();
    bool prev_cr = s_record && s_tx_cr;
    xiaotai_err_t err = console_io_connected() ? console_io_flush() : XIAOTAI_FAIL;
    char wire[CONSOLE_TX_CHUNK];
    size_t offset = 0;
    while (offset < size && err == XIAOTAI_OK) {
        size_t used = 0;
        while (offset < size && used < sizeof(wire)) {
            const char byte = data[offset];
            if (byte == '\n' && !prev_cr) {
                if (sizeof(wire) - used < 2) break;
                wire[used++] = '\r';
            }
            wire[used++] = byte;
            ++offset;
            prev_cr = byte == '\r';
        }
        size_t accepted = 0;
        err = console_io_write(wire, used, &accepted);
        count(&s_tx, accepted);
        if (accepted < used) {
            count(&s_dropped, used - accepted);
            if (err == XIAOTAI_OK) err = XIAOTAI_ERR_TIMEOUT;
        }
    }
    if (err != XIAOTAI_OK) {
        if (err == XIAOTAI_ERR_TIMEOUT) count(&s_timeouts, 1);
        drop_tail(data + offset, size - offset, prev_cr);
    }
    if (s_record) s_tx_cr = err == XIAOTAI_OK && prev_cr;
    else console_io_unlock();
    return err;
}

void xiaotai_console_stats(xiaotai_console_stats_t *stats)
{
    if (!stats) return;
    *stats = (xiaotai_console_stats_t){
        .rx_bytes = atomic_load(&s_rx), .rx_overflows = atomic_load(&s_overflows),
        .tx_bytes = atomic_load(&s_tx), .tx_timeouts = atomic_load(&s_timeouts),
        .tx_dropped = atomic_load(&s_dropped), .connected = atomic_load(&s_opened) && console_io_connected(),
    };
}
