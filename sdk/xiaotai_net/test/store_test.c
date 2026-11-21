// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_wifi_credentials.py
// Replaces legacy: tests/xmini/test_wifi_credentials.c::wifi_credentials
// Replaces legacy storage cases: tests/regression/test_wifi_credentials.py,
// tests/xmini/test_platform_network.c, tests/xmini/test_wifi_credentials.c.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store.h"
#include "kv_backend.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"

#define SPACE "wifi_test"
#define WAIT 2000U

static const xiaotai_network_t wifi = {.desc.kind = XIAOTAI_NETWORK_WIFI};
static const xiaotai_module_t engine = {.name = "net", .init = xiaotai_engine_net_init,
    .start = xiaotai_engine_net_start};
static const xiaotai_module_t *const modules[] = {&engine};
static const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 1};

enum { STORE_FAULT_NONE, STORE_FAULT_PAIR_WRITTEN, STORE_FAULT_MRU_WRITE, STORE_FAULT_COMMIT };
static atomic_int store_fault;

xiaotai_err_t __real_xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text,
                                           const void *data, size_t size);
xiaotai_err_t __wrap_xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text,
                                           const void *data, size_t size);
xiaotai_err_t __wrap_xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text,
                                           const void *data, size_t size)
{
    if (atomic_load(&store_fault) == STORE_FAULT_MRU_WRITE && !strcmp(key, "known")) {
        atomic_store(&store_fault, STORE_FAULT_NONE);
        return XIAOTAI_ERR_TIMEOUT;
    }
    const xiaotai_err_t result = __real_xiaotai_kv_backend_set(handle, key, text, data, size);
    if (atomic_load(&store_fault) == STORE_FAULT_PAIR_WRITTEN && !strcmp(key, "credentials")) {
        assert(result == XIAOTAI_OK && !text && size == 99);
        atomic_store(&store_fault, STORE_FAULT_NONE);
        return XIAOTAI_FAIL;
    }
    return result;
}

xiaotai_err_t __real_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);
xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);
xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    if (atomic_load(&store_fault) == STORE_FAULT_COMMIT) {
        atomic_store(&store_fault, STORE_FAULT_NONE);
        return XIAOTAI_ERR_TIMEOUT;
    }
    return __real_xiaotai_kv_backend_commit(handle);
}

const xiaotai_network_t *xiaotai_hal_network(size_t i) { return i == 0 ? &wifi : NULL; }

static xiaotai_net_credentials_t fixture(char letter, size_t password_size)
{
    xiaotai_net_credentials_t c = {0};
    c.ssid[0] = letter;
    memset(c.password, letter, password_size);
    return c;
}

static void settle(void)
{
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    assert(xiaotai_kv_flush(WAIT) == XIAOTAI_OK);
}

