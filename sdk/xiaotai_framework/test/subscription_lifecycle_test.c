// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_mem_host.h"

static xiaotai_signal_t barrier;
static xiaotai_signal_t publication_read, publication_done;
static unsigned publication_callbacks;
static char observed[10];
static int16_t scopes[10];
static size_t delivered, state_bytes, fallback_allocations, fallback_bytes;
static bool provisioning;
static enum { LINEAR, STATIC_ONLY, FAIL_FALLBACK, FAIL_AFTER, FILL, PUBLICATION } scenario;
static _Thread_local bool measure_state, measure_fallback;

void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    if (measure_state) {
        assert(kind == XIAOTAI_MEM_BULK && count == 1);
        state_bytes = size;
        measure_state = false;
    }
    return __real_xiaotai_mem_calloc(kind, count, size);
}

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    if (measure_fallback) {
        assert(kind == XIAOTAI_MEM_BULK);
        ++fallback_allocations;
        fallback_bytes += size;
    }
    return __real_xiaotai_mem_alloc(kind, size);
}

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    assert(xiaotai_on_owner() && !strcmp(event->name, "b1.event"));
    assert(event->size == sizeof("saved") && !strcmp(event->data, "saved"));
    assert(delivered < sizeof(scopes) / sizeof(*scopes) - 1);
    observed[delivered] = *(const char *)ctx;
    scopes[delivered++] = fw_app_current();
    observed[delivered] = '\0';
}

#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
typedef xiaotai_subscriber_descriptor_t fixture_descriptor_t;
#else
typedef struct { const char *name; xiaotai_event_fn_t handler; void *ctx; } fixture_descriptor_t;
#endif

static char a = 'A', c = 'C', d = 'D', f = 'F', x = 'X', y = 'Y';
static fixture_descriptor_t const descriptors[] = {
    {"b1.event", on_event, &a}, {"b1.event", on_event, &c},
    {"b1.event", on_event, &x}, {"b1.inactive", on_event, &x}, {NULL, on_event, NULL},
    {"b1.event", on_event, &y},
};

static xiaotai_err_t compiled(uint16_t id)
{
    measure_fallback = true;
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    xiaotai_err_t result = xiaotai_event_subscribe_compiled(id);
#else
    assert(id < sizeof(descriptors) / sizeof(*descriptors));
    const fixture_descriptor_t *item = &descriptors[id];
    xiaotai_err_t result = xiaotai_event_subscribe(item->name, item->handler, item->ctx);
#endif
    measure_fallback = false;
    return result;
}

static xiaotai_err_t runtime(char *marker)
{
    measure_fallback = true;
    xiaotai_err_t result = xiaotai_event_subscribe("b1.event", on_event, marker);
    measure_fallback = false;
    return result;
}

#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
static void *foreign_during_init(void *arg)
{
    (void)arg;
    assert(!xiaotai_on_owner());
    assert(compiled(0) == XIAOTAI_ERR_INVALID_STATE);
    assert(runtime(&d) == XIAOTAI_ERR_INVALID_STATE);
    return NULL;
}
#endif

static void on_publication(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && fw_app_current() == -1);
    assert(event->args.value == 2);
    ++publication_callbacks;
    xiaotai_signal_give(&barrier);
}

static void *emit_during_registration(void *arg)
{
    (void)arg;
    assert(!xiaotai_on_owner());
    for (int i = 1; i <= 2; ++i) {
        assert(xiaotai_signal_take(&publication_read, 2000));
        const xiaotai_args_t args = {.value = i};
        assert(xiaotai_event_emit("b1.publication", &args, NULL, 0) == XIAOTAI_OK);
        xiaotai_signal_give(&publication_done);
    }
    return NULL;
}

