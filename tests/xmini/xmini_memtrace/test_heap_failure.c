#include "xmini_heap_failure.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WRITERS 8u
#define PER_WRITER 5000u
#define MASK_COOKIE 0x5a5a1234u
#define FIRST_TIME INT64_C(123456000)
#define FIRST_SIZE 65536u
#define FIRST_HANDLE 0x10000u
#define FIRST_CAPS 0xa5a50000u

typedef struct {
    uintptr_t handle;
    int64_t time_us;
    const char *task;
    const char *function;
    size_t size;
    uint32_t caps;
} callback_context_t;

static esp_alloc_failed_hook_t s_hook;
static esp_err_t s_registration_result = ESP_OK;
static unsigned s_registrations;
static pthread_mutex_t s_interrupt_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned s_masked, s_in_callback;
static _Thread_local bool s_isr, s_cache_enabled = true;
static _Thread_local const callback_context_t *s_context;
static atomic_uint s_task_reads, s_name_reads, s_time_reads;
static atomic_uint s_isr_reads, s_cache_reads;
static atomic_uint s_ready, s_completed, s_reader_ready;
static atomic_bool s_go;
static bool s_mixed_contexts;
static callback_context_t s_writers[WRITERS];
static char s_functions[WRITERS][XMINI_HEAP_FAILURE_FUNCTION_CAPACITY];
static char s_tasks[WRITERS][XMINI_HEAP_FAILURE_TASK_CAPACITY];

UBaseType_t heap_test_set_interrupt_mask(void)
{
    assert(!s_masked);
    assert(pthread_mutex_lock(&s_interrupt_lock) == 0);
    s_masked = 1u;
    return MASK_COOKIE;
}

void heap_test_clear_interrupt_mask(UBaseType_t previous)
{
    assert(s_masked && previous == MASK_COOKIE);
    s_masked = 0u;
    assert(pthread_mutex_unlock(&s_interrupt_lock) == 0);
}

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    assert(!s_masked && callback);
    ++s_registrations;
    if (s_registration_result == ESP_OK) s_hook = callback;
    return s_registration_result;
}

BaseType_t xPortInIsrContext(void)
{
    assert(s_masked && s_in_callback);
    atomic_fetch_add(&s_isr_reads, 1u);
    return s_isr;
}

bool spi_flash_cache_enabled(void)
{
    assert(s_masked && s_in_callback);
    atomic_fetch_add(&s_cache_reads, 1u);
    return s_cache_enabled;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    assert(s_masked && s_in_callback && s_context);
    assert(!s_isr && s_cache_enabled);
    atomic_fetch_add(&s_task_reads, 1u);
    return (TaskHandle_t)s_context->handle;
}

char *pcTaskGetName(TaskHandle_t task)
{
    assert(s_masked && s_in_callback && s_context && task);
    assert(!s_isr && s_cache_enabled);
    assert((uintptr_t)task == s_context->handle);
    atomic_fetch_add(&s_name_reads, 1u);
    return (char *)s_context->task;
}

int64_t esp_timer_get_time(void)
{
    assert(s_masked && s_in_callback && s_context);
    atomic_fetch_add(&s_time_reads, 1u);
    return s_context->time_us;
}

static xmini_heap_failure_status_t status(void)
{
    assert(!s_isr && s_cache_enabled && !s_in_callback);
    xmini_heap_failure_status_t result;
    memset(&result, 0xa5, sizeof(result));
    xmini_heap_failure_status(&result);
    return result;
}

static void check_no_first(const xmini_heap_failure_status_t *value)
{
    assert(!value->has_first && !value->failures);
    assert(!value->first_at_us && !value->requested_size && !value->caps && !value->task_handle);
    assert(!value->function[0] && !value->task[0]);
    assert(!value->function_truncated && !value->task_truncated);
    assert(!value->in_isr && !value->cache_enabled && !value->task_available);
}

static void initialize(void)
{
    xmini_heap_failure_status_t before = status();
    assert(!before.installed && before.registration_error == ESP_ERR_INVALID_STATE);
    check_no_first(&before);
    xmini_heap_failure_status(NULL);
    assert(xmini_heap_failure_init() == ESP_OK);
    assert(s_registrations == 1u && s_hook);
    before = status();
    assert(before.installed && before.registration_error == ESP_OK);
    check_no_first(&before);
}