static void start(xiaotai_net_store_layout_t layout, unsigned capacity, bool short_password)
{
    const xiaotai_net_config_t cfg = {.space = SPACE, .layout = layout,
        .history_capacity = capacity, .serial_short_passwords = short_password};
    assert(xiaotai_net_configure(&cfg) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    settle();
}

static void stop(void)
{
    settle();
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_net_host_reset();
    xiaotai_kv_host_clear();
}

static xiaotai_err_t load(net_call_t *call)
{
    xiaotai_net_credentials_t c;
    const xiaotai_err_t result = net_store_load(&c);
    memcpy(call->data, &c, sizeof(c));
    call->size = sizeof(c);
    return result;
}

static void read_credentials(xiaotai_err_t expected, const xiaotai_net_credentials_t *expected_value)
{
    xiaotai_net_credentials_t c;
    memset(&c, 0xab, sizeof(c));
    size_t size;
    assert(net_call(load, 0, NULL, 0, &c, sizeof(c), &size, WAIT) == expected);
    assert(size == sizeof(c));
    const xiaotai_net_credentials_t empty = {0};
    assert(!memcmp(&c, expected_value ? expected_value : &empty, size));
}

static xiaotai_err_t flag(net_call_t *call)
{
    bool marked;
    const xiaotai_err_t result = net_store_boot_flag(&marked);
    assert(result != XIAOTAI_OK || marked == (call->arg != 0));
    call->size = 0;
    return result;
}

static xiaotai_err_t boot_request(net_call_t *call)
{
    call->size = 0;
    return net_store_request_boot(call->arg != 0);
}

static xiaotai_err_t clear_flag(net_call_t *call)
{
    call->size = 0;
    return net_store_clear_boot();
}

static void command(xiaotai_err_t (*fn)(net_call_t *), unsigned arg, xiaotai_err_t want)
{
    assert(net_call(fn, arg, NULL, 0, NULL, 0, NULL, WAIT) == want);
}

static uint32_t token_for(const char *ssid, unsigned wanted_joins)
{
    xiaotai_net_host_record_t records[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t n = xiaotai_net_host_records(records, XIAOTAI_NET_HOST_RECORDS_MAX);
    unsigned joins = 0;
    uint32_t token = 0;
    for (size_t i = 0; i < n; ++i) {
        if (records[i].call != XIAOTAI_NET_HOST_JOIN) continue;
        ++joins;
        if (records[i].has_credentials && !strcmp(records[i].credentials.ssid, ssid)) token = records[i].token;
    }
    assert(joins == wanted_joins);
    return token;
}

static void flags_and_record(void)
{
    const xiaotai_net_credentials_t a = fixture('a', 8), b = fixture('b', 64);
    assert(!xiaotai_net_has_saved_credentials());
    assert(xiaotai_net_set_credentials(&a) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_kv_set_str(SPACE, "ssid", a.ssid, WAIT) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str(SPACE, "password", a.password, WAIT) == XIAOTAI_OK);
    start(XIAOTAI_NET_STORE_RECORD, 4, false);
    assert(xiaotai_net_has_saved_credentials());
    const uint32_t old = token_for(a.ssid, 1);
    assert(old);
    read_credentials(XIAOTAI_OK, &a);
    command(flag, 0, XIAOTAI_OK);
    command(boot_request, 0, XIAOTAI_OK);
    assert(xiaotai_net_has_saved_credentials());
    command(flag, 1, XIAOTAI_OK);
    read_credentials(XIAOTAI_OK, &a);
    assert(xiaotai_net_set_credentials(&b) == XIAOTAI_OK && token_for(a.ssid, 1) == old);
    command(flag, 0, XIAOTAI_OK);
    uint8_t wire[99] = {1};
    memcpy(wire + 1, &b, sizeof(b));
    uint8_t bytes[99];
    size_t n = sizeof(bytes);
    assert(xiaotai_kv_get(SPACE, "credentials", bytes, &n, WAIT) == XIAOTAI_OK && n == sizeof(wire));
    assert(!memcmp(wire, bytes, n));
    char legacy[33];
    assert(xiaotai_kv_get_str(SPACE, "ssid", legacy, sizeof(legacy), WAIT) == XIAOTAI_OK);
    assert(!strcmp(legacy, a.ssid));

    const xiaotai_net_driver_event_t address = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 1};
    assert(xiaotai_net_host_emit(&address) == XIAOTAI_OK);
    settle();
    uint8_t known[492];
    n = sizeof(known);
    assert(xiaotai_kv_get(SPACE, "known", known, &n, WAIT) == XIAOTAI_OK && n == 394);
    assert(known[1] == 1 && !memcmp(known + 2, &a, sizeof(a)));

    wire[0] = 2;
    assert(xiaotai_kv_set(SPACE, "credentials", wire, sizeof(wire), WAIT) == XIAOTAI_OK);
    read_credentials(XIAOTAI_FAIL, NULL);
    wire[0] = 1;
    assert(xiaotai_kv_set(SPACE, "credentials", wire, sizeof(wire) - 1, WAIT) == XIAOTAI_OK);
    read_credentials(XIAOTAI_ERR_INVALID_SIZE, NULL);
    memset(wire + 1, 'x', 33);
    assert(xiaotai_kv_set(SPACE, "credentials", wire, sizeof(wire), WAIT) == XIAOTAI_OK);
    read_credentials(XIAOTAI_FAIL, NULL);
    assert(xiaotai_kv_set_u8(SPACE, "credentials", 1, WAIT) == XIAOTAI_OK);
    read_credentials(XIAOTAI_ERR_INVALID_ARG, NULL); /* A present wrong type never revives legacy. */
    assert(xiaotai_kv_erase(SPACE, "credentials", WAIT) == XIAOTAI_OK);
    read_credentials(XIAOTAI_OK, &a);

    uint32_t ticket;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    assert(xiaotai_net_set_credentials(&b) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_forget_credentials() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(ticket);
    xiaotai_kv_host_fail_key("provision", XIAOTAI_FAIL);
    assert(xiaotai_net_set_credentials(&b) == XIAOTAI_FAIL);
    read_credentials(XIAOTAI_OK, &b); /* KV batches do not promise rollback after failure. */
    assert(!xiaotai_net_has_saved_credentials()); /* A partial commit is not confirmation. */
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    assert(xiaotai_net_set_credentials(&b) == XIAOTAI_OK && xiaotai_net_has_saved_credentials());
    command(boot_request, 1, XIAOTAI_OK);
    command(flag, 1, XIAOTAI_OK);
    read_credentials(XIAOTAI_ERR_NOT_FOUND, NULL);
    assert(!xiaotai_net_has_saved_credentials());
    command(clear_flag, 0, XIAOTAI_OK);
    command(flag, 0, XIAOTAI_OK);
    assert(xiaotai_net_forget_credentials() == XIAOTAI_OK);
    n = sizeof(known);
    assert(xiaotai_kv_get(SPACE, "known", known, &n, WAIT) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_net_generation()); /* Clear only storage; this does not disconnect. */
    stop();
}

static void record_failures(void)
{
    static const xiaotai_net_credentials_t old_pair = {.ssid = "old-ap", .password = "old-password"};
    static const xiaotai_net_credentials_t next = {.ssid = "new-ap", .password = "new-password"};
    static const xiaotai_net_credentials_t written = {.ssid = "next-ap", .password = "next-password"};
    static const xiaotai_net_credentials_t unconfirmed = {.ssid = "last-ap", .password = "last-password"};
    assert(xiaotai_kv_set_str(SPACE, "ssid", old_pair.ssid, WAIT) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str(SPACE, "password", old_pair.password, WAIT) == XIAOTAI_OK);
    start(XIAOTAI_NET_STORE_RECORD, 5, false);
    read_credentials(XIAOTAI_OK, &old_pair);
    xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);
    read_credentials(XIAOTAI_ERR_TIMEOUT, NULL);
    assert(xiaotai_net_set_credentials(&next) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_fail(XIAOTAI_OK);

    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    xiaotai_kv_host_fail_key("credentials", XIAOTAI_FAIL);
    assert(xiaotai_net_set_credentials(&next) == XIAOTAI_FAIL);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_kv_host_counts(&after);
    assert(before.commits == after.commits);
    read_credentials(XIAOTAI_OK, &old_pair);
    assert(xiaotai_net_set_credentials(&next) == XIAOTAI_OK);
    read_credentials(XIAOTAI_OK, &next);

    atomic_store(&store_fault, STORE_FAULT_PAIR_WRITTEN);
    assert(xiaotai_net_set_credentials(&written) == XIAOTAI_FAIL);
    assert(atomic_load(&store_fault) == STORE_FAULT_NONE && !xiaotai_net_has_saved_credentials());
    read_credentials(XIAOTAI_OK, &written);
    atomic_store(&store_fault, STORE_FAULT_COMMIT);
    assert(xiaotai_net_set_credentials(&unconfirmed) == XIAOTAI_ERR_TIMEOUT);
    assert(atomic_load(&store_fault) == STORE_FAULT_NONE && !xiaotai_net_has_saved_credentials());
    read_credentials(XIAOTAI_OK, &unconfirmed);
    assert(xiaotai_net_set_credentials(&unconfirmed) == XIAOTAI_OK);
    char retained[65];
    assert(xiaotai_kv_get_str(SPACE, "password", retained, sizeof(retained), WAIT) == XIAOTAI_OK);
    assert(!strcmp(retained, old_pair.password));
    uint8_t wire[99], wanted[99] = {1};
    memcpy(wanted + 1, &unconfirmed, sizeof(unconfirmed));
    size_t size = sizeof(wire);
    assert(xiaotai_kv_get(SPACE, "credentials", wire, &size, WAIT) == XIAOTAI_OK);
    assert(size == sizeof(wanted) && !memcmp(wire, wanted, size));
    puts("legacy credential storage: open/write/commit faults, atomic pair, old strings kept, exact 99-byte recovery");
    stop();
}

static xiaotai_err_t admission(net_call_t *call)
{
    const xiaotai_net_credentials_t c = fixture('v', 7);
    assert(!net_credentials_valid_for(&c, XIAOTAI_PROVISION_PORTAL));
    assert(net_credentials_valid_for(&c, XIAOTAI_PROVISION_SERIAL) == (call->arg != 0));
    call->size = 0;
    return XIAOTAI_OK;
}

static void plain(void)
{
    const xiaotai_net_credentials_t short_key = fixture('e', 7);
    assert(xiaotai_kv_set(SPACE, "wifi", &short_key, sizeof(short_key), WAIT) == XIAOTAI_OK);
    assert(xiaotai_kv_set_u8(SPACE, "other", 42, WAIT) == XIAOTAI_OK);
    start(XIAOTAI_NET_STORE_PLAIN, 0, false);
    assert(xiaotai_net_has_saved_credentials());
    assert(token_for(short_key.ssid, 1));
    read_credentials(XIAOTAI_OK, &short_key);
    assert(xiaotai_net_set_credentials(&short_key) == XIAOTAI_ERR_INVALID_ARG);
    command(admission, 0, XIAOTAI_OK);
    command(boot_request, 0, XIAOTAI_OK);
    command(flag, 1, XIAOTAI_OK);
    read_credentials(XIAOTAI_OK, &short_key);
    assert(xiaotai_net_forget_credentials() == XIAOTAI_OK);
    assert(!xiaotai_net_has_saved_credentials());
    command(flag, 1, XIAOTAI_OK);
    read_credentials(XIAOTAI_ERR_NOT_FOUND, NULL);
    uint8_t other;
    assert(xiaotai_kv_get_u8(SPACE, "other", &other, WAIT) == XIAOTAI_OK && other == 42);
    stop();
}

static void serial(void)
{
    start(XIAOTAI_NET_STORE_PLAIN, 0, true);
    assert(!xiaotai_net_has_saved_credentials());
    command(admission, 1, XIAOTAI_OK);
    for (size_t i = 1; i <= 7; ++i) {
        const xiaotai_net_credentials_t c = fixture('p', i);
        command(boot_request, 1, XIAOTAI_OK);
        command(flag, 1, XIAOTAI_OK);
        read_credentials(XIAOTAI_ERR_NOT_FOUND, NULL);
        assert(xiaotai_net_set_credentials(&c) == XIAOTAI_OK);
        assert(xiaotai_net_has_saved_credentials());
        xiaotai_net_credentials_t wire;
        size_t size = sizeof(wire);
        assert(xiaotai_kv_get(SPACE, "wifi", &wire, &size, WAIT) == XIAOTAI_OK && size == 98);
        assert(!memcmp(&c, &wire, size));
        command(flag, 0, XIAOTAI_OK);
    }
    assert(!token_for("p", 0));
    stop();
    assert(!xiaotai_net_has_saved_credentials());
}

static void *store_while_flash_busy(void *unused)
{
    (void)unused;
    const xiaotai_net_credentials_t c = fixture('t', 8);
    assert(xiaotai_net_set_credentials(&c) == XIAOTAI_ERR_TIMEOUT);
    return NULL;
}

static void timeout(void)
{
    start(XIAOTAI_NET_STORE_PLAIN, 0, false);
    xiaotai_kv_host_gate(false);
    pthread_t request;
    assert(!pthread_create(&request, NULL, store_while_flash_busy, NULL));
    assert(xiaotai_kv_host_wait_held(WAIT));
    uint32_t ticket;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE);
    assert(!pthread_join(request, NULL));
    /* The caller and the KV wait use equal budgets with different start
     * times. Keep flash held until the worker's own timeout is observed. */
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    settle();
    const xiaotai_net_credentials_t c = fixture('t', 8);
    read_credentials(XIAOTAI_OK, &c);
    assert(!xiaotai_net_has_saved_credentials()); /* The timed-out writer did not confirm. */
    stop();
}

static void lengths(void)
{
    assert(!xiaotai_net_credentials_valid(NULL));
    for (size_t ssid = 0; ssid <= 33; ++ssid) {
        for (size_t password = 0; password <= 65; ++password) {
            xiaotai_net_credentials_t c = {0};
            memset(c.ssid, 'u', ssid);
            memset(c.password, 'v', password);
            assert(xiaotai_net_credentials_valid(&c) ==
                   (ssid >= 1 && ssid <= 32 && (password == 0 || (password >= 8 && password <= 64))));
        }
    }
}

static xiaotai_err_t history_remember(net_call_t *call)
{
    xiaotai_net_credentials_t c;
    memcpy(&c, call->data, sizeof(c));
    net_history_remember(&c);
    call->size = 0;
    return XIAOTAI_OK;
}

static void remember(xiaotai_net_credentials_t c)
{
    assert(net_call(history_remember, 0, &c, sizeof(c), NULL, 0, NULL, WAIT) == XIAOTAI_OK);
    settle();
}

static xiaotai_err_t history_security(net_call_t *call)
{
    (void)call;
    char password[65];
    assert(net_history_saved("m", true) && !net_history_saved("m", false));
    assert(net_history_password("m", false, password) == XIAOTAI_ERR_NOT_FOUND && !password[0]);
    assert(net_history_password("m", true, password) == XIAOTAI_OK && password[0] == 'm');
    xiaotai_net_network_t names[5];
    size_t count = 0;
    assert(net_history_list(names, 5, &count) == XIAOTAI_OK && count);
    assert(!strcmp(names[0].ssid, "m") && names[0].saved && !names[0].available && names[0].secure);
    return XIAOTAI_OK;
}

static void history_record(unsigned capacity)
{
    const size_t wire_size = 2 + capacity * 98;
    uint8_t wire[492] = {1, 0}, actual[492];
    assert(xiaotai_kv_set(SPACE, "known", wire, wire_size, WAIT) == XIAOTAI_OK);
    start(XIAOTAI_NET_STORE_RECORD, capacity == 4 ? 5 : 4, false);
    for (char c = 'a'; c <= 'f'; ++c) remember(fixture(c, 8));
    size_t size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_OK && size == wire_size);
    assert(actual[0] == 1 && actual[1] == capacity);
    for (unsigned i = 0; i < capacity; ++i) assert(actual[2 + i * 98] == 'f' - (char)i);
    remember(fixture('d', 9));
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_OK && size == wire_size);
    assert(actual[2] == 'd' && actual[100] == 'f' && actual[2 + 33 + 8] == 'd');
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    remember(fixture('d', 9));
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before.writes);

    assert(net_gate_coprocessor_begin() == XIAOTAI_OK);
    remember(fixture('n', 8));
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_OK && actual[2] == 'd');
    net_gate_coprocessor_end();
    settle();
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_OK && actual[2] == 'n');

    uint32_t ticket;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    remember(fixture('m', 8));
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_OK && actual[2] == 'n');
    xiaotai_net_maintenance_end(ticket);
    settle();
    command(history_security, 0, XIAOTAI_OK);
    assert(xiaotai_net_forget_credentials() == XIAOTAI_OK);
    remember(fixture('m', 8));
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_ERR_NOT_FOUND);
    stop();
}

