// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_os_host.h"

#define COUNT 20000U
static xiaotai_eventlog_topic_t *s_topic;
static atomic_uint s_running;
static atomic_bool s_go;
static unsigned s_inject_after;
static uint32_t s_inject_values[XIAOTAI_EVENTLOG_TOPIC_VALUES];
static bool s_external;
static const char *const fields[] = {
    "n0", "n1", "n2", "n3", "n4", "n5", "n6", "n7", "n8",
    "n9", "n10", "n11", "n12", "n13", "n14", "n15", "n16",
};
static const xiaotai_eventlog_topic_desc_t descriptor = {
    .source = "play", .name = "pacing.long", .fields = fields, .count = XIAOTAI_EVENTLOG_TOPIC_VALUES,
};

int64_t __real_xiaotai_time_us(void);
bool __wrap_xiaotai_mem_has_external(void) { return s_external; }

int64_t __wrap_xiaotai_time_us(void)
{
    if (s_inject_after && --s_inject_after == 0) xiaotai_eventlog_post(s_topic, s_inject_values);
    return __real_xiaotai_time_us();
}

static void *producer(void *context)
{
    const uint32_t stride = *(const uint32_t *)context;
    while (!atomic_load(&s_go)) {
    }
    for (uint32_t i = 0; i < COUNT; ++i) {
        uint32_t values[XIAOTAI_EVENTLOG_TOPIC_VALUES];
        for (size_t j = 0; j < XIAOTAI_EVENTLOG_TOPIC_VALUES; ++j) values[j] = i * stride + (uint32_t)j;
        xiaotai_eventlog_post(s_topic, values);
    }
    atomic_fetch_sub(&s_running, 1U);
    return NULL;
}

static uint32_t reports(void)
{
    xiaotai_eventlog_stats_t stats;
    xiaotai_eventlog_stats(&stats);
    return stats.reports;
}

static size_t topic_bytes(void)
{
    xiaotai_mem_host_ledger_entry_t entries[8];
    size_t count = 0;
    assert(xiaotai_mem_host_ledger_snapshot(entries, 8, &count));
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].kind == XIAOTAI_MEM_BULK) bytes += entries[i].bytes;
    }
    return bytes;
}

static void test_mailbox_size(bool external)
{
    s_external = external;
    static const char *const short_fields[] = {"one"};
    static const xiaotai_eventlog_topic_desc_t short_desc = {
        .source = "play", .name = "pacing.short", .fields = short_fields, .count = 1,
    };
    assert(xiaotai_eventlog_init(&(xiaotai_eventlog_config_t){.reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER})
           == XIAOTAI_OK);
    size_t init_bytes = topic_bytes();
    xiaotai_eventlog_topic_t *full, *brief;
    assert(xiaotai_eventlog_topic_register(&descriptor, &full) == XIAOTAI_OK);
    size_t full_bytes = topic_bytes() - init_bytes;
    assert(xiaotai_eventlog_topic_register(&short_desc, &brief) == XIAOTAI_OK);
    size_t short_bytes = topic_bytes() - init_bytes - full_bytes;
    printf("external=%u; short=%zu B; full=%zu B\n", (unsigned)external, short_bytes, full_bytes);
    assert(full_bytes - short_bytes ==
           (external ? 0U : (XIAOTAI_EVENTLOG_TOPIC_VALUES - 1U) * sizeof(uint32_t)));
    const uint32_t value = 42;
    xiaotai_eventlog_post(brief, &value);
    xiaotai_eventlog_topic_snapshot_t sample;
    assert(xiaotai_eventlog_topic_read(1, &sample) && sample.desc == &short_desc && sample.posts == 1);
    assert(sample.values[0] == value);
    for (size_t i = 1; i < XIAOTAI_EVENTLOG_TOPIC_VALUES; ++i) assert(sample.values[i] == 0);
    xiaotai_eventlog_reset();
    assert(topic_bytes() == 0);
}

