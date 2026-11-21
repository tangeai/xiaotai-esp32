// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_memtrace/test_heap_failure.c
#define XIAOTAI_MEM_DIAG_FIXTURE 1
#include "../esp-idf/mem_diag.c"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int registrations, registration_result;
static atomic_int task_reads, time_reads;
static _Thread_local bool in_interrupt, cache;
static _Thread_local int64_t timestamp;
static esp_alloc_failed_hook_t hook;
static _Thread_local const char *task_name;

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t cb)
{
    ++registrations;
    if (registration_result == ESP_OK) hook = cb;
    return registration_result;
}

int64_t esp_timer_get_time(void) { ++time_reads; return timestamp; }
bool spi_flash_cache_enabled(void) { return cache; }
bool xPortInIsrContext(void) { return in_interrupt; }
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    assert(!in_interrupt && cache);
    ++task_reads;
    return task_name ? (TaskHandle_t)task_name : NULL;
}
const char *pcTaskGetName(TaskHandle_t task) { return task; }

static bool fail_alloc, fail_detach, trace_active;
static heap_trace_record_t *active_buffer;
static size_t records_capacity, allocs, frees, arms;
void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    assert(kind == XIAOTAI_MEM_INTERNAL);
    if (fail_alloc) return NULL;
    ++allocs;
    return calloc(count, size);
}
void xiaotai_mem_free(void *p)
{
    assert(p != active_buffer);
    if (p) ++frees;
    free(p);
}
esp_err_t heap_trace_init_standalone(heap_trace_record_t *buffer, size_t capacity)
{
    assert(!trace_active && buffer && capacity);
    if (capacity == 1 && fail_detach) return XIAOTAI_FAIL;
    active_buffer = buffer;
    records_capacity = capacity;
    return ESP_OK;
}
esp_err_t heap_trace_start(heap_trace_mode_t mode)
{
    assert(!trace_active && mode == HEAP_TRACE_LEAKS && active_buffer);
    trace_active = true;
    ++arms;
    return ESP_OK;
}
esp_err_t heap_trace_stop(void)
{
    assert(trace_active);
    trace_active = false;
    return ESP_OK;
}
esp_err_t heap_trace_summary(heap_trace_summary_t *summary)
{
    assert(!trace_active && records_capacity > 1);
    *summary = (heap_trace_summary_t){1, records_capacity, 10, false};
    return ESP_OK;
}
esp_err_t heap_trace_get(size_t index, heap_trace_record_t *rec)
{
    assert(!trace_active && index == 0);
    *rec = (heap_trace_record_t){(void *)0x101, 64, {(void *)0x202, (void *)0x303}};
    return ESP_OK;
}

static void failure_record(void)
{
    xiaotai_mem_failure_t failure = {0};
    xiaotai_mem_failure(&failure);
    assert(!failure.has_first && !failure.installed && !failure.failures);
#if CONFIG_ESP_TIMER_IN_IRAM
    registration_result = XIAOTAI_FAIL;
    assert(install_observer() == XIAOTAI_FAIL && !hook && registrations == 1);
    registration_result = XIAOTAI_OK;
    assert(install_observer() == XIAOTAI_FAIL && registrations == 1);
    s_install_attempted = false; /* Independent boot. */
    assert(install_observer() == XIAOTAI_OK && registrations == 2 && hook);
    assert(install_observer() == XIAOTAI_OK && registrations == 2);

    static const char long_name[] = "function_with_a_long_test_label_more_than_thirty_two";
    in_interrupt = true;
    cache = false;
    timestamp = 1234;
    hook(73, 0x45, long_name);
    in_interrupt = false;
    cache = true;
    timestamp = 5678;
    task_name = "later task";
    hook(999, 0x76, "second");
    xiaotai_mem_failure(&failure);
    assert(failure.installed && failure.error == XIAOTAI_OK && failure.failures == 2 && failure.has_first);
    assert(failure.in_isr && !failure.cache_enabled && failure.first_at_us == 1234 && task_reads == 0);
    assert(failure.requested_size == 73 && failure.platform_caps == 0x45);
    assert(failure.function_truncated && strlen(failure.function) == XIAOTAI_MEM_NAME_MAX - 1);
    assert(!memcmp(failure.function, long_name, XIAOTAI_MEM_NAME_MAX - 1) && !failure.task[0]);
    s_first.failures = UINT32_MAX - 1;
    hook(2, 3, "again");
    hook(2, 3, "again");
    xiaotai_mem_failure(&failure);
    assert(failure.failures == UINT32_MAX && failure.requested_size == 73);
    xiaotai_mem_failure(&failure);
    assert(failure.failures == UINT32_MAX && failure.first_at_us == 1234);

    s_first = (xiaotai_mem_failure_t){.installed = true};
    task_name = "long_diagnostic_task_name";
    hook(4, 5, "ok");
    xiaotai_mem_failure(&failure);
    assert(task_reads == 1 && failure.task_truncated && strlen(failure.task) == sizeof(failure.task) - 1);
    assert(!failure.in_isr && failure.cache_enabled && !failure.function_truncated && !strcmp(failure.function, "ok"));
#else
    assert(install_observer() == XIAOTAI_ERR_NOT_SUPPORTED && !registrations && !hook);
    assert(install_observer() == XIAOTAI_ERR_NOT_SUPPORTED && !registrations);
    xiaotai_mem_failure(&failure);
    assert(!failure.installed && failure.error == XIAOTAI_ERR_NOT_SUPPORTED && !failure.failures);
#endif
}