static void emit_failure(const callback_context_t *context)
{
    assert(s_hook && !s_masked && !s_in_callback);
    s_context = context;
    s_in_callback = 1u;
    s_hook(context->size, context->caps, context->function);
    s_in_callback = 0u;
    assert(!s_masked);
}

static void check_names(const xmini_heap_failure_status_t *value,
                        const callback_context_t *expected,
                        size_t function_length, size_t task_length)
{
    size_t function_kept = function_length;
    size_t task_kept = task_length;
    if (function_kept >= sizeof(value->function)) function_kept = sizeof(value->function) - 1u;
    if (task_kept >= sizeof(value->task)) task_kept = sizeof(value->task) - 1u;
    assert(value->has_first && value->installed && value->registration_error == ESP_OK);
    assert(value->first_at_us == expected->time_us && value->requested_size == expected->size);
    assert(value->caps == expected->caps && value->task_handle == expected->handle);
    assert(!value->in_isr && value->cache_enabled && value->task_available == (expected->handle != 0));
    assert(memchr(value->function, 0, sizeof(value->function)) == value->function + function_kept);
    assert(memchr(value->task, 0, sizeof(value->task)) == value->task + task_kept);
    if (function_kept) assert(!memcmp(value->function, expected->function, function_kept));
    if (task_kept) assert(!memcmp(value->task, expected->task, task_kept));
    assert(value->function_truncated == (function_length >= sizeof(value->function)));
    assert(value->task_truncated == (task_length >= sizeof(value->task)));
}

static void check_same_first(const xmini_heap_failure_status_t *first,
                            xmini_heap_failure_status_t current)
{
    current.failures = first->failures;
    assert(!memcmp(first, &current, sizeof(current)));
}

static void test_basic(void)
{
    callback_context_t first = {0x1234u, INT64_C(123456789), "control", "heap_caps_malloc", 4096u, 0x12345678u};
    callback_context_t later = {0x9876u, INT64_C(987654321), "another-task", "another-function", 17u, 0xffu};
    initialize();
    emit_failure(&first);
    xmini_heap_failure_status_t initial = status();
    assert(initial.failures == 1u);
    check_names(&initial, &first, strlen(first.function), strlen(first.task));
    for (unsigned i = 0; i < 256u; ++i) {
        emit_failure(&later);
        xmini_heap_failure_status_t current = status();
        assert(current.failures == i + 2u);
        check_same_first(&initial, current);
    }
    xmini_heap_failure_status_t before = status();
    assert(xmini_heap_failure_init() == ESP_OK && s_registrations == 1u);
    xmini_heap_failure_status_t after = status();
    assert(!memcmp(&before, &after, sizeof(before)));
    assert(atomic_load(&s_task_reads) == 1u && atomic_load(&s_time_reads) == 1u && atomic_load(&s_name_reads) == 1u);
    assert(atomic_load(&s_isr_reads) == 1u && atomic_load(&s_cache_reads) == 1u);
    puts("first failure publication / immutable first / count / null output / idempotent init PASS");
}

static void test_registration_failure(void)
{
    xmini_heap_failure_status_t value = status();
    assert(!value.installed && value.registration_error == ESP_ERR_INVALID_STATE);
    check_no_first(&value);
    s_registration_result = ESP_FAIL;
    assert(xmini_heap_failure_init() == ESP_FAIL && s_registrations == 1u && !s_hook);
    value = status();
    assert(!value.installed && value.registration_error == ESP_FAIL);
    check_no_first(&value);
    s_registration_result = ESP_OK;
    assert(xmini_heap_failure_init() == ESP_FAIL && s_registrations == 1u && !s_hook);
    value = status();
    assert(!value.installed && value.registration_error == ESP_FAIL);
    check_no_first(&value);
    puts("registration failure stays explicit / installed0 / original result latched PASS");
}

