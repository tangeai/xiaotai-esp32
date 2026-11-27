// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_companion_maintenance.py
/* Network engine skeleton on the host port with the real framework and the
 * scripted driver (work package NW1 of the component's specification):
 *   - snapshots before init, and every refusal of xiaotai_net_configure; an
 *     all-zero configuration equals no configuration, and the defaults;
 *   - driver registration checks, the built-in drivers only for free kinds
 *     (a test's driver replaces one), binding to the first part with a
 *     driver, parts without one, registration closed at start;
 *   - what the driver's start() receives (fallback rule, driver storage);
 *   - bring-up: without prepare(), with prepare() and its progress, every
 *     final stage, errors, the immediate restart on the worker, a repeated
 *     bring-up with manual_retry, the gate's bring-up flag;
 *   - the queue: full queue counts and refusals, the lost-event mark, FIFO
 *     delivery and recovery; jobs, the blocking call slot with its timeout
 *     and hand-over, deadlines on absolute times; event packing;
 *   - snapshot commits and their events; generations and tokens;
 *   - memory by purpose (state and queue BULK, worker stack INTERNAL at
 *     priority 4), reset, and cleanup after a failed allocation at every
 *     step of a start. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/net.h"
#include "xiaotai/net_driver.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_net_host.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)
#define RUN(test)                                         \
    do {                                                  \
        if (test() != 0) {                                \
            fprintf(stderr, "FAILED: %s\n", #test);       \
            return 1;                                     \
        }                                                 \
    } while (0)

/* ------------------------------------------------- memory and task records */

typedef struct {
    xiaotai_mem_kind_t kind;
    size_t size;
} alloc_record_t;

static pthread_mutex_t s_record_lock = PTHREAD_MUTEX_INITIALIZER;
static alloc_record_t s_allocs[256];
static size_t s_alloc_count;
static xiaotai_task_config_t s_tasks[16];
static char s_task_names[16][32];
static size_t s_task_count;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);

static void record_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    pthread_mutex_lock(&s_record_lock);
    if (s_alloc_count < sizeof(s_allocs) / sizeof(s_allocs[0])) {
        s_allocs[s_alloc_count++] = (alloc_record_t){kind, size};
    }
    pthread_mutex_unlock(&s_record_lock);
}

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    record_alloc(kind, size);
    return __real_xiaotai_mem_alloc(kind, size);
}

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    record_alloc(kind, count * size);
    return __real_xiaotai_mem_calloc(kind, count, size);
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    pthread_mutex_lock(&s_record_lock);
    if (config && s_task_count < sizeof(s_tasks) / sizeof(s_tasks[0])) {
        s_tasks[s_task_count] = *config;
        snprintf(s_task_names[s_task_count], sizeof(s_task_names[0]), "%s", config->name ? config->name : "");
        ++s_task_count;
    }
    pthread_mutex_unlock(&s_record_lock);
    return __real_xiaotai_task_create(config, task);
}

static void forget_records(void)
{
    pthread_mutex_lock(&s_record_lock);
    s_alloc_count = 0;
    s_task_count = 0;
    pthread_mutex_unlock(&s_record_lock);
}

static size_t allocs_of(xiaotai_mem_kind_t kind, size_t size)
{
    size_t found = 0;
    pthread_mutex_lock(&s_record_lock);
    for (size_t i = 0; i < s_alloc_count; ++i) {
        if (s_allocs[i].kind == kind && s_allocs[i].size == size) ++found;
    }
    pthread_mutex_unlock(&s_record_lock);
    return found;
}

static const xiaotai_task_config_t *task_named(const char *name)
{
    for (size_t i = 0; i < s_task_count; ++i) {
        if (strcmp(s_task_names[i], name) == 0) return &s_tasks[i];
    }
    return NULL;
}

/* ------------------------------------------------------------- board parts */

static const xiaotai_network_t *s_parts[4];

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    return index < sizeof(s_parts) / sizeof(s_parts[0]) ? s_parts[index] : NULL;
}

static void parts(const xiaotai_network_t *first, const xiaotai_network_t *second)
{
    memset(s_parts, 0, sizeof(s_parts));
    s_parts[0] = first;
    s_parts[1] = second;
}

static const xiaotai_network_wifi_t s_wifi_facts = {.tx_buffers_on_demand = true};
/* Provisioning methods and their events are exercised by net_provision. */
static const xiaotai_network_t s_wifi_part = {
    .desc = {.kind = XIAOTAI_NETWORK_WIFI},
    .driver_config = &s_wifi_facts,
};
static const xiaotai_network_t s_ethernet_part = {.desc = {.kind = XIAOTAI_NETWORK_ETHERNET}};

/* A coprocessor part whose prepare() plays a script of stages. */
typedef struct {
    xiaotai_network_stage_t stages[6];
    uint8_t percents[6];
    size_t count;
    xiaotai_err_t result;
} prepare_script_t;

static prepare_script_t s_prepare;
static atomic_int s_prepare_calls;
static atomic_bool s_prepare_manual;
static atomic_bool s_prepare_on_worker;
static atomic_bool s_prepare_gate;

static xiaotai_err_t fake_prepare(void *self, bool manual_retry, xiaotai_network_progress_fn_t progress, void *ctx)
{
    const prepare_script_t *script = self;
    atomic_fetch_add(&s_prepare_calls, 1);
    atomic_store(&s_prepare_manual, manual_retry);
    atomic_store(&s_prepare_on_worker, xiaotai_net_host_on_worker());
    atomic_store(&s_prepare_gate, net_gate_bringup());
    for (size_t i = 0; i < script->count; ++i) progress(script->stages[i], script->percents[i], ctx);
    return script->result;
}

static const xiaotai_network_t s_coprocessor_part = {
    .desc = {.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR},
    .prepare = fake_prepare,
    .self = &s_prepare,
};

static void script(xiaotai_err_t result, size_t count, const xiaotai_network_stage_t *stages,
                   const uint8_t *percents)
{
    memset(&s_prepare, 0, sizeof(s_prepare));
    s_prepare.result = result;
    s_prepare.count = count;
    for (size_t i = 0; i < count; ++i) {
        s_prepare.stages[i] = stages[i];
        s_prepare.percents[i] = percents[i];
    }
    atomic_store(&s_prepare_calls, 0);
}

/* --------------------------------------------------------- event recorder */

typedef struct {
    char name[24];
    int32_t value;
    size_t size;
    union {
        xiaotai_net_status_t status;
        xiaotai_net_prepare_t prepare;
        xiaotai_net_provision_status_t provision;
    } data;
} event_record_t;

static event_record_t s_events[64];
static size_t s_event_count;
static atomic_bool s_gate_at_start;

static void on_net_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_event_count >= sizeof(s_events) / sizeof(s_events[0])) return;
    event_record_t *record = &s_events[s_event_count++];
    memset(record, 0, sizeof(*record));
    snprintf(record->name, sizeof(record->name), "%s", event->name);
    record->value = event->args.value;
    record->size = event->size;
    if (event->data && event->size <= sizeof(record->data)) memcpy(&record->data, event->data, event->size);
}

static xiaotai_err_t recorder_init(void)
{
    static const char *const names[] = {XIAOTAI_EVENT_NET_STATE, XIAOTAI_EVENT_NET_UP, XIAOTAI_EVENT_NET_DOWN,
                                        XIAOTAI_EVENT_NET_SIGNAL, XIAOTAI_EVENT_NET_PREPARE,
                                        XIAOTAI_EVENT_NET_PROVISION};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const xiaotai_err_t result = xiaotai_event_subscribe(names[i], on_net_event, NULL);
        if (result != XIAOTAI_OK) return result;
    }
    return XIAOTAI_OK;
}

/* Runs after the engine's start(), before XIAOTAI_EVENT_STARTED. */
static void recorder_start(void) { atomic_store(&s_gate_at_start, net_gate_bringup()); }

static const char *const RECORDER_DEPENDS[] = {"net", NULL};
static const xiaotai_module_t NET_MODULE = {
    .name = "net", .needs = XIAOTAI_NEED_NETWORK, .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start};
static const xiaotai_module_t RECORDER = {
    .name = "recorder", .depends = RECORDER_DEPENDS, .init = recorder_init, .start = recorder_start};
static const xiaotai_module_t *const MODULES[] = {&NET_MODULE, &RECORDER};

static void framework_restart(void) {}

static const xiaotai_framework_config_t FRAMEWORK = {
    .modules = MODULES,
    .module_count = 2,
    .available_needs = XIAOTAI_NEED_NETWORK,
    .restart = framework_restart,
};