int main(void)
{
    xiaotai_mem_host_ledger_start();
    const int64_t baseline = xiaotai_mem_host_outstanding();
    assert(xiaotai_eventlog_topic_register(&descriptor, &s_topic) == XIAOTAI_ERR_INVALID_STATE && !s_topic);
    xiaotai_eventlog_post(s_topic, (uint32_t[1]){0});
    assert(xiaotai_eventlog_init(&(xiaotai_eventlog_config_t){.reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER})
           == XIAOTAI_OK);
    assert(xiaotai_eventlog_topic_register(NULL, &s_topic) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_eventlog_topic_register(&(xiaotai_eventlog_topic_desc_t){
               .name = "bad", .source = "play", .fields = fields, .count = 18}, &s_topic) == XIAOTAI_ERR_INVALID_ARG);
    for (unsigned allocation = 1; allocation <= 2; ++allocation) {
        const int64_t n = xiaotai_mem_host_outstanding();
        xiaotai_mem_host_fail_nth(allocation);
        assert(xiaotai_eventlog_topic_register(&descriptor, &s_topic) == XIAOTAI_ERR_NO_MEM && !s_topic);
        assert(xiaotai_eventlog_topic_count() == 0 && xiaotai_mem_host_outstanding() == n);
    }
    assert(xiaotai_eventlog_topic_register(&descriptor, &s_topic) == XIAOTAI_OK);
    xiaotai_eventlog_topic_t *duplicate = s_topic;
    assert(xiaotai_eventlog_topic_register(&descriptor, &duplicate) == XIAOTAI_ERR_INVALID_STATE && !duplicate);
    xiaotai_eventlog_flush();
    assert(reports() == 0);

    pthread_t a, b;
    const uint32_t one = 1, two = 2;
    atomic_store(&s_running, 2);
    assert(pthread_create(&a, NULL, producer, (void *)&one) == 0);
    assert(pthread_create(&b, NULL, producer, (void *)&two) == 0);
    atomic_store(&s_go, true);
    xiaotai_eventlog_topic_snapshot_t last;
    while (atomic_load(&s_running)) {
        if (xiaotai_eventlog_topic_read(0, &last) && last.posts) {
            for (size_t j = 0; j < XIAOTAI_EVENTLOG_TOPIC_VALUES; ++j) {
                assert(last.values[j] == last.values[0] + j);
            }
        }
    }
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    assert(xiaotai_eventlog_topic_read(0, &last) && last.posts + last.missed == COUNT * 2 && last.desc == &descriptor);
    assert(!xiaotai_eventlog_topic_read(1, &last));
    xiaotai_eventlog_flush();
    assert(reports() == 1);
    uint32_t newest[XIAOTAI_EVENTLOG_TOPIC_VALUES];
    for (size_t j = 0; j < XIAOTAI_EVENTLOG_TOPIC_VALUES; ++j) newest[j] = UINT32_MAX - (uint32_t)j;
    xiaotai_eventlog_post(s_topic, newest);
    xiaotai_eventlog_flush();
    assert(reports() == 1);
    xiaotai_os_host_advance_us((int64_t)XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS * 1000);
    xiaotai_eventlog_flush();
    assert(reports() == 2);
    xiaotai_eventlog_flush();
    assert(reports() == 2 && xiaotai_eventlog_topic_read(0, &last));
    assert(last.values[0] == UINT32_MAX && last.values[16] == UINT32_MAX - 16U);
    for (size_t j = 0; j < XIAOTAI_EVENTLOG_TOPIC_VALUES; ++j) s_inject_values[j] = 101U + (uint32_t)j;
    xiaotai_eventlog_post(s_topic, newest);
    xiaotai_os_host_advance_us((int64_t)XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS * 1000);
    /* Flush samples time at entry, then again after taking its snapshot.
     * A new post in that gap must still be visible on the next interval. */
    s_inject_after = 2;
    xiaotai_eventlog_flush();
    assert(s_inject_after == 0 && reports() == 3);
    xiaotai_eventlog_flush();
    assert(reports() == 3);
    xiaotai_os_host_advance_us((int64_t)XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS * 1000);
    xiaotai_eventlog_flush();
    assert(reports() == 4 && xiaotai_eventlog_topic_read(0, &last));
    assert(last.values[0] == 101U && last.values[16] == 117U);
    xiaotai_eventlog_reset();
    assert(xiaotai_mem_host_outstanding() == baseline && xiaotai_eventlog_topic_count() == 0);
    test_mailbox_size(false);
    test_mailbox_size(true);
    assert(xiaotai_mem_host_outstanding() == baseline);
    return 0;
}