#if CONFIG_ESP_TIMER_IN_IRAM
enum { WRITERS = 8, FAILURES_PER_WRITER = 2500 };
static atomic_bool release_writers;
static atomic_uint writers_done;
static char names[WRITERS][8] = {"first0", "first1", "first2", "first3", "first4", "first5", "first6", "first7"};

static void *failing_writer(void *context)
{
    const unsigned i = *(const unsigned *)context;
    cache = (i & 2U) != 0;
    in_interrupt = (i & 1U) != 0;
    timestamp = 1000 + i;
    task_name = names[i];
    while (!atomic_load(&release_writers)) sched_yield();
    for (unsigned j = 0; j < FAILURES_PER_WRITER; ++j) hook(2048 + i, 90 + i, names[i]);
    atomic_fetch_add(&writers_done, 1);
    return NULL;
}

static void first_record_is_consistent(void)
{
    s_first = (xiaotai_mem_failure_t){.installed = true};
    s_function = NULL;
    task_reads = 0;
    pthread_t writers[WRITERS];
    unsigned ids[WRITERS];
    for (unsigned i = 0; i < WRITERS; ++i) {
        ids[i] = i;
        assert(pthread_create(&writers[i], NULL, failing_writer, &ids[i]) == 0);
    }
    atomic_store(&release_writers, true);
    uint32_t count = 0;
    xiaotai_mem_failure_t first = {0};
    do {
        xiaotai_mem_failure_t record;
        xiaotai_mem_failure(&record);
        assert(record.installed && record.failures >= count);
        count = record.failures;
        if (!record.has_first) {
            assert(!count);
            sched_yield();
            continue;
        }
        const unsigned i = (unsigned)(record.first_at_us - 1000);
        assert(i < WRITERS);
        assert(record.requested_size == 2048 + i && record.platform_caps == 90 + i);
        assert(strcmp(record.function, names[i]) == 0);
        assert(record.in_isr == ((i & 1U) != 0) && record.cache_enabled == ((i & 2U) != 0));
        assert(strcmp(record.task, !record.in_isr && record.cache_enabled ? names[i] : "") == 0);
        if (!first.has_first) first = record;
        assert(record.first_at_us == first.first_at_us);
        sched_yield();
    } while (atomic_load(&writers_done) != WRITERS);
    for (unsigned i = 0; i < WRITERS; ++i) assert(pthread_join(writers[i], NULL) == 0);
    xiaotai_mem_failure(&first);
    assert(first.failures == WRITERS * FAILURES_PER_WRITER);
    assert(task_reads == (first.cache_enabled && !first.in_isr ? 1 : 0));
}

static void inaccessible_flash_is_not_read_in_hook(void)
{
    const long size = sysconf(_SC_PAGESIZE);
    assert(size > 0);
    char *page = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    char *name = page + size - sizeof("allocation");
    memcpy(name, "allocation", sizeof("allocation"));
    for (unsigned mode = 0; mode < 4; ++mode) {
        s_first = (xiaotai_mem_failure_t){.installed = true};
        s_function = NULL;
        task_reads = time_reads = 0;
        in_interrupt = (mode & 1U) != 0;
        cache = (mode & 2U) != 0;
        task_name = "saved-task";
        assert(mprotect(page, (size_t)size, PROT_NONE) == 0);
        timestamp = INT64_C(0x123456789abcdef);
        hook(1, 2, name);
        for (unsigned i = 0; i < 64; ++i) hook(3, 4, "later");
        assert(mprotect(page, (size_t)size, PROT_READ) == 0);
        xiaotai_mem_failure_t record;
        xiaotai_mem_failure(NULL);
        xiaotai_mem_failure(&record);
        assert(record.failures == 65 && record.has_first && record.requested_size == 1 && record.platform_caps == 2);
        assert(record.first_at_us == timestamp && time_reads == 1 && task_reads == (mode == 2));
        assert(record.in_isr == in_interrupt && record.cache_enabled == cache);
        assert(!record.function_truncated && strcmp(record.function, "allocation") == 0);
        assert(strcmp(record.task, mode == 2 ? "saved-task" : "") == 0);
    }
    assert(munmap(page, (size_t)size) == 0);
}