static void test_string(const char *mode)
{
    char function[8193], task[8193];
    size_t function_length, task_length;
    callback_context_t first = {0x5555u, 0, task, function, 0, UINT32_MAX};
    if (!strcmp(mode, "null")) {
        function_length = task_length = 0;
        first.function = first.task = NULL;
    } else if (!strcmp(mode, "no-task")) {
        function_length = 4u;
        task_length = 0;
        memcpy(function, "boot", 5u);
        first.handle = 0;
        first.task = NULL;
    } else {
        function_length = !strcmp(mode, "boundary") ? sizeof(((xmini_heap_failure_status_t *)0)->function) - 1u :
            !strcmp(mode, "truncated") ? sizeof(((xmini_heap_failure_status_t *)0)->function) : 8192u;
        task_length = !strcmp(mode, "boundary") ? sizeof(((xmini_heap_failure_status_t *)0)->task) - 1u :
            !strcmp(mode, "truncated") ? sizeof(((xmini_heap_failure_status_t *)0)->task) : 8192u;
        memset(function, 'F', function_length);
        function[function_length] = 0;
        memset(task, 'T', task_length);
        task[task_length] = 0;
    }
    initialize();
    emit_failure(&first);
    xmini_heap_failure_status_t value = status();
    assert(value.failures == 1u);
    check_names(&value, &first, function_length, task_length);
    assert(atomic_load(&s_name_reads) == (first.handle ? 1u : 0u));
    printf("function/task strings %s / size0 / timestamp0 / caps32 preserved PASS\n", mode);
}

static void test_guarded_strings(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *memory = mmap(NULL, page * 4u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, page, PROT_NONE) == 0);
    assert(mprotect(memory + page * 3u, page, PROT_NONE) == 0);
    callback_context_t first = {0x7777u, 7, memory + page * 3u - XMINI_HEAP_FAILURE_TASK_CAPACITY,
        memory + page - XMINI_HEAP_FAILURE_FUNCTION_CAPACITY, 1, 2};
    memset((char *)first.function, 'G', XMINI_HEAP_FAILURE_FUNCTION_CAPACITY);
    memset((char *)first.task, 'H', XMINI_HEAP_FAILURE_TASK_CAPACITY);
    initialize();
    emit_failure(&first);
    xmini_heap_failure_status_t value = status();
    assert(value.failures == 1u);
    check_names(&value, &first, XMINI_HEAP_FAILURE_FUNCTION_CAPACITY, XMINI_HEAP_FAILURE_TASK_CAPACITY);
    assert(munmap(memory, page * 4u) == 0);
    puts("bounded name reads stop at32/16 before protected pages PASS");
}

static void check_context(const xmini_heap_failure_status_t *value,
                          const callback_context_t *first, bool isr, bool cache)
{
    bool task_available = !isr && cache && first->handle != 0;
    assert(value->has_first && value->installed && value->registration_error == ESP_OK);
    assert(value->in_isr == isr && value->cache_enabled == cache);
    assert(value->task_available == task_available);
    assert(value->first_at_us == first->time_us && value->requested_size == first->size && value->caps == first->caps);
    assert(value->task_handle == (task_available ? first->handle : 0));
    assert(!strcmp(value->function, first->function) && !value->function_truncated);
    if (task_available) assert(!strcmp(value->task, first->task));
    else assert(!value->task[0] && !value->task_truncated);
}

static void test_context(bool isr, bool cache)
{
    callback_context_t first = {0x8888u, INT64_C(0x123456789abcdef), "first-task", "heap_caps_malloc", 400, 0x80cu};
    callback_context_t later = {0x9999u, INT64_C(0x23456789abcdef0), "later-task", "heap_caps_calloc", 222, 0x804u};
    initialize();
    s_isr = isr;
    s_cache_enabled = cache;
    emit_failure(&first);
    s_isr = false;
    s_cache_enabled = true;
    xmini_heap_failure_status_t initial = status();
    check_context(&initial, &first, isr, cache);
    assert(initial.failures == 1u);
    for (unsigned i = 0; i < 64; ++i) {
        s_isr = (i & 1u) != 0;
        s_cache_enabled = (i & 2u) != 0;
        emit_failure(&later);
        s_isr = false;
        s_cache_enabled = true;
        xmini_heap_failure_status_t current = status();
        assert(current.failures == i + 2u);
        check_same_first(&initial, current);
    }
    unsigned task_reads = !isr && cache ? 1u : 0u;
    assert(atomic_load(&s_task_reads) == task_reads && atomic_load(&s_name_reads) == task_reads);
    assert(atomic_load(&s_time_reads) == 1u && atomic_load(&s_isr_reads) == 1u && atomic_load(&s_cache_reads) == 1u);
    assert(xmini_heap_failure_init() == ESP_OK && s_registrations == 1u);
    check_same_first(&initial, status());
    printf("first context ISR=%u cache=%u / getters=%u / immutable first / count65 PASS\n", isr, cache, task_reads);
}