static xiaotai_signal_t s_sync;
static void give_sync(void *ctx) { (void)ctx; xiaotai_signal_give(&s_sync); }

/* Everything queued to the owner before this point has been handled. */
static bool drain_owner(void)
{
    if (xiaotai_run(give_sync, NULL) != XIAOTAI_OK) return false;
    return xiaotai_signal_take(&s_sync, 2000);
}

/* The worker and then the owner have handled everything so far. */
static bool settle(void)
{
    return drain_owner() && xiaotai_net_host_sync(3000) == XIAOTAI_OK && drain_owner();
}

static const event_record_t *event_at(size_t index) { return index < s_event_count ? &s_events[index] : NULL; }

static void teardown(void)
{
    xiaotai_net_host_release();
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_net_host_reset();
}

/* Configures, starts the framework with the engine and the recorder, and
 * waits for the bring-up unless asked not to. */
static xiaotai_err_t boot(const xiaotai_net_config_t *config, bool wait)
{
    s_event_count = 0;
    if (config) {
        const xiaotai_err_t configured = xiaotai_net_configure(config);
        if (configured != XIAOTAI_OK) return configured;
    }
    const xiaotai_err_t started = xiaotai_framework_start(&FRAMEWORK);
    if (started != XIAOTAI_OK) return started;
    if (wait && !settle()) return XIAOTAI_ERR_TIMEOUT;
    return XIAOTAI_OK;
}

