// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_service_environment.py
// Replaces legacy: tests/regression/test_service_environment_store.py
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
/* The stored environment base (src/environment.c) on the real framework and
 * the host storage port, ported from tests/environment/test_environment.c,
 * test_environment_store.c and tests/regression/test_service_environment*.py
 * (rules part):
 *   - the base rules of xiaotai_platform_environment_normalize, with the
 *     exact length bound;
 *   - boot: the built-in URL when nothing is stored (and nothing written),
 *     the stored base + "/services" otherwise, a stored legacy spelling
 *     normalized, and every read failure or corrupt record a boot error with
 *     no fallback to the built-in URL;
 *   - store and reset: normalized write, read-back, snapshot, a reboot that
 *     reads the new value; done on the owner task, from the owner task and
 *     from another task;
 *   - failures: a write error, a write that times out (and still lands), a
 *     read-back that times out, fails or differs set `unconfirmed` and leave
 *     saved/next alone until a confirmed store; a store refused before
 *     anything reached storage leaves `unconfirmed` alone;
 *   - a store whose completion cannot be handed to a full owner queue still
 *     completes through its step deadline;
 *   - one store at a time; no port allocation for boot or store.
 * Deadlines are measured against the monotonic clock of the store's start. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "platform_internal.h"
#include "platform_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"
#include "kv_backend.h"

#define NS PLATFORM_ENVIRONMENT_NAMESPACE
#define KEY PLATFORM_ENVIRONMENT_KEY
#define BUILTIN "https://default.test:8080/custom/services"
#define BUILTIN_HTTP "http://default.test:8080/custom/services"
#define STEP_MS 150U

static atomic_uint s_corrupt_read;
static atomic_uint s_text_reads;
static atomic_bool s_timeout_after_commit;

xiaotai_err_t __real_xiaotai_kv_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text, void *data,
                                           size_t *size);
xiaotai_err_t __real_xiaotai_kv_backend_get_str_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                      size_t *size);
xiaotai_err_t __real_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);

/* Corruption enters below both real KV read paths. */
static void corrupt_read(const char *key, xiaotai_err_t result, char *data, size_t capacity, size_t *size)
{
    if (strcmp(key, KEY) != 0) return;
    atomic_fetch_add(&s_text_reads, 1);
    if (result != XIAOTAI_OK) return;
    switch (atomic_exchange(&s_corrupt_read, 0)) {
    case 1: *size = 0; break;
    case 2: memset(data, 'x', capacity); *size = capacity; break;
    case 3: CHECK(*size + 2U <= capacity); memcpy(data + *size, "x", 2); *size += 2U; break;
    case 4: *size = capacity + 1U; break;
    case 5: data[0] = '\0'; *size = 1; break;
    default: break;
    }
}

xiaotai_err_t __wrap_xiaotai_kv_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text, void *data,
                                           size_t *size)
{
    const size_t capacity = *size;
    const xiaotai_err_t result = __real_xiaotai_kv_backend_get(handle, key, text, data, size);
    if (text) corrupt_read(key, result, data, capacity, size);
    return result;
}

xiaotai_err_t __wrap_xiaotai_kv_backend_get_str_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                      size_t *size)
{
    const size_t capacity = *size;
    const xiaotai_err_t result = __real_xiaotai_kv_backend_get_str_strict(handle, key, data, size);
    corrupt_read(key, result, data, capacity, size);
    return result;
}

xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    const xiaotai_err_t result = __real_xiaotai_kv_backend_commit(handle);
    if (result == XIAOTAI_OK && atomic_exchange(&s_timeout_after_commit, false)) return XIAOTAI_ERR_TIMEOUT;
    return result;
}

/* ------------------------------------------------------------- framework */

static const char *s_builtin;
static xiaotai_err_t s_boot_result;
static uint32_t s_boot_allocations;

/* Stands in for the engine's init, which reads the environment. */
static xiaotai_err_t engine_init(void)
{
    xiaotai_mem_host_clear();
    s_boot_result = platform_environment_boot(s_builtin);
    s_boot_allocations = xiaotai_mem_host_allocations();
    return XIAOTAI_OK;
}