static void test_deferred_function(bool safe)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memcpy(memory, "heap_caps_malloc", sizeof("heap_caps_malloc"));
    callback_context_t first = {0x7777u, INT64_C(0x123456789abcdef), "not-accessed", memory, 400, 0x80cu};
    initialize();
    assert(mprotect(memory, page, PROT_NONE) == 0);
    s_isr = !safe;
    s_cache_enabled = safe;
    emit_failure(&first);
    /* Restoring the simulated cache must precede the task-only snapshot. */
    assert(mprotect(memory, page, PROT_READ) == 0);
    s_isr = false;
    s_cache_enabled = true;
    xmini_heap_failure_status_t initial = status();
    check_context(&initial, &first, !safe, safe);
    unsigned task_reads = safe ? 1u : 0u;
    assert(initial.failures == 1u && atomic_load(&s_task_reads) == task_reads && atomic_load(&s_name_reads) == task_reads);
    callback_context_t later = {0x9999u, 42, "normal-task", "other-function", 9, 7};
    emit_failure(&later);
    xmini_heap_failure_status_t current = status();
    assert(current.failures == 2u);
    check_same_first(&initial, current);
    assert(atomic_load(&s_task_reads) == task_reads && atomic_load(&s_name_reads) == task_reads && atomic_load(&s_time_reads) == 1u);
    assert(munmap(memory, page) == 0);
    printf("PROT_NONE function during %s callback / deferred readable snapshot / no later task enrichment PASS\n",
           safe ? "normal" : "ISR/cache-off");
}

static void test_owned_task_name(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memcpy(memory, "retired-task", sizeof("retired-task"));
    callback_context_t first = {0x6666u, 1234, memory, "heap_caps_malloc", 99, 0x804u};
    initialize();
    emit_failure(&first);
    assert(mprotect(memory, page, PROT_NONE) == 0);
    xmini_heap_failure_status_t value = status();
    assert(value.task_available && value.task_handle == first.handle && !strcmp(value.task, "retired-task"));
    assert(!strcmp(value.function, first.function) && value.failures == 1u);
    assert(munmap(memory, page) == 0);
    puts("task-name copied at first safe failure / task memory unavailable before snapshot PASS");
}

static void check_concurrent_snapshot(const xmini_heap_failure_status_t *value)
{
    assert(value->installed && value->registration_error == ESP_OK);
    if (!value->has_first) {
        check_no_first(value);
        return;
    }
    assert(value->failures && value->failures <= WRITERS * PER_WRITER);
    assert(value->first_at_us >= FIRST_TIME && value->first_at_us < FIRST_TIME + WRITERS);
    unsigned writer = (unsigned)(value->first_at_us - FIRST_TIME);
    if (s_mixed_contexts)
        check_context(value, &s_writers[writer], (writer & 1u) != 0, (writer & 2u) != 0);
    else
        check_names(value, &s_writers[writer], strlen(s_functions[writer]), strlen(s_tasks[writer]));
}

static void *writer(void *opaque)
{
    callback_context_t *context = opaque;
    atomic_fetch_add(&s_ready, 1u);
    while (!atomic_load_explicit(&s_go, memory_order_acquire)) sched_yield();
    for (unsigned i = 0; i < PER_WRITER; ++i) {
        unsigned id = (unsigned)(context->handle - FIRST_HANDLE);
        s_isr = s_mixed_contexts && (id & 1u) != 0;
        s_cache_enabled = !s_mixed_contexts || (id & 2u) != 0;
        emit_failure(context);
        s_isr = false;
        s_cache_enabled = true;
        if (i % 17u == 0u) {
            xmini_heap_failure_status_t value = status();
            check_concurrent_snapshot(&value);
            sched_yield();
        }
    }
    atomic_fetch_add_explicit(&s_completed, 1u, memory_order_release);
    return NULL;
}