static size_t records(xiaotai_net_host_call_t call, xiaotai_net_host_record_t *out)
{
    static xiaotai_net_host_record_t all[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t count = xiaotai_net_host_records(all, XIAOTAI_NET_HOST_RECORDS_MAX);
    size_t found = 0;
    for (size_t i = 0; i < count && i < XIAOTAI_NET_HOST_RECORDS_MAX; ++i) {
        if (all[i].call != call) continue;
        if (out && found == 0) *out = all[i];
        ++found;
    }
    return found;
}

/* ================================================================== tests */

/* Field by field: padding bytes of structs are not compared. */
static bool text_equal(const char *a, const char *b) { return a == b || (a && b && strcmp(a, b) == 0); }

static bool config_equal(const xiaotai_net_config_t *a, const xiaotai_net_config_t *b)
{
    const xiaotai_net_retry_policy_t *x = &a->retry, *y = &b->retry;
    return a->layout == b->layout && text_equal(a->space, b->space) && a->history_capacity == b->history_capacity &&
           a->provisioning == b->provisioning && a->store_unverified == b->store_unverified &&
           a->keep_running == b->keep_running && a->accept_weak_security == b->accept_weak_security &&
           text_equal(a->ap_prefix, b->ap_prefix) && x->fast_retries == y->fast_retries &&
           x->jitter_percent == y->jitter_percent && x->first_ms == y->first_ms && x->max_ms == y->max_ms &&
           x->grow_from_previous_base == y->grow_from_previous_base &&
           x->join_immediately_on_radio_start == y->join_immediately_on_radio_start &&
           x->max_credentials_ms == y->max_credentials_ms && x->steps_ms == y->steps_ms &&
           x->step_count == y->step_count && x->retries_max == y->retries_max &&
           a->unverified_methods == b->unverified_methods &&
           a->serial_short_passwords == b->serial_short_passwords && a->dns_fallback == b->dns_fallback &&
           a->driver_storage == b->driver_storage;
}

static bool event_equal(const xiaotai_net_driver_event_t *a, const xiaotai_net_driver_event_t *b)
{
    return a->kind == b->kind && a->token == b->token && a->error == b->error && a->loss == b->loss &&
           a->code == b->code && a->signal_valid == b->signal_valid && a->signal_dbm == b->signal_dbm &&
           a->ipv4 == b->ipv4 && memcmp(a->ssid, b->ssid, sizeof(a->ssid)) == 0 &&
           a->ipv6_valid == b->ipv6_valid && memcmp(a->ipv6, b->ipv6, sizeof(a->ipv6)) == 0;
}

static int test_before_init(void)
{
    xiaotai_net_status_t status;
    memset(&status, 0xA5, sizeof(status));
    xiaotai_net_status(&status);
    static const xiaotai_net_status_t zero_status;
    CHECK(status.state == XIAOTAI_NET_STOPPED);
    CHECK(memcmp(&status, &zero_status, sizeof(status)) == 0);   /* the engine zeroes the whole struct */
    xiaotai_net_provision_status_t provision;
    memset(&provision, 0xA5, sizeof(provision));
    xiaotai_net_provision_status(&provision);
    static const xiaotai_net_provision_status_t zero_provision;
    CHECK(memcmp(&provision, &zero_provision, sizeof(provision)) == 0);
    xiaotai_net_diag_t diag;
    memset(&diag, 0xA5, sizeof(diag));
    xiaotai_net_diag(&diag);
    CHECK(diag.dropped_events == 0 && diag.attempts == 0 && diag.recent_count == 0);
    CHECK(xiaotai_net_generation() == 0);
    CHECK(!xiaotai_net_provisioning_boot());
    CHECK(!xiaotai_net_provisioning_exclusive());
    CHECK(xiaotai_net_driver_bound() == NULL);
    CHECK(xiaotai_net_host_sync(100) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

static void noop_job(const net_message_t *message) { (void)message; }

static int test_configure(void)
{
    xiaotai_net_config_t configured, effective, defaults;
    xiaotai_net_reset();
    xiaotai_net_host_config(NULL, &defaults);
    /* The defaults of xiaotai/net.h. */
    CHECK(defaults.layout == XIAOTAI_NET_STORE_RECORD && strcmp(defaults.space, "wifi_cfg") == 0);
    CHECK(defaults.history_capacity == 5);
    CHECK(defaults.provisioning == XIAOTAI_NET_PROVISIONING_CONCURRENT);
    CHECK(!defaults.store_unverified && !defaults.keep_running && !defaults.accept_weak_security);
    CHECK(strcmp(defaults.ap_prefix, "XiaoTai-") == 0);
    CHECK(defaults.retry.fast_retries == 0 && defaults.retry.jitter_percent == 0);
    CHECK(!defaults.retry.grow_from_previous_base && !defaults.retry.join_immediately_on_radio_start);
    CHECK(defaults.retry.first_ms == 1000 && defaults.retry.max_ms == 30000 &&
          defaults.retry.max_credentials_ms == 30000);
    CHECK(defaults.retry.steps_ms == NULL && defaults.retry.step_count == 0 && defaults.retry.retries_max == 0);
    CHECK(defaults.unverified_methods == 0 && !defaults.serial_short_passwords);
    CHECK(defaults.dns_fallback == XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY && !defaults.driver_storage);

    /* All zero is the same as never configured, field for field. */
    static const xiaotai_net_config_t zero;
    CHECK(xiaotai_net_configure(&zero) == XIAOTAI_OK);
    xiaotai_net_host_config(&configured, &effective);
    CHECK(config_equal(&configured, &zero));
    CHECK(config_equal(&effective, &defaults));

    /* A valid configuration, kept through every refusal below. */
    static const uint32_t steps[16] = {1000, 2000, 4000, 8000, 15000, 30000};
    const xiaotai_net_config_t good = {
        .layout = XIAOTAI_NET_STORE_PLAIN,
        .space = "net_plain12345",   /* 14 characters */
        .provisioning = XIAOTAI_NET_PROVISIONING_EXCLUSIVE,
        .ap_prefix = "123456789012345678901234567",   /* 27 characters: the longest */
        .retry = {.jitter_percent = 100, .first_ms = 5000, .max_ms = 5000, .steps_ms = steps, .step_count = 16},
        .serial_short_passwords = true,
        .dns_fallback = XIAOTAI_NET_DNS_FALLBACK_NEVER,
        .history_capacity = 4,
    };
    CHECK(xiaotai_net_configure(&good) == XIAOTAI_OK);

    xiaotai_net_config_t bad;
#define REFUSED(change)                                                \
    do {                                                               \
        bad = good;                                                    \
        change;                                                        \
        CHECK(xiaotai_net_configure(&bad) == XIAOTAI_ERR_INVALID_ARG); \
    } while (0)
    REFUSED(bad.layout = (xiaotai_net_store_layout_t)7);
    REFUSED(bad.space = "");
    REFUSED(bad.space = "sixteen_chars_xx");
    REFUSED(bad.space = NULL);   /* plain layout without a namespace */
    REFUSED(bad.history_capacity = 3);
    REFUSED(bad.history_capacity = 6);
    REFUSED(bad.provisioning = (xiaotai_net_provisioning_mode_t)2);
    REFUSED(bad.ap_prefix = "1234567890123456789012345678");   /* 28 */
    REFUSED(bad.retry.jitter_percent = 101);
    REFUSED(bad.retry.max_ms = 4999);                          /* below first_ms */
    REFUSED((bad.retry.first_ms = 40000, bad.retry.max_ms = 0)); /* above the default cap */
    REFUSED(bad.retry.step_count = 0);                         /* table without a count */
    REFUSED(bad.retry.steps_ms = NULL);                        /* count without a table */
    REFUSED(bad.retry.step_count = 17);
    REFUSED(bad.retry.grow_from_previous_base = true);         /* mutually exclusive with a fixed table */
    REFUSED(bad.layout = XIAOTAI_NET_STORE_RECORD);            /* serial short passwords need plain */
    REFUSED(bad.dns_fallback = (xiaotai_net_dns_fallback_t)3);
#undef REFUSED
    xiaotai_net_host_config(&configured, &effective);
    CHECK(config_equal(&configured, &good));
    CHECK(effective.history_capacity == 4 && effective.retry.max_credentials_ms == 5000);

    /* Record layout: no namespace is the default one; history 0 is 5. */
    const xiaotai_net_config_t record = {.history_capacity = 0, .space = NULL};
    CHECK(xiaotai_net_configure(&record) == XIAOTAI_OK);
    /* NULL restores every default. */
    CHECK(xiaotai_net_configure(&good) == XIAOTAI_OK);
    CHECK(xiaotai_net_configure(NULL) == XIAOTAI_OK);
    xiaotai_net_host_config(&configured, &effective);
    CHECK(config_equal(&configured, &zero));
    CHECK(config_equal(&effective, &defaults));

    /* Nothing is queued before init. */
    CHECK(net_post(noop_job, 0, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

static xiaotai_err_t ok_start(void *self, const xiaotai_network_t *part, const xiaotai_net_driver_start_t *start);
static xiaotai_err_t ok_join(void *self, uint32_t token, const xiaotai_net_credentials_t *credentials,
                             uint32_t flags)
{
    (void)self;
    (void)token;
    (void)credentials;
    (void)flags;
    return XIAOTAI_OK;
}
static xiaotai_err_t ok_leave(void *self)
{
    (void)self;
    return XIAOTAI_OK;
}
static xiaotai_err_t ok_scan(void *self, uint32_t token)
{
    (void)self;
    (void)token;
    return XIAOTAI_OK;
}
static xiaotai_err_t ok_scan_read(void *self, xiaotai_net_scan_record_t *records, size_t capacity, size_t *count,
                                  bool *truncated)
{
    (void)self;
    (void)records;
    (void)capacity;
    *count = 0;
    *truncated = false;
    return XIAOTAI_OK;
}
static xiaotai_err_t ok_ap_start(void *self, const xiaotai_net_ap_config_t *config)
{
    (void)self;
    (void)config;
    return XIAOTAI_OK;
}
static xiaotai_err_t ok_signal(void *self, int8_t *dbm)
{
    (void)self;
    *dbm = -40;
    return XIAOTAI_OK;
}

static atomic_int s_own_starts;
static const xiaotai_network_t *s_own_part;
static xiaotai_err_t ok_start(void *self, const xiaotai_network_t *part, const xiaotai_net_driver_start_t *start)
{
    (void)self;
    (void)start;
    s_own_part = part;
    atomic_fetch_add(&s_own_starts, 1);
    return XIAOTAI_OK;
}

static const xiaotai_net_driver_t OWN_WIFI = {
    .kind = XIAOTAI_NETWORK_WIFI, .name = "own_wifi", .capabilities = XIAOTAI_NET_DRIVER_CREDENTIALS,
    .start = ok_start, .join = ok_join, .leave = ok_leave};

static int test_driver_registration(void)
{
    xiaotai_net_reset();
    xiaotai_net_driver_t driver = OWN_WIFI;
    CHECK(xiaotai_net_driver_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    driver = OWN_WIFI;
    driver.start = NULL;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);
    driver = OWN_WIFI;
    driver.join = NULL;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);
    driver = OWN_WIFI;
    driver.leave = NULL;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);
    /* A capability without its functions. */
    driver = OWN_WIFI;
    driver.capabilities = XIAOTAI_NET_DRIVER_SCAN;
    driver.scan = ok_scan;
    driver.scan_cancel = ok_leave;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);   /* no scan_read */
    driver.scan_read = ok_scan_read;
    driver.capabilities |= XIAOTAI_NET_DRIVER_ACCESS_POINT;
    driver.ap_start = ok_ap_start;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);   /* no ap_stop */
    driver.ap_stop = ok_leave;
    driver.capabilities |= XIAOTAI_NET_DRIVER_SIGNAL;
    CHECK(xiaotai_net_driver_register(&driver) == XIAOTAI_ERR_INVALID_ARG);   /* no signal */
    driver.signal = ok_signal;
    /* Names appear in diagnostics lines; kinds must be known. */
    xiaotai_net_driver_t named = driver;
    named.name = NULL;
    CHECK(xiaotai_net_driver_register(&named) == XIAOTAI_ERR_INVALID_ARG);
    named.name = "Wifi";
    CHECK(xiaotai_net_driver_register(&named) == XIAOTAI_ERR_INVALID_ARG);
    named.name = "sixteen_chars_xx";
    CHECK(xiaotai_net_driver_register(&named) == XIAOTAI_ERR_INVALID_ARG);
    named = driver;
    named.kind = (xiaotai_network_kind_t)99;
    CHECK(xiaotai_net_driver_register(&named) == XIAOTAI_ERR_INVALID_ARG);

    static xiaotai_net_driver_t full;
    full = driver;
    CHECK(xiaotai_net_driver_register(&full) == XIAOTAI_OK);
    CHECK(net_driver_for(XIAOTAI_NETWORK_WIFI) == &full);
    CHECK(xiaotai_net_driver_register(&OWN_WIFI) == XIAOTAI_ERR_INVALID_STATE);   /* kind taken */
    CHECK(xiaotai_net_driver_bound() == NULL);
    xiaotai_net_reset();
    CHECK(net_driver_for(XIAOTAI_NETWORK_WIFI) == NULL);
    CHECK(xiaotai_net_driver_register(&OWN_WIFI) == XIAOTAI_OK);   /* forgotten by reset */
    xiaotai_net_reset();
    return 0;
}

/* Boot on a Wi-Fi part: the built-in driver is bound, its start() receives the
 * product's fallback rule and driver storage, the network runs unconfigured;
 * placement of the engine's memory and worker. */
static int test_boot_wifi(void)
{
    const struct {
        xiaotai_net_dns_fallback_t rule;
        bool storage;
    } cases[] = {
        {XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY, true},    /* the record-layout family that never turned it off */
        {XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY, true},
        {XIAOTAI_NET_DNS_FALLBACK_NEVER, false},       /* the plain-layout family */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        parts(&s_wifi_part, NULL);
        forget_records();
        xiaotai_net_config_t config = {.dns_fallback = cases[i].rule, .driver_storage = cases[i].storage};
        if (cases[i].rule == XIAOTAI_NET_DNS_FALLBACK_NEVER) {
            config.layout = XIAOTAI_NET_STORE_PLAIN;
            config.space = "net_plain";
        }
        CHECK(boot(&config, true) == XIAOTAI_OK);
        CHECK(!xiaotai_net_provisioning_exclusive());
        CHECK(xiaotai_net_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);   /* closed by init */
        CHECK(xiaotai_net_driver_bound() == xiaotai_net_host_driver(XIAOTAI_NETWORK_WIFI));

        xiaotai_net_host_record_t start;
        CHECK(records(XIAOTAI_NET_HOST_START, &start) == 1);
        CHECK(start.kind == XIAOTAI_NETWORK_WIFI && start.part == &s_wifi_part && start.on_worker);
        CHECK(start.start.fallback_dns_rule == cases[i].rule);
        CHECK(start.start.driver_storage == cases[i].storage);
        CHECK(start.start.fallback_dns && strcmp(start.start.fallback_dns, XIAOTAI_NET_FALLBACK_DNS_IPV4) == 0);
        CHECK(start.start.emit != NULL);
        CHECK(records(XIAOTAI_NET_HOST_JOIN, NULL) == 0);   /* nothing joins without the link unit's say */

        xiaotai_net_status_t status;
        xiaotai_net_status(&status);
        CHECK(status.state == XIAOTAI_NET_UNCONFIGURED);
        CHECK(status.part == 0 && status.kind == XIAOTAI_NETWORK_WIFI);
        CHECK(status.prepare.stage == XIAOTAI_NET_PREPARE_NONE && status.generation == 0);
        CHECK(status.revision >= 2);   /* binding, then the state */
        CHECK(s_event_count == 1);
        CHECK(strcmp(event_at(0)->name, XIAOTAI_EVENT_NET_STATE) == 0);
        CHECK(event_at(0)->value == XIAOTAI_NET_UNCONFIGURED && event_at(0)->size == sizeof(xiaotai_net_status_t));
        CHECK(event_at(0)->data.status.state == XIAOTAI_NET_UNCONFIGURED);
        CHECK(event_at(0)->data.status.revision == status.revision);
        CHECK(!net_gate_bringup() && !atomic_load(&s_gate_at_start));   /* no prepare(): no bring-up hold */

        /* Memory by purpose. */
        CHECK(allocs_of(XIAOTAI_MEM_BULK, sizeof(net_t)) == 1);
        CHECK(allocs_of(XIAOTAI_MEM_INTERNAL, sizeof(net_t)) == 0);
        CHECK(allocs_of(XIAOTAI_MEM_BULK, (size_t)XIAOTAI_NET_QUEUE_DEPTH * NET_MESSAGE_BYTES) == 1);
        const xiaotai_task_config_t *worker = task_named("xiaotai_net");
        CHECK(worker != NULL);
        CHECK(worker->stack == XIAOTAI_MEM_INTERNAL && worker->stack_bytes == XIAOTAI_NET_WORKER_STACK_BYTES);
        CHECK(worker->priority == 4);
        teardown();
    }
    parts(&s_wifi_part, NULL);
    forget_records();
    const xiaotai_net_config_t exclusive = {.provisioning = XIAOTAI_NET_PROVISIONING_EXCLUSIVE};
    CHECK(boot(&exclusive, true) == XIAOTAI_OK);
    CHECK(xiaotai_net_provisioning_exclusive() && xiaotai_net_provisioning_boot());
    teardown();
    CHECK(!xiaotai_net_provisioning_exclusive());
    return 0;
}

/* No part with a driver: nothing is bound and nothing starts; a part with a
 * driver after one without is used, with its index. */
static int test_parts_without_driver(void)
{
    parts(&s_ethernet_part, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_net_driver_bound() == NULL);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_STOPPED && status.revision == 0);
    CHECK(records(XIAOTAI_NET_HOST_START, NULL) == 0 && s_event_count == 0);
    /* Registration is closed by start, also for a kind nobody took. */
    static const xiaotai_net_driver_t ethernet = {
        .kind = XIAOTAI_NETWORK_ETHERNET, .name = "late", .start = ok_start, .join = ok_join, .leave = ok_leave};
    CHECK(xiaotai_net_driver_register(&ethernet) == XIAOTAI_ERR_INVALID_STATE);
    teardown();

    parts(NULL, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_net_driver_bound() == NULL && s_event_count == 0);
    teardown();

    parts(&s_ethernet_part, &s_wifi_part);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    xiaotai_net_status(&status);
    CHECK(status.part == 1 && status.kind == XIAOTAI_NETWORK_WIFI && status.state == XIAOTAI_NET_UNCONFIGURED);
    xiaotai_net_host_record_t start;
    CHECK(records(XIAOTAI_NET_HOST_START, &start) == 1 && start.part == &s_wifi_part);
    teardown();
    return 0;
}

/* A driver registered before the engine's init replaces the built-in one of
 * its kind; the other kind keeps the built-in driver. */
static int test_driver_replacement(void)
{
    parts(&s_wifi_part, NULL);
    atomic_store(&s_own_starts, 0);
    s_own_part = NULL;
    CHECK(xiaotai_net_driver_register(&OWN_WIFI) == XIAOTAI_OK);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_net_driver_bound() == &OWN_WIFI);
    CHECK(atomic_load(&s_own_starts) == 1 && s_own_part == &s_wifi_part);
    CHECK(records(XIAOTAI_NET_HOST_START, NULL) == 0);
    CHECK(net_driver_for(XIAOTAI_NETWORK_WIFI_COPROCESSOR) == xiaotai_net_host_driver(XIAOTAI_NETWORK_WIFI_COPROCESSOR));
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_UNCONFIGURED);
    teardown();
    return 0;
}