static xiaotai_err_t list_history(net_call_t *call)
{
    xiaotai_net_network_t list[5];
    size_t count = 0;
    const xiaotai_err_t result = net_history_list(list, 5, &count);
    assert(result == XIAOTAI_OK || count == 0);
    call->size = 0;
    return result;
}

static void history_write_failures(void)
{
    start(XIAOTAI_NET_STORE_RECORD, 5, false);
    const xiaotai_net_credentials_t home = {.ssid = "home", .password = "fixture-only"};
    remember(home);
    uint8_t first[492], wire[492];
    size_t size = sizeof(first);
    assert(xiaotai_kv_get(SPACE, "known", first, &size, WAIT) == XIAOTAI_OK && size == sizeof(first));
    assert(first[0] == 1 && first[1] == 1 && !memcmp(first + 2, &home, sizeof(home)));
    xiaotai_kv_host_counts_t original, repeated;
    xiaotai_kv_host_counts(&original);
    for (unsigned i = 0; i < 100; ++i) remember(home);
    xiaotai_kv_host_counts(&repeated);
    assert(original.writes == repeated.writes);

    const xiaotai_net_credentials_t retry = {.ssid = "write-fails", .password = "updated-fixture"};
    atomic_store(&store_fault, STORE_FAULT_MRU_WRITE);
    remember(retry);
    assert(atomic_load(&store_fault) == STORE_FAULT_NONE);
    size = sizeof(wire);
    assert(xiaotai_kv_get(SPACE, "known", wire, &size, WAIT) == XIAOTAI_OK && size == sizeof(first));
    assert(!memcmp(wire, first, size));
    xiaotai_kv_host_fail_key("known", XIAOTAI_ERR_TIMEOUT);
    command(list_history, 0, XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    remember(retry);
    size = sizeof(wire);
    assert(xiaotai_kv_get(SPACE, "known", wire, &size, WAIT) == XIAOTAI_OK && size == sizeof(wire));
    assert(wire[0] == 1 && wire[1] == 2 && !memcmp(wire + 2, &retry, sizeof(retry)));
    assert(!memcmp(wire + 2 + sizeof(retry), &home, sizeof(home)));
    command(list_history, 0, XIAOTAI_OK);
    puts("legacy MRU: 100 repeats no writes, failed 492-byte write preserves history, read failure and retry recover");
    stop();
}

static void incompatible_history(unsigned variant)
{
    uint8_t wire[493] = {1, 0}, actual[493];
    size_t size = 394;
    if (variant == 0) wire[0] = 2;
    else if (variant == 1) wire[1] = 5;
    else if (variant == 2) { wire[1] = 1; memset(wire + 2, 'x', 33); }
    else if (variant == 3) size = 393;
    else if (variant == 4) size = 493;
    else if (variant == 5) {
        const xiaotai_net_credentials_t same = fixture('s', 8);
        wire[1] = 2;
        memcpy(wire + 2, &same, sizeof(same));
        memcpy(wire + 100, &same, sizeof(same));
    }
    if (variant == 6) assert(xiaotai_kv_set_u8(SPACE, "known", 9, WAIT) == XIAOTAI_OK);
    else assert(xiaotai_kv_set(SPACE, "known", wire, size, WAIT) == XIAOTAI_OK);
    start(XIAOTAI_NET_STORE_RECORD, 4, false);
    remember(fixture('m', 8));
    xiaotai_net_status_t link;
    xiaotai_net_status(&link);
    assert(link.state == XIAOTAI_NET_UNCONFIGURED);
    const xiaotai_net_scan_status_t scan = *net_scan_edit();
    net_snapshot_cancel();
    assert(scan.history_error);
    if (variant == 6) {
        uint8_t value;
        assert(xiaotai_kv_get_u8(SPACE, "known", &value, WAIT) == XIAOTAI_OK && value == 9);
    } else {
        size_t actual_size = sizeof(actual);
        assert(xiaotai_kv_get(SPACE, "known", actual, &actual_size, WAIT) == XIAOTAI_OK);
        assert(actual_size == size && !memcmp(actual, wire, size));
    }
    assert(xiaotai_kv_erase(SPACE, "known", WAIT) == XIAOTAI_OK);
    remember(fixture('m', 8));
    size = sizeof(actual);
    assert(xiaotai_kv_get(SPACE, "known", actual, &size, WAIT) == XIAOTAI_ERR_NOT_FOUND);
    stop();
}

int main(int argc, char **argv)
{
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    if (argc == 1) {
        lengths();
        flags_and_record();
    } else if (!strcmp(argv[1], "plain")) plain();
    else if (!strcmp(argv[1], "serial")) serial();
    else if (!strcmp(argv[1], "timeout")) timeout();
    else if (!strcmp(argv[1], "history")) history_record(4);
    else if (!strcmp(argv[1], "history_492")) history_record(5);
    else if (!strcmp(argv[1], "legacy_failures")) record_failures();
    else if (!strcmp(argv[1], "legacy_history_failures")) history_write_failures();
    else if (argc == 3 && !strcmp(argv[1], "history_bad")) incompatible_history((unsigned)atoi(argv[2]));
    else assert(false);
    return 0;
}