static xiaotai_err_t init(void)
{
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    assert(compiled(UINT16_MAX) == XIAOTAI_ERR_INVALID_ARG);
    assert(compiled(4) == XIAOTAI_ERR_INVALID_ARG);
    if (scenario == STATIC_ONLY) {
        pthread_t foreign;
        assert(pthread_create(&foreign, NULL, foreign_during_init, NULL) == 0);
        assert(pthread_join(foreign, NULL) == 0);
    }
#endif
    assert(compiled(0) == XIAOTAI_OK);
    if (scenario == FILL) {
        for (size_t i = 1; i < XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS; ++i)
            assert(compiled(0) == XIAOTAI_OK);
        assert(compiled(0) == XIAOTAI_ERR_NO_MEM);
        assert(runtime(&d) == XIAOTAI_ERR_NO_MEM);
        return XIAOTAI_OK;
    }
    if (scenario != STATIC_ONLY) assert(runtime(&d) == XIAOTAI_OK);
    if (scenario == PUBLICATION) {
        pthread_t foreign;
        assert(pthread_create(&foreign, NULL, emit_during_registration, NULL) == 0);
        xiaotai_signal_give(&publication_read);
        assert(xiaotai_signal_take(&publication_done, 2000));
        assert(compiled(1) == XIAOTAI_OK);
        assert(xiaotai_event_subscribe("b1.publication", on_publication, NULL) == XIAOTAI_OK);
        xiaotai_signal_give(&publication_read);
        assert(xiaotai_signal_take(&publication_done, 2000));
        assert(runtime(&f) == XIAOTAI_OK);
        assert(pthread_join(foreign, NULL) == 0);
        return XIAOTAI_OK;
    }
    if (!provisioning) assert(compiled(1) == XIAOTAI_OK);
    if (scenario == FAIL_FALLBACK) {
        xiaotai_mem_host_fail_nth(1);
        const xiaotai_err_t error = runtime(&f);
        assert(!xiaotai_mem_host_fail_pending());
        return error;
    }
    if (scenario != STATIC_ONLY) assert(runtime(&f) == XIAOTAI_OK);
    assert(compiled(0) == XIAOTAI_OK);
    return scenario == FAIL_AFTER ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t app_a_init(void) { return compiled(2); }
static xiaotai_err_t app_b_init(void) { return scenario == STATIC_ONLY ? compiled(5) : runtime(&y); }
static bool select_a(const xiaotai_event_t *event) { return event->args.value == 1; }
static const char *const handles[] = {"b1.event", NULL};
static const xiaotai_module_t source = {.name = "source", .init = init};
static const xiaotai_application_t first = {
    .name = "first", .init = app_a_init, .handles = handles, .claim = select_a,
};
static const xiaotai_application_t second = {.name = "second", .init = app_b_init, .handles = handles};

static xiaotai_err_t start(bool with_apps)
{
    static const xiaotai_module_t *const modules[] = {&source};
    static const xiaotai_application_t *const apps[] = {&first, &second};
    const xiaotai_framework_config_t cfg = {
        .modules = modules, .module_count = 1,
        .applications = with_apps ? apps : NULL, .application_count = with_apps ? 2 : 0,
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
        .compiled_subscribers = descriptors, .compiled_subscriber_count = sizeof(descriptors) / sizeof(*descriptors),
#endif
    };
    state_bytes = fallback_allocations = fallback_bytes = 0;
    measure_state = true;
    const xiaotai_err_t err = xiaotai_framework_start(&cfg);
    assert(state_bytes > 0 && !measure_state);
    return err;
}

static void synchronized(void *arg)
{
    (void)arg;
    xiaotai_signal_give(&barrier);
}

static void verify_event(int app, const char *expected)
{
    char name[] = "b1.event";
    char text[] = "saved";
    const xiaotai_args_t args = {.value = app + 1};
    delivered = 0;
    assert(xiaotai_event_emit(name, &args, text, sizeof(text)) == XIAOTAI_OK);
    name[0] = text[0] = '?';
    assert(xiaotai_run(synchronized, NULL) == XIAOTAI_OK && xiaotai_signal_take(&barrier, 2000));
    assert(delivered == strlen(expected) && !strcmp(observed, expected));
    for (size_t i = 0; i < delivered - 1; ++i) assert(scopes[i] == -1);
    assert(scopes[delivered - 1] == app);
}

static void verify_publication(void)
{
    /* The process-wide storage worker intentionally survives framework reset. */
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    const int64_t saved = xiaotai_mem_host_outstanding();
    const xiaotai_module_t *const modules[] = {&source};
    xiaotai_framework_config_t config = {
        .modules = modules, .module_count = 1,
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
        .compiled_subscribers = descriptors, .compiled_subscriber_count = sizeof(descriptors) / sizeof(*descriptors),
#endif
    };
    scenario = PUBLICATION;
    assert(xiaotai_signal_init(&publication_read) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&publication_done) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&barrier, 2000));
    xiaotai_framework_reset();
    assert(publication_callbacks == 1);
    xiaotai_signal_deinit(&publication_done);
    xiaotai_signal_deinit(&publication_read);
    assert(xiaotai_mem_host_outstanding() == saved);
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&barrier) == XIAOTAI_OK);
    if (argc > 1) {
        assert(argc == 2 && strcmp(argv[1], "--publication") == 0);
        verify_publication();
        xiaotai_signal_deinit(&barrier);
        return 0;
    }
    assert(xiaotai_event_subscribe("b1.event", on_event, &a) == XIAOTAI_ERR_INVALID_STATE);
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    assert(compiled(0) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.compiled_subscriber_count = 1}) ==
           XIAOTAI_ERR_INVALID_ARG);
#endif

    scenario = STATIC_ONLY;
    assert(start(true) == XIAOTAI_OK);
    assert(fallback_allocations == 0 && fallback_bytes == 0);
    printf("subscriber prototype: compact=%d slots=%d framework_state=%zu static_fallback=0\n",
           XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS, XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS, state_bytes);
    verify_event(0, "ACAX");
    verify_event(1, "ACAY");
    xiaotai_framework_reset();

    const int64_t saved = xiaotai_mem_host_outstanding();
    scenario = FAIL_AFTER;
    assert(start(true) == XIAOTAI_FAIL);
    assert(xiaotai_mem_host_outstanding() == saved);
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    scenario = FAIL_FALLBACK;
    assert(start(false) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_mem_host_outstanding() == saved);
#endif
    assert(xiaotai_event_emit("b1.event", NULL, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);

    scenario = LINEAR;
    for (unsigned i = 0; i < 2; ++i) {
        provisioning = i != 0;
        assert(start(true) == XIAOTAI_OK);
        assert(fallback_allocations == (XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS ? 3U : 0U));
        if (!i) printf("mixed_fallback_allocations=%zu mixed_fallback_bytes=%zu\n",
                       fallback_allocations, fallback_bytes);
        const size_t before = xiaotai_mem_host_allocations();
        assert(xiaotai_event_emit("b1.inactive", NULL, "saved", sizeof("saved")) == XIAOTAI_OK);
        assert(xiaotai_mem_host_allocations() == before);
        verify_event(0, provisioning ? "ADFAX" : "ADCFAX");
        verify_event(1, provisioning ? "ADFAY" : "ADCFAY");
        assert(xiaotai_event_subscribe("b1.event", on_event, &a) == XIAOTAI_ERR_INVALID_STATE);
        xiaotai_framework_reset();
        assert(xiaotai_mem_host_outstanding() == saved);
    }
    scenario = FILL;
    assert(start(false) == XIAOTAI_OK);
    assert(fallback_allocations == 0);
    xiaotai_framework_reset();
    assert(xiaotai_mem_host_outstanding() == saved);

    xiaotai_signal_deinit(&barrier);
    return 0;
}