/* The driver's start() fails: BLOCKED with FAULT_START and its error. */
static int test_start_failure(void)
{
    parts(&s_wifi_part, NULL);
    const xiaotai_net_host_step_t fail = {.result = XIAOTAI_ERR_NOT_FOUND};
    CHECK(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &fail) == XIAOTAI_OK);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_BLOCKED);
    CHECK(status.prepare.stage == XIAOTAI_NET_PREPARE_FAILED && status.prepare.fault == XIAOTAI_NET_FAULT_START);
    CHECK(status.prepare.error == XIAOTAI_ERR_NOT_FOUND);
    CHECK(s_event_count == 2);
    CHECK(strcmp(event_at(0)->name, XIAOTAI_EVENT_NET_PREPARE) == 0);
    CHECK(event_at(0)->size == sizeof(xiaotai_net_prepare_t));
    CHECK(event_at(0)->data.prepare.fault == XIAOTAI_NET_FAULT_START);
    CHECK(strcmp(event_at(1)->name, XIAOTAI_EVENT_NET_STATE) == 0 && event_at(1)->value == XIAOTAI_NET_BLOCKED);
    CHECK(!net_gate_bringup());
    teardown();
    return 0;
}

static void bringup_again(const net_message_t *message)
{
    (void)message;
    net_bringup(true);
}

static int companion_start_failure(bool block_retry)
{
    parts(&s_coprocessor_part, NULL);
    const xiaotai_network_stage_t ready = XIAOTAI_NETWORK_READY;
    const uint8_t full = 100;
    script(XIAOTAI_OK, 1, &ready, &full);
    const xiaotai_net_host_step_t fail = {.result = XIAOTAI_FAIL};
    CHECK(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &fail) == XIAOTAI_OK);
    const xiaotai_net_config_t config = {.block_retry_after_driver_start_failure = block_retry};
    CHECK(boot(&config, true) == XIAOTAI_OK);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_BLOCKED && status.prepare.fault == XIAOTAI_NET_FAULT_START &&
          status.prepare.error == XIAOTAI_FAIL && !net_gate_bringup());
    CHECK(atomic_load(&s_prepare_calls) == 1 && records(XIAOTAI_NET_HOST_START, NULL) == 1U);
    const xiaotai_err_t retry = xiaotai_net_retry();
    CHECK(settle());
    xiaotai_net_status(&status);
    const size_t starts = records(XIAOTAI_NET_HOST_START, NULL);
    const unsigned prepares = (unsigned)atomic_load(&s_prepare_calls);
    printf("companion driver-start failure: block_retry=%u admission=%d prepare=%u driver_start=%zu\n",
           (unsigned)block_retry, retry, prepares, starts);
    const unsigned expected_attempts = block_retry ? 1U : 2U;
    CHECK(prepares == expected_attempts && starts == expected_attempts && !status.control_pending);
    if (block_retry) {
        CHECK(retry == XIAOTAI_ERR_INVALID_STATE && status.state == XIAOTAI_NET_BLOCKED &&
              status.prepare.fault == XIAOTAI_NET_FAULT_START && status.prepare.error == XIAOTAI_FAIL);
        /* Work already handed to the worker must observe the same fault. */
        CHECK(net_post(bringup_again, 0, NULL, 0) == XIAOTAI_OK && settle());
        CHECK(atomic_load(&s_prepare_calls) == 1 && records(XIAOTAI_NET_HOST_START, NULL) == 1U);
    } else {
        CHECK(retry == XIAOTAI_OK && status.state == XIAOTAI_NET_UNCONFIGURED && atomic_load(&s_prepare_manual));
    }
    uint32_t ticket;
    CHECK(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    xiaotai_net_maintenance_end(ticket);
    teardown();
    return 0;
}