static void bounded_names_and_task_ownership(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *blocks = mmap(NULL, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(blocks != MAP_FAILED);
    assert(mprotect(blocks + page, page, PROT_NONE) == 0);
    assert(mprotect(blocks + page * 3, page, PROT_NONE) == 0);
    char *function = blocks + page - sizeof(s_first.function);
    char *task = blocks + page * 3 - sizeof(s_first.task);
    in_interrupt = false;
    cache = true;
    timestamp = 0;
    for (unsigned clipped = 0; clipped < 2; ++clipped) {
        s_first = (xiaotai_mem_failure_t){.installed = true};
        s_function = NULL;
        memset(function, 'F', sizeof(s_first.function));
        memset(task, 'T', sizeof(s_first.task));
        if (!clipped) {
            function[sizeof(s_first.function) - 1] = '\0';
            task[sizeof(s_first.task) - 1] = '\0';
        }
        task_name = task;
        hook(0, UINT32_MAX, function);
        assert(mprotect(blocks + page * 2, page, PROT_NONE) == 0);
        xiaotai_mem_failure_t record;
        xiaotai_mem_failure(&record);
        assert(record.first_at_us == 0 && record.requested_size == 0 && record.platform_caps == UINT32_MAX);
        assert(record.function_truncated == (clipped != 0) && record.task_truncated == (clipped != 0));
        assert(strlen(record.function) == sizeof(s_first.function) - 1);
        assert(strlen(record.task) == sizeof(s_first.task) - 1 && record.task[0] == 'T');
        assert(mprotect(blocks + page * 2, page, PROT_READ | PROT_WRITE) == 0);
    }
    assert(munmap(blocks, page * 4) == 0);
    s_first = (xiaotai_mem_failure_t){.installed = true};
    s_function = NULL;
    task_name = NULL;
    hook(0, UINT32_MAX, NULL);
    xiaotai_mem_failure_t record;
    xiaotai_mem_failure(&record);
    assert(record.has_first && !record.task_truncated && !record.function_truncated);
    assert(!record.task[0] && !record.function[0]);
}
#endif

static void collect(void *ctx, const char *line)
{
    unsigned *lines = ctx;
    assert(xiaotai_mem_trace_start() == XIAOTAI_ERR_INVALID_STATE); /* No lock across output. */
    if ((*lines)++ % 2 == 0) {
        assert(strstr(line, "MEM_BLOCK ptr=") == line);
        assert(strstr(line, " size=64") && strstr(line, " pc="));
    } else {
        assert(strstr(line, "MEM_TRACE count=1 capacity=") == line && strstr(line, " high=10 overflow=0"));
    }
}

static void trace_lifecycle(void)
{
    unsigned lines = 0;
    assert(xiaotai_mem_trace_dump(collect, &lines) == XIAOTAI_ERR_INVALID_STATE);
    fail_alloc = true;
    assert(xiaotai_mem_trace_start() == XIAOTAI_ERR_NO_MEM && allocs == 0 && !trace_active);
    fail_alloc = false;
    for (unsigned i = 0; i < 12; ++i) {
        assert(xiaotai_mem_trace_start() == XIAOTAI_OK && trace_active);
        assert(xiaotai_mem_trace_start() == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_mem_trace_dump(collect, &lines) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_mem_trace_stop() == XIAOTAI_OK && !trace_active);
        if (i == 0) {
            fail_detach = true;
            assert(xiaotai_mem_trace_dump(collect, &lines) == XIAOTAI_FAIL);
            assert(frees == 0 && active_buffer == s_records);
            fail_detach = false;
        }
        assert(xiaotai_mem_trace_dump(collect, &lines) == XIAOTAI_OK);
        assert(allocs == frees && active_buffer == &s_inactive && records_capacity == 1);
    }
    assert(arms == 12 && lines == 26);
}

int main(void)
{
    failure_record();
#if CONFIG_ESP_TIMER_IN_IRAM
    first_record_is_consistent();
    inaccessible_flash_is_not_read_in_hook();
    bounded_names_and_task_ownership();
#endif
    trace_lifecycle();
    return 0;
}