static void *reader(void *opaque)
{
    unsigned *samples = opaque;
    uint32_t previous = 0;
    xmini_heap_failure_status_t value = status();
    check_no_first(&value);
    *samples = 1u;
    atomic_store_explicit(&s_reader_ready, 1u, memory_order_release);
    do {
        value = status();
        check_concurrent_snapshot(&value);
        assert(value.failures >= previous);
        previous = value.failures;
        ++*samples;
        sched_yield();
    } while (atomic_load_explicit(&s_completed, memory_order_acquire) != WRITERS);
    return NULL;
}

static void test_concurrent(bool mixed)
{
    pthread_t writers[WRITERS], observer;
    unsigned samples = 0;
    s_mixed_contexts = mixed;
    initialize();
    for (unsigned i = 0; i < WRITERS; ++i) {
        snprintf(s_functions[i], sizeof(s_functions[i]), "writer-function-%u", i);
        snprintf(s_tasks[i], sizeof(s_tasks[i]), "writer-task-%u", i);
        s_writers[i] = (callback_context_t){FIRST_HANDLE + i, FIRST_TIME + i, s_tasks[i], s_functions[i], FIRST_SIZE + i, FIRST_CAPS + i};
        assert(pthread_create(&writers[i], NULL, writer, &s_writers[i]) == 0);
    }
    assert(pthread_create(&observer, NULL, reader, &samples) == 0);
    while (atomic_load(&s_ready) != WRITERS || !atomic_load_explicit(&s_reader_ready, memory_order_acquire)) sched_yield();
    atomic_store_explicit(&s_go, true, memory_order_release);
    for (unsigned i = 0; i < WRITERS; ++i) assert(pthread_join(writers[i], NULL) == 0);
    assert(pthread_join(observer, NULL) == 0);
    xmini_heap_failure_status_t first = status();
    check_concurrent_snapshot(&first);
    assert(first.failures == WRITERS * PER_WRITER && samples > 1u);
    unsigned task_reads = first.task_available ? 1u : 0u;
    assert(atomic_load(&s_task_reads) == task_reads && atomic_load(&s_time_reads) == 1u && atomic_load(&s_name_reads) == task_reads);
    assert(atomic_load(&s_isr_reads) == 1u && atomic_load(&s_cache_reads) == 1u);
    assert(xmini_heap_failure_init() == ESP_OK && s_registrations == 1u);
    xmini_heap_failure_status_t after = status();
    assert(!memcmp(&first, &after, sizeof(first)));
    printf("8 writers x%u / contexts=%s / snapshots=%u / coherent first / count=%u / task getters=%u PASS\n",
           PER_WRITER, mixed ? "mixed" : "normal", samples, first.failures, task_reads);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "basic";
    if (!strcmp(mode, "basic")) test_basic();
    else if (!strcmp(mode, "register-fail")) test_registration_failure();
    else if (!strcmp(mode, "concurrent")) test_concurrent(false);
    else if (!strcmp(mode, "concurrent-mixed")) test_concurrent(true);
    else if (!strcmp(mode, "guarded")) test_guarded_strings();
    else if (!strcmp(mode, "context-0-0")) test_context(false, false);
    else if (!strcmp(mode, "context-0-1")) test_context(false, true);
    else if (!strcmp(mode, "context-1-0")) test_context(true, false);
    else if (!strcmp(mode, "context-1-1")) test_context(true, true);
    else if (!strcmp(mode, "deferred")) test_deferred_function(false);
    else if (!strcmp(mode, "deferred-safe")) test_deferred_function(true);
    else if (!strcmp(mode, "owned-task")) test_owned_task_name();
    else if (!strcmp(mode, "null") || !strcmp(mode, "no-task") || !strcmp(mode, "boundary") ||
             !strcmp(mode, "truncated") || !strcmp(mode, "long")) test_string(mode);
    else return 2;
    return 0;
}