static int companion_prepare_recovery(bool restrict_retry)
{
    parts(&s_coprocessor_part, NULL);
    const xiaotai_network_stage_t failure = XIAOTAI_NETWORK_NO_LINK;
    const uint8_t zero = 0;
    script(XIAOTAI_FAIL, 1, &failure, &zero);
    const xiaotai_net_config_t config = {.block_retry_after_driver_start_failure = true,
                                        .restrict_blocked_retry_to_recoverable = restrict_retry};
    CHECK(boot(&config, true) == XIAOTAI_OK);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_BLOCKED && status.prepare.fault == XIAOTAI_NET_FAULT_NO_LINK &&
          atomic_load(&s_prepare_calls) == 1 && !atomic_load(&s_prepare_manual));
    uint32_t lease;
    CHECK(xiaotai_net_maintenance_begin(&lease) == XIAOTAI_OK && lease);
    CHECK(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(lease);
    CHECK(xiaotai_net_retry() == XIAOTAI_OK && settle());
    CHECK(atomic_load(&s_prepare_calls) == 2 && atomic_load(&s_prepare_manual) && !records(XIAOTAI_NET_HOST_START, NULL));
    s_prepare.stages[0] = XIAOTAI_NETWORK_READY;
    s_prepare.result = XIAOTAI_OK;
    CHECK(xiaotai_net_retry() == XIAOTAI_OK && settle());
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_UNCONFIGURED && !status.control_pending && status.prepare.error == XIAOTAI_OK);
    CHECK(atomic_load(&s_prepare_calls) == 3 && records(XIAOTAI_NET_HOST_START, NULL) == 1U);
    printf("companion prepare recovery: restrict=%u; held lease rejects retry; two manual attempts start driver once\n",
           (unsigned)restrict_retry);
    teardown();
    return 0;
}

static int test_companion_start_failure(void)
{
    CHECK(!companion_start_failure(true));
    CHECK(!companion_start_failure(false));
    CHECK(!companion_prepare_recovery(true));
    CHECK(!companion_prepare_recovery(false));
    return 0;
}

/* Bring-up with prepare(): progress, READY then start; the gate counts the
 * bring-up from init to the driver's start. */
static int test_prepare_ready(void)
{
    parts(&s_coprocessor_part, NULL);
    const xiaotai_network_stage_t stages[] = {XIAOTAI_NETWORK_CHECKING, XIAOTAI_NETWORK_VERIFYING,
                                              XIAOTAI_NETWORK_READY};
    const uint8_t percents[] = {10, 20, 150};
    script(XIAOTAI_OK, 3, stages, percents);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(atomic_load(&s_gate_at_start));   /* held from init on */
    CHECK(atomic_load(&s_prepare_calls) == 1 && !atomic_load(&s_prepare_manual));
    CHECK(atomic_load(&s_prepare_on_worker) && atomic_load(&s_prepare_gate));
    CHECK(!net_gate_bringup());
    xiaotai_net_host_record_t start;
    CHECK(records(XIAOTAI_NET_HOST_START, &start) == 1 && start.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR);
    CHECK(start.part == &s_coprocessor_part);

    /* STATE(PREPARING), three PREPARE, STATE(UNCONFIGURED). */
    CHECK(s_event_count == 5);
    CHECK(strcmp(event_at(0)->name, XIAOTAI_EVENT_NET_STATE) == 0 && event_at(0)->value == XIAOTAI_NET_PREPARING);
    CHECK(event_at(0)->data.status.prepare.stage == XIAOTAI_NET_PREPARE_NONE);
    const xiaotai_net_prepare_stage_t expected[] = {XIAOTAI_NET_PREPARE_CHECKING, XIAOTAI_NET_PREPARE_VERIFYING,
                                                    XIAOTAI_NET_PREPARE_READY};
    const int32_t expected_percent[] = {10, 20, 100};
    for (size_t i = 0; i < 3; ++i) {
        const event_record_t *event = event_at(1 + i);
        CHECK(strcmp(event->name, XIAOTAI_EVENT_NET_PREPARE) == 0);
        CHECK(event->value == expected_percent[i] && event->data.prepare.percent == expected_percent[i]);
        CHECK(event->data.prepare.stage == expected[i] && event->data.prepare.fault == XIAOTAI_NET_FAULT_NONE);
    }
    CHECK(strcmp(event_at(4)->name, XIAOTAI_EVENT_NET_STATE) == 0 && event_at(4)->value == XIAOTAI_NET_UNCONFIGURED);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.prepare.stage == XIAOTAI_NET_PREPARE_READY && status.prepare.percent == 100);
    CHECK(status.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR);
    /* Once running, a repeated bring-up does nothing. */
    CHECK(net_post(bringup_again, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(atomic_load(&s_prepare_calls) == 1 && records(XIAOTAI_NET_HOST_START, NULL) == 1);
    teardown();
    return 0;
}

/* Every final stage, an error without one, and OK naming no outcome. */
static int test_prepare_faults(void)
{
    const struct {
        xiaotai_network_stage_t stage;   /* reported after CHECKING; READY: none reported */
        bool report;
        xiaotai_err_t result;
        xiaotai_net_fault_t fault;
        xiaotai_err_t error;
    } cases[] = {
        {XIAOTAI_NETWORK_NO_LINK, true, XIAOTAI_FAIL, XIAOTAI_NET_FAULT_NO_LINK, XIAOTAI_FAIL},
        {XIAOTAI_NETWORK_UNFINISHED, true, XIAOTAI_FAIL, XIAOTAI_NET_FAULT_INTERRUPTED, XIAOTAI_FAIL},
        {XIAOTAI_NETWORK_BAD_IMAGE, true, XIAOTAI_ERR_INVALID_SIZE, XIAOTAI_NET_FAULT_BAD_IMAGE,
         XIAOTAI_ERR_INVALID_SIZE},
        {XIAOTAI_NETWORK_UNSUPPORTED, true, XIAOTAI_FAIL, XIAOTAI_NET_FAULT_UNSUPPORTED, XIAOTAI_FAIL},
        {XIAOTAI_NETWORK_POWER_CYCLE, true, XIAOTAI_FAIL, XIAOTAI_NET_FAULT_POWER_CYCLE, XIAOTAI_FAIL},
        {XIAOTAI_NETWORK_WRITING, true, XIAOTAI_ERR_TIMEOUT, XIAOTAI_NET_FAULT_ERROR, XIAOTAI_ERR_TIMEOUT},
        {XIAOTAI_NETWORK_READY, false, XIAOTAI_ERR_NO_MEM, XIAOTAI_NET_FAULT_ERROR, XIAOTAI_ERR_NO_MEM},
        {XIAOTAI_NETWORK_CHECKING, false, XIAOTAI_OK, XIAOTAI_NET_FAULT_ERROR, XIAOTAI_ERR_INVALID_STATE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        parts(&s_coprocessor_part, NULL);
        const xiaotai_network_stage_t stages[] = {XIAOTAI_NETWORK_CHECKING, cases[i].stage};
        const uint8_t percents[] = {5, 7};
        script(cases[i].result, cases[i].report ? 2 : 0, stages, percents);
        CHECK(boot(NULL, true) == XIAOTAI_OK);
        xiaotai_net_status_t status;
        xiaotai_net_status(&status);
        CHECK(status.state == XIAOTAI_NET_BLOCKED);
        CHECK(status.prepare.stage == XIAOTAI_NET_PREPARE_FAILED);
        CHECK(status.prepare.fault == cases[i].fault && status.prepare.error == cases[i].error);
        CHECK(records(XIAOTAI_NET_HOST_START, NULL) == 0);
        CHECK(!net_gate_bringup());   /* BLOCKED does not hold the gate */
        const event_record_t *last = event_at(s_event_count - 1);
        CHECK(last && strcmp(last->name, XIAOTAI_EVENT_NET_STATE) == 0 && last->value == XIAOTAI_NET_BLOCKED);
        const event_record_t *fault = event_at(s_event_count - 2);
        CHECK(fault && strcmp(fault->name, XIAOTAI_EVENT_NET_PREPARE) == 0);
        CHECK(fault->data.prepare.fault == cases[i].fault);

        /* The repeated bring-up passes manual_retry and may succeed. */
        if (i == 0) {
            const xiaotai_network_stage_t ready[] = {XIAOTAI_NETWORK_READY};
            const uint8_t full[] = {100};
            script(XIAOTAI_OK, 1, ready, full);
            CHECK(net_post(bringup_again, 0, NULL, 0) == XIAOTAI_OK && settle());
            CHECK(atomic_load(&s_prepare_calls) == 1 && atomic_load(&s_prepare_manual));
            xiaotai_net_status(&status);
            CHECK(status.state == XIAOTAI_NET_UNCONFIGURED && records(XIAOTAI_NET_HOST_START, NULL) == 1);
            CHECK(status.prepare.stage == XIAOTAI_NET_PREPARE_READY);
        }
        teardown();
    }
    return 0;
}

static atomic_int s_restarts;
static atomic_bool s_restart_on_worker;
static void fake_restart(void)
{
    atomic_fetch_add(&s_restarts, 1);
    atomic_store(&s_restart_on_worker, xiaotai_net_host_on_worker());
}

static atomic_int s_tapped;
static uint32_t s_tapped_tokens[32];
static void tap(const xiaotai_net_driver_event_t *event, void *ctx)
{
    (void)ctx;
    const int index = atomic_fetch_add(&s_tapped, 1);
    if (index < 32) s_tapped_tokens[index] = event->token;
}

/* RESTARTING: the immediate restart on the worker, no driver start, and the
 * engine stays quiet afterwards. */
static int test_prepare_restart(void)
{
    parts(&s_coprocessor_part, NULL);
    const xiaotai_network_stage_t stages[] = {XIAOTAI_NETWORK_WRITING, XIAOTAI_NETWORK_RESTARTING};
    const uint8_t percents[] = {50, 100};
    script(XIAOTAI_OK, 2, stages, percents);
    atomic_store(&s_restarts, 0);
    xiaotai_net_host_set_restart(fake_restart);
    CHECK(boot(NULL, false) == XIAOTAI_OK);
    /* No barrier can pass the worker once it restarted: poll. */
    for (int i = 0; i < 200 && atomic_load(&s_restarts) == 0; ++i) xiaotai_delay_ms(10);
    CHECK(drain_owner());
    CHECK(atomic_load(&s_restarts) == 1 && atomic_load(&s_restart_on_worker));
    CHECK(records(XIAOTAI_NET_HOST_START, NULL) == 0);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_PREPARING && status.prepare.stage == XIAOTAI_NET_PREPARE_RESTARTING);
    /* Nothing else runs: a job is dropped, a blocking call is refused. */
    atomic_store(&s_tapped, 0);
    xiaotai_net_host_tap(tap, NULL);
    CHECK(net_post(bringup_again, 0, NULL, 0) == XIAOTAI_OK);
    CHECK(xiaotai_net_host_sync(1000) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(atomic_load(&s_prepare_calls) == 1 && atomic_load(&s_tapped) == 0);
    teardown();
    return 0;
}

/* A full queue: driver events and requests are dropped and counted, the
 * lost-event mark is raised once; after the worker drains, delivery is in
 * order and requests are accepted again. */
static int test_queue_full(void)
{
    parts(&s_wifi_part, NULL);
    const xiaotai_net_host_step_t hold = {.result = XIAOTAI_OK, .hold = true};
    CHECK(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &hold) == XIAOTAI_OK);
    CHECK(boot(NULL, false) == XIAOTAI_OK);
    CHECK(drain_owner());
    CHECK(xiaotai_net_host_wait_held(3000));   /* the worker sits in start() */
    atomic_store(&s_tapped, 0);
    xiaotai_net_host_tap(tap, NULL);

    for (uint32_t i = 1; i <= XIAOTAI_NET_QUEUE_DEPTH; ++i) {
        const xiaotai_net_driver_event_t lost = {
            .kind = XIAOTAI_NET_DRIVER_LOST, .token = i, .loss = XIAOTAI_NET_LOSS_SIGNAL};
        CHECK(xiaotai_net_host_emit(&lost) == XIAOTAI_OK);
    }
    xiaotai_net_diag_t diag;
    xiaotai_net_diag(&diag);
    CHECK(diag.dropped_events == 0);
    CHECK(!net_events_lost_take());
    const xiaotai_net_driver_event_t extra = {.kind = XIAOTAI_NET_DRIVER_LOST, .token = 99};
    CHECK(xiaotai_net_host_emit(&extra) == XIAOTAI_OK);
    xiaotai_net_diag(&diag);
    CHECK(diag.dropped_events == 1);
    CHECK(net_post(noop_job, 0, NULL, 0) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_net_host_sync(50) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_net_diag(&diag);
    CHECK(diag.dropped_events == 3);

    xiaotai_net_host_release();
    /* The barrier itself cannot enter a full queue: wait for the worker to
     * take the queued events first. */
    for (int i = 0; i < 300 && atomic_load(&s_tapped) < (int)XIAOTAI_NET_QUEUE_DEPTH; ++i) xiaotai_delay_ms(10);
    CHECK(settle());
    CHECK(net_events_lost_take());    /* raised by the dropped driver event */
    CHECK(!net_events_lost_take());   /* and only once */
    CHECK(atomic_load(&s_tapped) == (int)XIAOTAI_NET_QUEUE_DEPTH);
    for (uint32_t i = 0; i < XIAOTAI_NET_QUEUE_DEPTH; ++i) CHECK(s_tapped_tokens[i] == i + 1U);
    CHECK(net_post(noop_job, 0, NULL, 0) == XIAOTAI_OK);
    CHECK(xiaotai_net_host_sync(1000) == XIAOTAI_OK);
    xiaotai_net_diag(&diag);
    CHECK(diag.dropped_events == 3);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.state == XIAOTAI_NET_UNCONFIGURED);
    teardown();
    return 0;
}

static int test_event_packing(void)
{
    xiaotai_net_driver_event_t events[6];
    memset(events, 0, sizeof(events));
    events[0] = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = 7};
    memset(events[0].ssid, 'n', XIAOTAI_NET_SSID_MAX);   /* 32 bytes, any value but 0 */
    events[0].ssid[0] = 0x01;
    events[0].ssid[1] = (char)0xFF;
    events[1] = (xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = 8, .ipv4 = 0x0101A8C0U, .ipv6_valid = true};
    for (int i = 0; i < 16; ++i) events[1].ipv6[i] = (uint8_t)(0xF0 + i);
    events[2] = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST,
                                             .token = 0xFFFFFFFFU,
                                             .loss = XIAOTAI_NET_LOSS_SECURITY,
                                             .code = 211,
                                             .signal_valid = true,
                                             .signal_dbm = -91};
    events[3] = (xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = 3, .error = XIAOTAI_ERR_TIMEOUT};
    events[4] = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STOPPED};
    events[5] = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = 9, .ipv4 = 5};
    for (size_t i = 0; i < 6; ++i) {
        net_message_t message;
        xiaotai_net_driver_event_t back;
        memset(&back, 0x5A, sizeof(back));
        net_event_pack(&events[i], &message);
        CHECK(message.kind == NET_MSG_DRIVER);
        net_event_unpack(&message, &back);
        CHECK(event_equal(&back, &events[i]));
    }
    /* Fields a kind does not carry do not travel. */
    xiaotai_net_driver_event_t mixed = {
        .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = 1, .ipv4 = 9, .code = 4, .signal_valid = true};
    net_message_t message;
    xiaotai_net_driver_event_t back;
    net_event_pack(&mixed, &message);
    net_event_unpack(&message, &back);
    CHECK(back.ipv4 == 0 && back.code == 0 && !back.signal_valid && back.token == 1);
    CHECK(sizeof(net_message_t) == 64);
    return 0;
}