static const xiaotai_module_t s_engine = {.name = "platform", .init = engine_init};
static const xiaotai_module_t *const s_modules[] = {&s_engine};

static bool boot(const char *builtin)
{
    s_builtin = builtin;
    platform_environment_reset();
    platform_environment_set_step_ms(STEP_MS);
    xiaotai_framework_config_t config = {.modules = s_modules, .module_count = 1, .available_needs = UINT32_MAX};
    return xiaotai_framework_start(&config) == XIAOTAI_OK;
}

static void shutdown(void)
{
    (void)xiaotai_kv_flush(5000);
    xiaotai_framework_reset();
}

static xiaotai_signal_t s_sync;
static void signal_sync(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_sync);
}

/* Everything queued on the owner before this point has run. */
static bool drain(void)
{
    return xiaotai_run(signal_sync, NULL) == XIAOTAI_OK && xiaotai_signal_take(&s_sync, 3000);
}

/* ----------------------------------------------------------------- store */

typedef struct {
    atomic_int calls;
    xiaotai_err_t result;
    bool on_owner;
    int64_t at_us;
    xiaotai_platform_environment_t snapshot; /* as seen by done */
} completion_t;

static completion_t s_done;
static xiaotai_signal_t s_done_signal;

static void on_done(xiaotai_err_t result, void *ctx)
{
    CHECK(ctx == &s_done);
    s_done.result = result;
    s_done.on_owner = xiaotai_on_owner();
    s_done.at_us = xiaotai_time_us();
    xiaotai_platform_environment(&s_done.snapshot);
    atomic_fetch_add(&s_done.calls, 1);
    xiaotai_signal_give(&s_done_signal);
}

static void expect_clear(void)
{
    memset(&s_done, 0, sizeof(s_done));
    while (xiaotai_signal_take(&s_done_signal, 0)) {
    }
}

static bool wait_done(uint32_t timeout_ms) { return xiaotai_signal_take(&s_done_signal, timeout_ms); }

/* Store from the owner task, as the environment module does. */
typedef struct {
    const char *base;
    xiaotai_err_t returned;
} owner_store_t;

static void store_on_owner(void *ctx)
{
    owner_store_t *call = ctx;
    call->returned = xiaotai_platform_environment_store(call->base, on_done, &s_done);
}

static xiaotai_err_t owner_store(const char *base)
{
    owner_store_t call = {.base = base, .returned = 12345};
    CHECK(xiaotai_run(store_on_owner, &call) == XIAOTAI_OK);
    CHECK(drain());
    return call.returned;
}

static void stored(char *out, size_t capacity, xiaotai_err_t *err)
{
    out[0] = '\0';
    *err = xiaotai_kv_get_str(NS, KEY, out, capacity, 2000);
}

static void put(const char *value)
{
    CHECK(xiaotai_kv_set_str(NS, KEY, value, 2000) == XIAOTAI_OK);
}

static uint32_t writes(void)
{
    xiaotai_kv_host_counts_t counts;
    xiaotai_kv_host_counts(&counts);
    return counts.writes;
}

static void snapshot(xiaotai_platform_environment_t *env) { xiaotai_platform_environment(env); }

/* ----------------------------------------------------------------- tests */

