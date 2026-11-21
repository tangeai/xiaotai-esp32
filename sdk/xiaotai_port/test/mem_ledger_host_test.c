// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_mqtt_host.h"

static xiaotai_mem_host_ledger_entry_t rows[XIAOTAI_MEM_HOST_LEDGER_CAPACITY];
static void *overflow_blocks[XIAOTAI_MEM_HOST_LEDGER_CAPACITY + 1U];
static xiaotai_signal_t started, end;

static void worker(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&started);
    assert(xiaotai_signal_take(&end, 1000));
}

static void mqtt_event(void *ctx, const xiaotai_mqtt_event_t *event)
{
    (void)ctx;
    (void)event;
}

static size_t snapshot(void)
{
    size_t count = 0;
    assert(xiaotai_mem_host_ledger_snapshot(rows, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    return count;
}

int main(void)
{
    size_t count = 0;
    void *before = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 1);
    assert(before && !xiaotai_mem_host_ledger_snapshot(rows, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    xiaotai_mem_host_ledger_start();
    assert(snapshot() == 0);
    xiaotai_mem_free(before);

    void *a = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 21);
    void *b = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 2, 9);
    void *c = xiaotai_mem_aligned(XIAOTAI_MEM_DMA, 16, 26);
    assert(a && b && c && snapshot() == 3);
    const size_t bytes[] = {21, 18, 26};
    for (size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i)
        assert(rows[i].bytes == bytes[i] && (size_t)rows[i].kind == i && !rows[i].task &&
               !rows[i].fixture && !rows[i].name[0] && rows[i].site);
    assert(rows[0].site != rows[1].site && rows[1].site != rows[2].site);
    assert(!xiaotai_mem_host_ledger_snapshot(rows, 2, &count) && count == 3);
    xiaotai_mem_host_fail_nth(1);
    assert(!xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 777) && snapshot() == 3);
    xiaotai_mem_host_clear();
    assert(snapshot() == 3);
    xiaotai_mem_free(a);
    xiaotai_mem_free(b);
    xiaotai_mem_free(c);

    xiaotai_queue_t first, second;
    assert(xiaotai_queue_init(&first, 3, 32, XIAOTAI_MEM_BULK) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&second, 6, 16, XIAOTAI_MEM_BULK) == XIAOTAI_OK);
    assert(snapshot() == 2 && rows[0].bytes == 96 && rows[1].bytes == 96 && rows[0].site != rows[1].site);
    xiaotai_queue_deinit(&first);
    xiaotai_queue_deinit(&second);
    assert(snapshot() == 0);

    assert(xiaotai_signal_init(&started) == XIAOTAI_OK && xiaotai_signal_init(&end) == XIAOTAI_OK);
    xiaotai_task_t task;
    assert(xiaotai_task_create(&(xiaotai_task_config_t){
        .name = "ledger_probe", .entry = worker, .stack_bytes = 2048, .stack = XIAOTAI_MEM_BULK,
    }, &task) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, 1000) && snapshot() == 1);
    assert(rows[0].bytes == 2048 && rows[0].task && !rows[0].fixture && rows[0].kind == XIAOTAI_MEM_BULK &&
           rows[0].site && !strcmp(rows[0].name, "ledger_probe"));
    xiaotai_signal_give(&end);
    for (unsigned i = 0; i < 1000 && snapshot(); ++i) xiaotai_delay_ms(1);
    assert(snapshot() == 0);
    xiaotai_signal_deinit(&end);
    xiaotai_signal_deinit(&started);

    xiaotai_mqtt_t *client = NULL;
    assert(xiaotai_mqtt_create(&(xiaotai_mqtt_config_t){
        .uri = "mqtt://offline.test", .client_id = "ledger-probe", .handler = mqtt_event,
        .rx_buffer_bytes = 301, .tx_buffer_bytes = 302, .task_stack_bytes = 5678,
    }, &client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client, 1000) == XIAOTAI_OK);
    count = snapshot();
    size_t task_bytes = 0;
    unsigned fixture_records = 0, wire_buffers = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_mem_host_ledger_entry_t *row = &rows[i];
        assert(row->site);
        fixture_records += row->fixture;
        if (!row->fixture && row->kind == XIAOTAI_MEM_BULK && (row->bytes == 301 || row->bytes == 302))
            ++wire_buffers;
        if (row->task && !strcmp(row->name, "xiaotai_mqtt")) task_bytes += row->bytes;
    }
    assert(task_bytes == 5678 && fixture_records == 2 && wire_buffers == 2);
    assert(xiaotai_mqtt_stop(client) == XIAOTAI_OK && snapshot() + 3 == count);
    assert(xiaotai_mqtt_destroy(client) == XIAOTAI_OK && snapshot() == 0);

    for (size_t i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY + 1U; ++i) {
        overflow_blocks[i] = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, 1);
        assert(overflow_blocks[i]);
    }
    assert(!xiaotai_mem_host_ledger_snapshot(rows, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    assert(count == XIAOTAI_MEM_HOST_LEDGER_CAPACITY);
    for (size_t i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY + 1U; ++i)
        xiaotai_mem_free(overflow_blocks[i]);
    assert(!xiaotai_mem_host_ledger_snapshot(rows, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count) && count == 0);
    puts("PASS: dormant/live allocation classes, release, stack lifetime, MQTT, and sticky overflow");
    return 0;
}