/* Jobs run on the worker in order with their copied arguments; the call
 * slot passes input and output, refuses the worker itself, times out without
 * losing the slot. */
static char s_job_trace[128];
static atomic_bool s_job_on_worker;
static void trace_job(const net_message_t *message)
{
    char text[NET_JOB_DATA_MAX + 1] = {0};
    memcpy(text, message->u.job.data, message->u.job.size);
    char entry[48];
    snprintf(entry, sizeof(entry), "%u:%s ", (unsigned)message->u.job.arg, text);
    strncat(s_job_trace, entry, sizeof(s_job_trace) - strlen(s_job_trace) - 1);
    atomic_store(&s_job_on_worker, xiaotai_net_host_on_worker());
}

static xiaotai_err_t reverse_call(net_call_t *call)
{
    for (size_t i = 0; i < call->size / 2; ++i) {
        const uint8_t byte = call->data[i];
        call->data[i] = call->data[call->size - 1 - i];
        call->data[call->size - 1 - i] = byte;
    }
    return (xiaotai_err_t)call->arg;
}

static atomic_llong s_slow_end_us;
static atomic_llong s_next_begin_us;
static xiaotai_err_t slow_call(net_call_t *call)
{
    xiaotai_delay_ms(300);
    call->size = 1;
    call->data[0] = 0x42;
    atomic_store(&s_slow_end_us, xiaotai_time_us());
    return XIAOTAI_OK;
}
static xiaotai_err_t stamp_call(net_call_t *call)
{
    atomic_store(&s_next_begin_us, xiaotai_time_us());
    call->size = 0;
    return XIAOTAI_OK;
}

static atomic_int s_nested_result;
static void nested_call_job(const net_message_t *message)
{
    (void)message;
    atomic_store(&s_nested_result, net_call(stamp_call, 0, NULL, 0, NULL, 0, NULL, 100));
}