static void normalize(void)
{
    static const char *const good[][2] = {
        {"http://host.test", "http://host.test"},
        {"http://host.test/", "http://host.test"},
        {"HTTPS://host.test:8443/pre///", "http://host.test:8443/pre"},
        {"http://127.0.0.1:8080/pre", "http://127.0.0.1:8080/pre"},
        {"http://[::1]:8080/tenant/v2", "http://[::1]:8080/tenant/v2"},
        {"http://host.test/pre%20two", "http://host.test/pre%20two"},
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        char out[XIAOTAI_PLATFORM_BASE_MAX + 1];
        CHECK_TEXT(xiaotai_platform_environment_normalize(good[i][0], out, sizeof(out)) == XIAOTAI_OK, good[i][0]);
        CHECK_TEXT(strcmp(out, good[i][1]) == 0, good[i][0]);
        /* Idempotent. */
        char again[XIAOTAI_PLATFORM_BASE_MAX + 1];
        CHECK_TEXT(xiaotai_platform_environment_normalize(out, again, sizeof(again)) == XIAOTAI_OK, out);
        CHECK_TEXT(strcmp(again, out) == 0, out);
        CHECK_TEXT(xiaotai_platform_environment_normalize(good[i][0], NULL, 0) == XIAOTAI_OK, good[i][0]);
        strcpy(again, good[i][0]);
        CHECK_TEXT(xiaotai_platform_environment_normalize(again, again, sizeof(again)) == XIAOTAI_OK, good[i][0]);
        CHECK_TEXT(strcmp(again, good[i][1]) == 0, good[i][0]);
    }
    static const char *const bad[] = {
        "", "http://", "mqtt://host", "ftp://host", "http://user@host", "http://user:password@host",
        "http://host:0", "http://host:65536", "http://host:port", "http://[::1", "http://host/pre?token=secret",
        "http://host/pre#part", "http://host\\pre", "http://host name", "http://host/pre\n", "http://host/pre\177",
        "http://host/pre\t",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char out[XIAOTAI_PLATFORM_BASE_MAX + 1];
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(xiaotai_platform_environment_normalize(bad[i], out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG,
                   bad[i]);
        CHECK_TEXT(out[0] == '\0', bad[i]);
    }
    CHECK(xiaotai_platform_environment_normalize(NULL, NULL, 0) == XIAOTAI_ERR_INVALID_ARG);
    /* 246 characters pass and make a 255-character discovery URL; 247 do not. */
    char longest[XIAOTAI_PLATFORM_BASE_MAX + 2];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest, "http://host/", 12);
    longest[XIAOTAI_PLATFORM_BASE_MAX] = '\0';
    char out[XIAOTAI_PLATFORM_BASE_MAX + 1];
    CHECK(xiaotai_platform_environment_normalize(longest, out, sizeof(out)) == XIAOTAI_OK);
    CHECK(strlen(out) == XIAOTAI_PLATFORM_BASE_MAX);
    CHECK(xiaotai_platform_environment_normalize(longest, out, XIAOTAI_PLATFORM_BASE_MAX) ==
          XIAOTAI_ERR_INVALID_SIZE);
    longest[XIAOTAI_PLATFORM_BASE_MAX] = 'a';
    longest[XIAOTAI_PLATFORM_BASE_MAX + 1] = '\0';
    CHECK(xiaotai_platform_environment_normalize(longest, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    /* An https base one character too long is refused although its http
     * spelling would fit: the input is what is bounded. */
    char https[XIAOTAI_PLATFORM_BASE_MAX + 2];
    memset(https, 'a', sizeof(https));
    memcpy(https, "https://host/", 13);
    https[XIAOTAI_PLATFORM_BASE_MAX + 1] = '\0';
    CHECK(xiaotai_platform_environment_normalize(https, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
}

static void default_boot(void)
{
    xiaotai_kv_host_clear();
    uint32_t before = writes();
    CHECK(boot(BUILTIN));
    CHECK(s_boot_result == XIAOTAI_OK && s_boot_allocations == 0);
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(strcmp(env.active, BUILTIN_HTTP) == 0);
    CHECK(env.saved[0] == '\0' && strcmp(env.next, BUILTIN_HTTP) == 0);
    CHECK(env.boot_error == XIAOTAI_OK && !env.unconfirmed);
    char url[XIAOTAI_PLATFORM_URL_MAX];
    CHECK(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_OK && strcmp(url, BUILTIN_HTTP) == 0);
    CHECK(platform_environment_discovery_url(url, 10) == XIAOTAI_ERR_INVALID_SIZE);
    /* Reading the default writes nothing. */
    CHECK(writes() == before);
    shutdown();

    /* No built-in URL and nothing stored: no discovery URL at all. */
    CHECK(boot(NULL));
    snapshot(&env);
    CHECK(env.active[0] == '\0' && env.next[0] == '\0' && env.boot_error == XIAOTAI_OK);
    CHECK(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_ERR_NOT_FOUND);
    shutdown();

    /* An inadmissible built-in URL counts as none. */
    CHECK(boot("ftp://default.test/services"));
    CHECK(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_ERR_NOT_FOUND);
    shutdown();

    /* Before boot nothing is known and nothing can be stored. */
    platform_environment_reset();
    snapshot(&env);
    CHECK(env.active[0] == '\0' && env.boot_error == XIAOTAI_OK);
    CHECK(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_platform_environment_store("http://x.test", NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
}

static void stored_boot(void)
{
    xiaotai_kv_host_clear();
    (void)xiaotai_kv_init();
    put("http://test.test:8081/pre");
    CHECK(boot(BUILTIN));
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(strcmp(env.active, "http://test.test:8081/pre/services") == 0);
    CHECK(strcmp(env.saved, "http://test.test:8081/pre") == 0);
    CHECK(strcmp(env.next, "http://test.test:8081/pre/services") == 0);
    shutdown();

    /* A stored spelling that is not normalized is read normalized. */
    put("HTTPS://legacy.test:8443/pre//");
    CHECK(boot(BUILTIN));
    snapshot(&env);
    CHECK(strcmp(env.active, "http://legacy.test:8443/pre/services") == 0);
    CHECK(strcmp(env.saved, "http://legacy.test:8443/pre") == 0);
    shutdown();

    /* A key of the wrong type is corrupt; only a missing key gets the default. */
    xiaotai_kv_host_clear();
    (void)xiaotai_kv_init();
    static const char blob[] = "http://blob.test";
    CHECK(xiaotai_kv_set(NS, KEY, blob, sizeof(blob), 2000) == XIAOTAI_OK);
    const uint32_t before = writes();
    CHECK(boot(BUILTIN));
    snapshot(&env);
    CHECK(s_boot_result == XIAOTAI_ERR_INVALID_ARG && env.boot_error == XIAOTAI_ERR_INVALID_ARG);
    CHECK(!env.active[0] && !env.saved[0] && !env.next[0] && writes() == before);
    char url[XIAOTAI_PLATFORM_URL_MAX], contents[sizeof(blob)];
    size_t size = sizeof(contents);
    CHECK(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_ERR_INVALID_ARG && !url[0]);
    CHECK(xiaotai_kv_get(NS, KEY, contents, &size, 2000) == XIAOTAI_OK && size == sizeof(blob));
    CHECK(!memcmp(contents, blob, sizeof(blob)));
    shutdown();
}

static void boot_errors(void)
{
    /* Corrupt records: empty, user information, not a URL, too long. Each is
     * a boot error; the built-in URL is never used instead. */
    static const char *const corrupt[] = {"", "http://user@host", "invalid", "http://host?x"};
    for (size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        xiaotai_kv_host_clear();
        (void)xiaotai_kv_init();
        put(corrupt[i]);
        CHECK(boot(BUILTIN));
        CHECK_TEXT(s_boot_result == XIAOTAI_FAIL, corrupt[i]);
        xiaotai_platform_environment_t env;
        snapshot(&env);
        CHECK_TEXT(env.boot_error == XIAOTAI_FAIL, corrupt[i]);
        CHECK_TEXT(env.active[0] == '\0' && env.saved[0] == '\0' && env.next[0] == '\0', corrupt[i]);
        char url[XIAOTAI_PLATFORM_URL_MAX];
        CHECK_TEXT(platform_environment_discovery_url(url, sizeof(url)) == XIAOTAI_FAIL && url[0] == '\0',
                   corrupt[i]);
        shutdown();
    }
    xiaotai_kv_host_clear();
    (void)xiaotai_kv_init();
    char longer[XIAOTAI_PLATFORM_BASE_MAX + 2];
    memset(longer, 'a', sizeof(longer));
    memcpy(longer, "http://host/", 12);
    longer[XIAOTAI_PLATFORM_BASE_MAX + 1] = '\0';
    put(longer);
    CHECK(boot(BUILTIN));
    CHECK(s_boot_result == XIAOTAI_ERR_INVALID_SIZE);
    shutdown();

    /* Storage errors at boot are boot errors, not "not stored". */
    static const xiaotai_err_t errors[] = {XIAOTAI_FAIL, XIAOTAI_ERR_TIMEOUT, XIAOTAI_ERR_INVALID_SIZE,
                                            XIAOTAI_ERR_INVALID_STATE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        xiaotai_kv_host_clear();
        xiaotai_kv_host_fail(errors[i]);
        CHECK(boot(BUILTIN));
        CHECK(s_boot_result == errors[i]);
        xiaotai_platform_environment_t env;
        snapshot(&env);
        CHECK(env.boot_error == errors[i] && env.active[0] == '\0');
        xiaotai_kv_host_fail(XIAOTAI_OK);
        /* Still repairable in the same boot: the store works and is shown. */
        expect_clear();
        CHECK(xiaotai_platform_environment_store("http://repair.test/", on_done, &s_done) == XIAOTAI_OK);
        CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK);
        snapshot(&env);
        CHECK(env.boot_error == errors[i] && env.active[0] == '\0');
        CHECK(strcmp(env.saved, "http://repair.test") == 0);
        CHECK(strcmp(env.next, "http://repair.test/services") == 0);
        shutdown();
        /* The next boot reads the repaired value. */
        CHECK(boot(BUILTIN));
        CHECK(s_boot_result == XIAOTAI_OK);
        snapshot(&env);
        CHECK(strcmp(env.active, "http://repair.test/services") == 0);
        shutdown();
    }
}

static void store_and_reset(void)
{
    xiaotai_kv_host_clear();
    CHECK(boot(BUILTIN));

    /* From another task: accepted at once, done later on the owner task. */
    expect_clear();
    xiaotai_mem_host_clear();
    CHECK(xiaotai_platform_environment_store("https://test.test:8081/pre/", on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000));
    CHECK(s_done.result == XIAOTAI_OK && s_done.on_owner && atomic_load(&s_done.calls) == 1);
    /* done already sees the new snapshot. */
    CHECK(strcmp(s_done.snapshot.saved, "http://test.test:8081/pre") == 0);
    CHECK(xiaotai_mem_host_allocations() == 0);
    char value[XIAOTAI_PLATFORM_BASE_MAX + 1];
    xiaotai_err_t err;
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_OK && strcmp(value, "http://test.test:8081/pre") == 0);
    xiaotai_platform_environment_t env;
    snapshot(&env);
    /* This boot keeps its environment; the next one takes the new one. */
    CHECK(strcmp(env.active, BUILTIN_HTTP) == 0);
    CHECK(strcmp(env.saved, "http://test.test:8081/pre") == 0);
    CHECK(strcmp(env.next, "http://test.test:8081/pre/services") == 0);
    CHECK(!env.unconfirmed);

    /* From the owner task: the call returns, done follows. */
    expect_clear();
    CHECK(owner_store("http://owner.test:9000/x") == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK && s_done.on_owner);
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_OK && strcmp(value, "http://owner.test:9000/x") == 0);
    shutdown();

    /* A restart reads it; twice the same. */
    CHECK(boot(BUILTIN));
    xiaotai_platform_environment_t first, second;
    snapshot(&first);
    CHECK(strcmp(first.active, "http://owner.test:9000/x/services") == 0);
    shutdown();
    CHECK(boot(BUILTIN));
    snapshot(&second);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);

    /* Reset removes the key only, and done is NULL-safe. */
    CHECK(xiaotai_kv_set_str("wifi", "ssid", "unchanged", 2000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str(NS, "keep", "unchanged", 2000) == XIAOTAI_OK);
    expect_clear();
    CHECK(xiaotai_platform_environment_store(NULL, on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK);
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_str("wifi", "ssid", value, sizeof(value), 2000) == XIAOTAI_OK &&
          strcmp(value, "unchanged") == 0);
    CHECK(xiaotai_kv_get_str(NS, "keep", value, sizeof(value), 2000) == XIAOTAI_OK &&
          strcmp(value, "unchanged") == 0);
    snapshot(&env);
    CHECK(env.saved[0] == '\0' && strcmp(env.next, BUILTIN_HTTP) == 0);
    CHECK(strcmp(env.active, "http://owner.test:9000/x/services") == 0);
    /* Resetting what is not stored succeeds. */
    CHECK(xiaotai_platform_environment_store(NULL, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    snapshot(&env);
    CHECK(env.saved[0] == '\0' && !env.unconfirmed);
    shutdown();
    CHECK(boot(BUILTIN));
    snapshot(&env);
    CHECK(strcmp(env.active, BUILTIN_HTTP) == 0);
    shutdown();
}

static void refused(void)
{
    xiaotai_kv_host_clear();
    CHECK(boot(BUILTIN));
    uint32_t before = writes();
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://bad?token", on_done, &s_done) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(owner_store("http://user@host") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    CHECK(writes() == before && atomic_load(&s_done.calls) == 0);

    /* One store at a time. */
    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://first.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_platform_environment_store("http://second.test", on_done, &s_done) == XIAOTAI_ERR_BUSY);
    xiaotai_kv_host_gate(true);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK && atomic_load(&s_done.calls) == 1);

    /* Refused by the storage port before anything was queued: from the
     * owner task the call returns the error and done never runs; from
     * another task done reports it. Either way storage is known. */
    xiaotai_kv_host_gate(false);
    unsigned filled = 0;
    for (; filled < 64; ++filled) {
        char key[8];
        snprintf(key, sizeof(key), "k%u", filled);
        if (xiaotai_kv_set_str_async("t_fill", key, "v") != XIAOTAI_OK) break;
    }
    CHECK(filled > 0 && filled < 64);
    expect_clear();
    CHECK(owner_store("http://refused.test") == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_platform_environment_store("http://refused.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_NO_MEM && s_done.on_owner);
    CHECK(!s_done.snapshot.unconfirmed);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(!env.unconfirmed && strcmp(env.saved, "http://first.test") == 0);
    CHECK(atomic_load(&s_done.calls) == 1);
    shutdown();
}

static void block_owner(void *ctx) { (void)xiaotai_signal_take((xiaotai_signal_t *)ctx, 10000); }

static void wait_writes(uint32_t count)
{
    for (int i = 0; i < 300 && writes() < count; ++i) {
        xiaotai_delay_ms(10);
    }
    CHECK(writes() >= count);
}

static void expect_unconfirmed(const char *saved)
{
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(env.unconfirmed);
    CHECK(strcmp(env.saved, saved) == 0);
}

static void failures(void)
{
    xiaotai_kv_host_clear();
    (void)xiaotai_kv_init();
    put("http://old.test");
    CHECK(boot(BUILTIN));
    char value[XIAOTAI_PLATFORM_BASE_MAX + 1];
    xiaotai_err_t err;

    /* A failed write does not read back or retry, even if it reports timeout. */
    static const xiaotai_err_t errors[] = {XIAOTAI_FAIL, XIAOTAI_ERR_TIMEOUT, XIAOTAI_ERR_INVALID_SIZE,
                                            XIAOTAI_ERR_INVALID_STATE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        const uint32_t before_writes = writes();
        const unsigned before_reads = atomic_load(&s_text_reads);
        xiaotai_kv_host_fail_key(KEY, errors[i]);
        expect_clear();
        CHECK(xiaotai_platform_environment_store("http://new.test", on_done, &s_done) == XIAOTAI_OK);
        CHECK(wait_done(3000) && s_done.result == errors[i] && s_done.snapshot.unconfirmed);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
        CHECK(writes() == before_writes + 1 && atomic_load(&s_text_reads) == before_reads);
        stored(value, sizeof(value), &err);
        CHECK(err == XIAOTAI_OK && strcmp(value, "http://old.test") == 0);
        expect_unconfirmed("http://old.test");
    }
    /* A confirmed store clears it. */
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://good.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK && !s_done.snapshot.unconfirmed);
    CHECK(strcmp(s_done.snapshot.saved, "http://good.test") == 0);

    /* The write times out after one step; it still lands later, unconfirmed. */
    const uint32_t late_writes = writes();
    const unsigned late_reads = atomic_load(&s_text_reads);
    xiaotai_kv_host_gate(false);
    expect_clear();
    int64_t start = xiaotai_time_us();
    CHECK(xiaotai_platform_environment_store("http://late.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_TIMEOUT);
    CHECK(s_done.at_us - start >= (int64_t)STEP_MS * 1000 && s_done.at_us - start < 2000000);
    CHECK(s_done.snapshot.unconfirmed && strcmp(s_done.snapshot.saved, "http://good.test") == 0);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    CHECK(writes() == late_writes + 1 && atomic_load(&s_text_reads) == late_reads);
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_OK && strcmp(value, "http://late.test") == 0);
    /* The late completion changes nothing and runs no second done. */
    CHECK(atomic_load(&s_done.calls) == 1);
    expect_unconfirmed("http://good.test");

    /* The backend can also commit before reporting a timeout. In either case
     * the owner must not publish a confirmed value, read back, or retry. */
    const uint32_t committed_writes = writes();
    const unsigned committed_reads = atomic_load(&s_text_reads);
    expect_clear();
    atomic_store(&s_timeout_after_commit, true);
    CHECK(xiaotai_platform_environment_store("http://committed.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_TIMEOUT && !atomic_load(&s_timeout_after_commit));
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    CHECK(writes() == committed_writes + 1 && atomic_load(&s_text_reads) == committed_reads);
    CHECK(atomic_load(&s_done.calls) == 1);
    expect_unconfirmed("http://good.test");
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_OK && strcmp(value, "http://committed.test") == 0);

    /* The read-back times out: the write passed, the read is held. The owner
     * is held while the write completes so the read is queued only after the
     * gate closed again. */
    xiaotai_signal_t hold;
    CHECK(xiaotai_signal_init(&hold) == XIAOTAI_OK);
    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://read.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_run(block_owner, &hold) == XIAOTAI_OK);
    uint32_t before = writes();
    xiaotai_kv_host_gate(true);
    wait_writes(before + 1);
    xiaotai_delay_ms(20); /* the write's completion is queued behind the block */
    xiaotai_kv_host_gate(false);
    start = xiaotai_time_us();
    xiaotai_signal_give(&hold);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_TIMEOUT);
    CHECK(s_done.at_us - start >= (int64_t)STEP_MS * 1000);
    CHECK(s_done.snapshot.unconfirmed && strcmp(s_done.snapshot.saved, "http://good.test") == 0);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK && drain());
    stored(value, sizeof(value), &err);
    CHECK(err == XIAOTAI_OK && strcmp(value, "http://read.test") == 0);

    /* The read-back fails with a storage error. */
    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://readerr.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_run(block_owner, &hold) == XIAOTAI_OK);
    before = writes();
    xiaotai_kv_host_gate(true);
    wait_writes(before + 1);
    xiaotai_delay_ms(20);
    xiaotai_kv_host_fail(XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&hold);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_INVALID_STATE && s_done.snapshot.unconfirmed);
    xiaotai_kv_host_fail(XIAOTAI_OK);

    /* The read-back differs (another write landed in between). */
    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store("http://mine.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_kv_set_str_async(NS, KEY, "http://other.test") == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_FAIL && s_done.snapshot.unconfirmed);
    CHECK(strcmp(s_done.snapshot.saved, "http://good.test") == 0);

    /* A reset whose read-back still finds a value. */
    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store(NULL, on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_kv_set_str_async(NS, KEY, "http://back.test") == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_FAIL && s_done.snapshot.unconfirmed);

    /* Recovery: a confirmed reset. */
    expect_clear();
    CHECK(xiaotai_platform_environment_store(NULL, on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK);
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(!env.unconfirmed && env.saved[0] == '\0' && strcmp(env.next, BUILTIN_HTTP) == 0);
    xiaotai_signal_deinit(&hold);
    shutdown();
}

static void noop(void *ctx) { (void)ctx; }

static void store_record_shape(void)
{
    static const char old_base[] = "http://old.test";
    static const char new_base[] = "http://new.test";
    static const char malformed[] = "http://blob.test";
    xiaotai_kv_host_clear();
    (void)xiaotai_kv_init();
    put(old_base);
    for (unsigned shape = 1; shape <= 5; ++shape) {
        const uint32_t before = writes();
        atomic_store(&s_corrupt_read, shape);
        CHECK(boot(BUILTIN));
        xiaotai_platform_environment_t env;
        snapshot(&env);
        CHECK(s_boot_result == XIAOTAI_FAIL && env.boot_error == XIAOTAI_FAIL);
        CHECK(!env.active[0] && !env.saved[0] && !env.next[0] && writes() == before);
        CHECK(atomic_load(&s_corrupt_read) == 0);
        const unsigned before_reads = atomic_load(&s_text_reads);
        CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK);
        CHECK(writes() == before && atomic_load(&s_text_reads) == before_reads);
        shutdown();
    }
    CHECK(boot(BUILTIN) && s_boot_result == XIAOTAI_OK);
    expect_clear();
    atomic_store(&s_corrupt_read, 3); /* valid prefix followed by hidden bytes */
    CHECK(xiaotai_platform_environment_store(new_base, on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000));
    CHECK(s_done.result == XIAOTAI_FAIL && s_done.snapshot.unconfirmed);
    CHECK(strcmp(s_done.snapshot.saved, old_base) == 0 && atomic_load(&s_corrupt_read) == 0);

    xiaotai_kv_host_gate(false);
    expect_clear();
    CHECK(xiaotai_platform_environment_store(NULL, on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_kv_set_async(NS, KEY, malformed, sizeof(malformed)) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_ERR_INVALID_ARG && s_done.snapshot.unconfirmed);
    CHECK(strcmp(s_done.snapshot.saved, old_base) == 0);
    char blob[sizeof(malformed)];
    size_t size = sizeof(blob);
    CHECK(xiaotai_kv_get(NS, KEY, blob, &size, 2000) == XIAOTAI_OK);
    CHECK(size == sizeof(malformed) && !memcmp(blob, malformed, size));

    expect_clear();
    CHECK(xiaotai_platform_environment_store(NULL, on_done, &s_done) == XIAOTAI_OK);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK && !s_done.snapshot.unconfirmed);
    CHECK(strcmp(s_done.snapshot.next, BUILTIN_HTTP) == 0);
    shutdown();
}

static void full_owner_queue(void)
{
    /* The write's completion meets a full owner queue: the step deadline
     * carries the store on, and it still succeeds. */
    xiaotai_kv_host_clear();
    CHECK(boot(BUILTIN));
    xiaotai_signal_t hold;
    CHECK(xiaotai_signal_init(&hold) == XIAOTAI_OK);
    xiaotai_kv_host_gate(false);
    expect_clear();
    int64_t start = xiaotai_time_us();
    CHECK(xiaotai_platform_environment_store("http://queued.test", on_done, &s_done) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(3000));
    CHECK(xiaotai_run(block_owner, &hold) == XIAOTAI_OK);
    xiaotai_delay_ms(20); /* the owner takes the block off its queue */
    unsigned queued = 0;
    while (queued < 1000 && xiaotai_run(noop, NULL) == XIAOTAI_OK) {
        ++queued;
    }
    CHECK(queued > 0 && queued < 1000);
    uint32_t before = writes();
    xiaotai_kv_host_gate(true);
    wait_writes(before + 1);
    xiaotai_delay_ms(20);
    xiaotai_signal_give(&hold);
    CHECK(wait_done(3000) && s_done.result == XIAOTAI_OK);
    /* Delivered by the deadline, not before it. */
    CHECK(s_done.at_us - start >= (int64_t)STEP_MS * 1000);
    xiaotai_platform_environment_t env;
    snapshot(&env);
    CHECK(!env.unconfirmed && strcmp(env.saved, "http://queued.test") == 0);
    xiaotai_signal_deinit(&hold);
    shutdown();
}

int main(void)
{
    CHECK(xiaotai_signal_init(&s_sync) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_done_signal) == XIAOTAI_OK);
    normalize();
    default_boot();
    stored_boot();
    boot_errors();
    store_and_reset();
    refused();
    failures();
    store_record_shape();
    full_owner_queue();
    platform_environment_reset();
    return platform_test_result("platform_environment");
}
