// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "internal.h"
#include "xiaotai_mem_host.h"

typedef struct {
    uint32_t sequence[16];
    size_t used;
    bool block;
    bool post;
} sink_t;

static xiaotai_signal_t s_entered, s_resume;
static unsigned s_creates;
static uint32_t s_expected_stack = 3072;
static bool s_has_external;

bool __wrap_xiaotai_mem_has_external(void) { return s_has_external; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    ++s_creates;
    assert(strcmp(config->name, "eventlog") == 0 && config->stack_bytes == s_expected_stack);
    assert(config->priority == 1 && config->stack == XIAOTAI_MEM_BULK);
    return __real_xiaotai_task_create(config, task);
}

static void sink(const xiaotai_eventlog_entry_t *entry, void *context)
{
    sink_t *records = context;
    assert(records->used < sizeof(records->sequence) / sizeof(records->sequence[0]));
    records->sequence[records->used++] = entry->sequence;
    if (records->block) {
        records->block = false;
        xiaotai_signal_give(&s_entered);
        assert(xiaotai_signal_take(&s_resume, 2000));
    }
    if (records->post) {
        records->post = false;
        xiaotai_eventlog_note("test", "during_flush", 0, 0);
    }
}

static void *flush(void *context)
{
    (void)context;
    xiaotai_eventlog_flush();
    return NULL;
}

int main(void)
{
    const int64_t baseline = xiaotai_mem_host_outstanding();
    sink_t before = {0}, late = {0};
    assert(xiaotai_eventlog_sink_register(sink, &before) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_eventlog_init(&(xiaotai_eventlog_config_t){.capacity = 3, .reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER})
           == XIAOTAI_OK);
    xiaotai_engine_eventlog_start();
    assert(s_creates == 0);
    assert(xiaotai_eventlog_sink_register(sink, &before) == XIAOTAI_OK);
    for (int i = 0; i < 5; ++i) xiaotai_eventlog_note("test", "before", 0, i);
    assert(xiaotai_eventlog_sink_register(sink, &late) == XIAOTAI_OK);
    late.post = true;
    xiaotai_eventlog_flush();
    assert(before.used == 3 && late.used == 3); /* event emitted by sink waits for next pass */
    for (uint32_t j = 0; j < 3; ++j) assert(before.sequence[j] == 3U + j && late.sequence[j] == 3U + j);
    xiaotai_eventlog_stats_t stats;
    xiaotai_eventlog_stats(&stats);
    assert(stats.sink_missed == 2 && stats.flushes == 1);
    xiaotai_eventlog_flush();
    assert(before.used == 4 && before.sequence[3] == 6 && late.used == 4 && late.sequence[3] == 6);

    xiaotai_eventlog_note("test", "concurrent", 0, 0);
    assert(xiaotai_signal_init(&s_entered) == XIAOTAI_OK && xiaotai_signal_init(&s_resume) == XIAOTAI_OK);
    before.block = true;
    pthread_t task;
    assert(pthread_create(&task, NULL, flush, NULL) == 0);
    assert(xiaotai_signal_take(&s_entered, 2000));
    xiaotai_eventlog_flush();
    xiaotai_eventlog_stats(&stats);
    assert(stats.flushes == 2 && late.used == 4);
    xiaotai_signal_give(&s_resume);
    assert(pthread_join(task, NULL) == 0);
    assert(before.used == 5 && late.used == 5 && before.sequence[4] == late.sequence[4]);
    xiaotai_signal_deinit(&s_resume);
    xiaotai_signal_deinit(&s_entered);

    /* Force public sequence wrap while keeping the sink's internal cursor. */
    eventlog_state.latest = UINT32_MAX - 1U;
    eventlog_state.kept = 0;
    eventlog_state.sinks[0].after = UINT32_MAX - 1U;
    eventlog_state.sinks[1].after = UINT32_MAX - 1U;
    xiaotai_eventlog_note("test", "last", 0, 0);
    xiaotai_eventlog_flush();
    xiaotai_eventlog_note("test", "first", 0, 0);
    xiaotai_eventlog_flush();
    assert(before.used == 7 && before.sequence[5] == UINT32_MAX && before.sequence[6] == 1);
    assert(late.used == 7 && late.sequence[6] == 1);
    xiaotai_eventlog_reset();
    assert(xiaotai_mem_host_outstanding() == baseline);

    assert(xiaotai_eventlog_init(&(xiaotai_eventlog_config_t){
               .reader_stack_bytes = 3072, .reader_period_ms = 60000}) == XIAOTAI_OK);
    sink_t reader = {0};
    xiaotai_eventlog_note("test", "before_reader", 0, 0);
    assert(xiaotai_eventlog_sink_register(sink, &reader) == XIAOTAI_OK);
    xiaotai_engine_eventlog_start();
    xiaotai_engine_eventlog_start();
    assert(s_creates == 1);
    /* Reset must wake a reader that could otherwise wait an entire minute. */
    xiaotai_eventlog_reset();
    assert(reader.used == 1 && reader.sequence[0] == 1 && xiaotai_mem_host_outstanding() == baseline);

    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    xiaotai_engine_eventlog_start();
    assert(s_creates == 1);
    xiaotai_eventlog_reset();

    s_has_external = true;
    s_expected_stack = 4096;
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    xiaotai_engine_eventlog_start();
    assert(s_creates == 2);
    xiaotai_eventlog_reset();
    assert(xiaotai_mem_host_outstanding() == baseline);
    return 0;
}