static int test_jobs_and_calls(void)
{
    parts(&s_wifi_part, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    s_job_trace[0] = '\0';
    CHECK(net_post(trace_job, 1, "one", 3) == XIAOTAI_OK);
    CHECK(net_post(trace_job, 2, "two", 3) == XIAOTAI_OK);
    char full[NET_JOB_DATA_MAX] = {0};
    memset(full, 'x', sizeof(full));
    CHECK(net_post(trace_job, 3, full, sizeof(full)) == XIAOTAI_OK);
    CHECK(net_post(trace_job, 4, full, sizeof(full) + 1) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(net_post(NULL, 0, NULL, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(settle());
    CHECK(strcmp(s_job_trace, "1:one 2:two 3:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx ") == 0);
    CHECK(atomic_load(&s_job_on_worker));

    uint8_t out[8] = {0};
    size_t size = 99;
    CHECK(net_call(reverse_call, XIAOTAI_ERR_NOT_FOUND, "abcde", 5, out, sizeof(out), &size, 1000) ==
          XIAOTAI_ERR_NOT_FOUND);
    CHECK(size == 5 && memcmp(out, "edcba", 5) == 0);
    /* Output cut to the caller's capacity. */
    CHECK(net_call(reverse_call, XIAOTAI_OK, "abcde", 5, out, 2, &size, 1000) == XIAOTAI_OK);
    CHECK(size == 2 && memcmp(out, "ed", 2) == 0);
    uint8_t big[NET_CALL_DATA_MAX + 1] = {0};
    CHECK(net_call(reverse_call, XIAOTAI_OK, big, sizeof(big), NULL, 0, NULL, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    /* The worker never waits for itself. */
    atomic_store(&s_nested_result, 0);
    CHECK(net_post(nested_call_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(atomic_load(&s_nested_result) == XIAOTAI_ERR_INVALID_STATE);

    /* A timed-out call still runs; the next caller gets the slot only after it. */
    atomic_store(&s_slow_end_us, 0);
    const int64_t begun = xiaotai_time_us();
    CHECK(net_call(slow_call, 0, NULL, 0, out, sizeof(out), &size, 50) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_time_us() - begun < 250000);
    CHECK(size == 0);
    CHECK(net_call(stamp_call, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    CHECK(atomic_load(&s_slow_end_us) != 0 && atomic_load(&s_next_begin_us) >= atomic_load(&s_slow_end_us));
    /* A caller that cannot get the slot in time. */
    atomic_store(&s_slow_end_us, 0);
    CHECK(net_call(slow_call, 0, NULL, 0, NULL, 0, NULL, 20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(net_call(stamp_call, 0, NULL, 0, NULL, 0, NULL, 50) == XIAOTAI_ERR_TIMEOUT);
    CHECK(net_call(stamp_call, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    teardown();
    return 0;
}

static xiaotai_signal_t s_prior_entered, s_prior_release;
static xiaotai_signal_t s_owner_entered, s_owner_proceed, s_owner_returned;
static atomic_uint s_accepted_runs;
static xiaotai_err_t s_owner_result, s_prior_result;

static void block_prior_job(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_prior_entered);
    (void)xiaotai_signal_take(&s_prior_release, XIAOTAI_WAIT_FOREVER);
}

static xiaotai_err_t block_prior_call(net_call_t *call)
{
    (void)call;
    block_prior_job(NULL);
    return XIAOTAI_OK;
}

static void *external_call(void *unused)
{
    (void)unused;
    s_prior_result = net_call(block_prior_call, 0, NULL, 0, NULL, 0, NULL, 3000);
    return NULL;
}

static xiaotai_err_t count_owner_call(net_call_t *call)
{
    (void)call;
    atomic_fetch_add(&s_accepted_runs, 1);
    return XIAOTAI_OK;
}

static void try_owner_call(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&s_owner_entered);
    (void)xiaotai_signal_take(&s_owner_proceed, XIAOTAI_WAIT_FOREVER);
    s_owner_result = xiaotai_on_owner() && !net_on_worker() ?
        net_call(count_owner_call, 0, NULL, 0, NULL, 0, NULL, 50) : XIAOTAI_FAIL;
    xiaotai_signal_give(&s_owner_returned);
}

static int test_owner_call_deadline(void)
{
    parts(NULL, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_prior_entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_prior_release) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_owner_entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_owner_proceed) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_owner_returned) == XIAOTAI_OK);
    for (unsigned slot_taken = 0; slot_taken < 2; ++slot_taken) {
        atomic_store(&s_accepted_runs, 0);
        pthread_t prior;
        CHECK(xiaotai_run(try_owner_call, NULL) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&s_owner_entered, 2000));
        if (slot_taken) {
            CHECK(pthread_create(&prior, NULL, external_call, NULL) == 0);
            CHECK(xiaotai_signal_take(&s_prior_entered, 2000));
        } else {
            // The older FIFO message arrived during the owner action, before net_call().
            CHECK(net_post(block_prior_job, 0, NULL, 0) == XIAOTAI_OK);
        }
        xiaotai_signal_give(&s_owner_proceed);
        if (!slot_taken) CHECK(xiaotai_signal_take(&s_prior_entered, 2000));
        // The owner must be able to return while earlier network I/O is still blocked.
        const bool returned = xiaotai_signal_take(&s_owner_returned, 1000);
        const unsigned before_release = atomic_load(&s_accepted_runs);
        xiaotai_signal_give(&s_prior_release);
        if (slot_taken) CHECK(pthread_join(prior, NULL) == 0 && s_prior_result == XIAOTAI_OK);
        CHECK(returned && s_owner_result == XIAOTAI_ERR_TIMEOUT && before_release == 0U);
        CHECK(settle());
        // A queued call still runs once; a call that missed admission does not.
        CHECK(atomic_load(&s_accepted_runs) == (slot_taken ? 0U : 1U));
    }
    xiaotai_signal_deinit(&s_owner_returned);
    xiaotai_signal_deinit(&s_owner_proceed);
    xiaotai_signal_deinit(&s_owner_entered);
    xiaotai_signal_deinit(&s_prior_release);
    xiaotai_signal_deinit(&s_prior_entered);
    teardown();
    return 0;
}

/* Deadlines on absolute times: each runs on the worker, never before its
 * time, nearest first; a cleared one never runs; re-arming replaces. */
static int64_t s_fired_at[NET_DEADLINE_COUNT];
static atomic_int s_fired_order[4];
static atomic_int s_fired_count;
static atomic_bool s_fired_off_worker;
static int64_t s_due[NET_DEADLINE_COUNT];

static void fired(net_deadline_t which)
{
    s_fired_at[which] = xiaotai_time_us();
    if (!xiaotai_net_host_on_worker()) atomic_store(&s_fired_off_worker, true);
    const int index = atomic_fetch_add(&s_fired_count, 1);
    if (index < 4) atomic_store(&s_fired_order[index], (int)which);
}
static void fired_backoff(void) { fired(NET_DEADLINE_BACKOFF); }
static void fired_signal(void) { fired(NET_DEADLINE_SIGNAL); }
static void fired_scan(void) { fired(NET_DEADLINE_SCAN); }
static void fired_verify(void) { fired(NET_DEADLINE_VERIFY); }

static void arm_job(const net_message_t *message)
{
    (void)message;
    const int64_t now = xiaotai_time_us();
    s_due[NET_DEADLINE_BACKOFF] = now + 120000;
    s_due[NET_DEADLINE_SIGNAL] = now + 40000;
    s_due[NET_DEADLINE_SCAN] = now + 60000;
    net_deadline_set(NET_DEADLINE_BACKOFF, s_due[NET_DEADLINE_BACKOFF], fired_backoff);
    net_deadline_set(NET_DEADLINE_SIGNAL, now + 10000, fired_signal);
    net_deadline_set(NET_DEADLINE_SIGNAL, s_due[NET_DEADLINE_SIGNAL], fired_signal);   /* replaces */
    net_deadline_set(NET_DEADLINE_SCAN, s_due[NET_DEADLINE_SCAN], fired_scan);
    net_deadline_set(NET_DEADLINE_VERIFY, now + 20000, fired_verify);
    net_deadline_clear(NET_DEADLINE_VERIFY);
}

static int64_t s_seen_due;
static void read_due_job(const net_message_t *message)
{
    (void)message;
    s_seen_due = net_deadline_due(NET_DEADLINE_BACKOFF);
}

static int test_deadlines(void)
{
    parts(&s_wifi_part, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    memset(s_fired_at, 0, sizeof(s_fired_at));
    atomic_store(&s_fired_count, 0);
    atomic_store(&s_fired_off_worker, false);
    CHECK(net_post(arm_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(net_post(read_due_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(s_seen_due == s_due[NET_DEADLINE_BACKOFF]);
    for (int i = 0; i < 100 && atomic_load(&s_fired_count) < 3; ++i) xiaotai_delay_ms(10);
    xiaotai_delay_ms(50);   /* the cleared one would have been due long ago */
    CHECK(atomic_load(&s_fired_count) == 3);
    CHECK(!atomic_load(&s_fired_off_worker));
    CHECK(atomic_load(&s_fired_order[0]) == NET_DEADLINE_SIGNAL);
    CHECK(atomic_load(&s_fired_order[1]) == NET_DEADLINE_SCAN);
    CHECK(atomic_load(&s_fired_order[2]) == NET_DEADLINE_BACKOFF);
    CHECK(s_fired_at[NET_DEADLINE_SIGNAL] >= s_due[NET_DEADLINE_SIGNAL]);
    CHECK(s_fired_at[NET_DEADLINE_SCAN] >= s_due[NET_DEADLINE_SCAN]);
    CHECK(s_fired_at[NET_DEADLINE_BACKOFF] >= s_due[NET_DEADLINE_BACKOFF]);
    CHECK(s_fired_at[NET_DEADLINE_VERIFY] == 0);
    CHECK(net_post(read_due_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(s_seen_due == 0);   /* a deadline that ran is clear */
    teardown();
    return 0;
}

/* Commits: revision, events and their values; generations and tokens. */
static uint32_t s_generations[3];
static uint32_t s_tokens[2];
static uint32_t s_revision_before, s_revision_after;
static void commit_job(const net_message_t *message)
{
    (void)message;
    xiaotai_net_status_t *status = net_status_edit();
    s_revision_before = status->revision;
    net_snapshot_cancel();

    s_generations[0] = net_generation_issue();
    status = net_status_edit();
    status->state = XIAOTAI_NET_UP;
    status->generation = s_generations[0];
    status->ipv4 = 0x0201A8C0U;
    net_status_commit(NET_EMIT_UP);
    status = net_status_edit();
    status->signal_valid = true;
    status->signal_dbm = -63;
    net_status_commit(NET_EMIT_SIGNAL);
    net_generation_clear();
    status = net_status_edit();
    status->generation = 0;
    status->signal_valid = false;
    net_status_commit(NET_EMIT_DOWN);
    net_set_state(XIAOTAI_NET_WAITING);
    net_set_state(XIAOTAI_NET_WAITING);   /* no change, no event */
    s_generations[1] = net_generation_issue();
    s_generations[2] = net_generation_issue();
    net_generation_clear();
    s_tokens[0] = net_token_next();
    s_tokens[1] = net_token_next();

    xiaotai_net_provision_status_t *provision = net_provision_edit();
    provision->state = XIAOTAI_NET_PROVISION_OPEN;
    net_provision_commit(true);
    provision = net_provision_edit();
    provision->methods = XIAOTAI_PROVISION_PORTAL;
    net_provision_commit(false);

    status = net_status_edit();
    s_revision_after = status->revision;
    net_snapshot_cancel();
}

static int test_commits(void)
{
    parts(&s_wifi_part, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    s_event_count = 0;
    CHECK(net_post(commit_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(s_revision_after == s_revision_before + 4);
    CHECK(s_generations[0] != 0 && s_generations[1] != 0 && s_generations[2] != 0);
    CHECK(s_generations[1] != s_generations[0] && s_generations[2] != s_generations[1]);
    CHECK(xiaotai_net_generation() == 0);
    CHECK(s_tokens[0] != 0 && s_tokens[1] != 0 && s_tokens[1] != s_tokens[0]);

    CHECK(s_event_count == 5);
    CHECK(strcmp(event_at(0)->name, XIAOTAI_EVENT_NET_UP) == 0);
    CHECK(event_at(0)->value == (int32_t)(s_generations[0] & 0x7FFFFFFFU));
    CHECK(event_at(0)->data.status.generation == s_generations[0] && event_at(0)->data.status.ipv4 == 0x0201A8C0U);
    CHECK(strcmp(event_at(1)->name, XIAOTAI_EVENT_NET_SIGNAL) == 0 && event_at(1)->value == -63);
    CHECK(strcmp(event_at(2)->name, XIAOTAI_EVENT_NET_DOWN) == 0 && event_at(2)->value == 0);
    CHECK(event_at(2)->data.status.generation == 0);
    CHECK(strcmp(event_at(3)->name, XIAOTAI_EVENT_NET_STATE) == 0 && event_at(3)->value == XIAOTAI_NET_WAITING);
    CHECK(strcmp(event_at(4)->name, XIAOTAI_EVENT_NET_PROVISION) == 0);
    CHECK(event_at(4)->value == XIAOTAI_NET_PROVISION_OPEN && event_at(4)->size == sizeof(xiaotai_net_provision_status_t));
    xiaotai_net_provision_status_t provision;
    xiaotai_net_provision_status(&provision);
    CHECK(provision.revision == 2 && provision.methods == XIAOTAI_PROVISION_PORTAL);
    CHECK(provision.state == XIAOTAI_NET_PROVISION_OPEN);
    teardown();

    /* After reset generations and tokens start over and the snapshots read zero. */
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    CHECK(status.revision == 0 && status.state == XIAOTAI_NET_STOPPED);
    parts(&s_wifi_part, NULL);
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(net_post(commit_job, 0, NULL, 0) == XIAOTAI_OK && settle());
    CHECK(s_tokens[0] == 1 && s_generations[0] == 1);
    teardown();
    return 0;
}

/* Reset stops the worker and releases every allocation; a failed allocation
 * at any step of a start leaves nothing behind. */
static int test_reset_and_failed_starts(void)
{
    parts(&s_coprocessor_part, NULL);
    const xiaotai_network_stage_t ready[] = {XIAOTAI_NETWORK_READY};
    const uint8_t full[] = {100};
    script(XIAOTAI_OK, 1, ready, full);
    xiaotai_mem_host_clear();
    CHECK(boot(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_outstanding() > 0);
    teardown();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    CHECK(xiaotai_net_driver_bound() == NULL && !net_gate_bringup());
    CHECK(xiaotai_net_configure(NULL) == XIAOTAI_OK);

    uint32_t failed = 0;
    for (uint32_t step = 1; step < 64; ++step) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(step);
        s_event_count = 0;
        const xiaotai_err_t started = xiaotai_framework_start(&FRAMEWORK);
        const bool reached = !xiaotai_mem_host_fail_pending();
        xiaotai_mem_host_fail_nth(0);
        if (started != XIAOTAI_OK) {
            ++failed;
            CHECK(xiaotai_net_driver_bound() == NULL);
        } else {
            CHECK(settle());
        }
        teardown();
        if (xiaotai_mem_host_outstanding() != 0) {
            fprintf(stderr, "allocation %u: %lld blocks left\n", (unsigned)step,
                    (long long)xiaotai_mem_host_outstanding());
            return 1;
        }
        if (!reached) {
            CHECK(started == XIAOTAI_OK);
            break;
        }
        CHECK(step < 63);
    }
    CHECK(failed >= 2);   /* at least the engine's state and its queue */
    return 0;
}

int main(int argc, char **argv)
{
    if (xiaotai_signal_init(&s_sync) != XIAOTAI_OK) return 1;
    if (argc == 2 && !strcmp(argv[1], "owner-deadline")) {
        RUN(test_owner_call_deadline);
        xiaotai_signal_deinit(&s_sync);
        puts("owner-deadline: timeout without inline work; queued vs unadmitted distinguished");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "companion-start-failure")) {
        RUN(test_companion_start_failure);
        xiaotai_signal_deinit(&s_sync);
        return 0;
    }
    RUN(test_before_init);
    RUN(test_configure);
    RUN(test_driver_registration);
    RUN(test_event_packing);
    RUN(test_boot_wifi);
    RUN(test_parts_without_driver);
    RUN(test_driver_replacement);
    RUN(test_start_failure);
    RUN(test_prepare_ready);
    RUN(test_prepare_faults);
    RUN(test_prepare_restart);
    RUN(test_queue_full);
    RUN(test_jobs_and_calls);
    RUN(test_deadlines);
    RUN(test_commits);
    RUN(test_reset_and_failed_starts);
    puts("net_lifecycle: ok");
    return 0;
}
