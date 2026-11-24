// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_nvs_worker.py
// Replaces legacy: tests/regression/test_binding_grant_durability.py
#include <stdatomic.h>

#include "identity_test.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"

typedef struct {
    xiaotai_signal_t done;
    xiaotai_err_t result;
    xiaotai_identity_state_t state;
    uint32_t generation;
    char id[65];
    bool on_reload, on_kv;
} result_t;

static xiaotai_task_t s_kv, s_reload;
static xiaotai_signal_t s_reload_exited;
static atomic_uint s_kv_count, s_reload_count;
static bool s_fail_create;

void __real_xiaotai_task_exit(void) __attribute__((noreturn));
void __wrap_xiaotai_task_exit(void)
{
    if (xiaotai_task_is_current(&s_reload)) xiaotai_signal_give(&s_reload_exited);
    __real_xiaotai_task_exit();
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    const bool reload = !strcmp(cfg->name, "identity_reload");
    if (reload) {
        CHECK(cfg->priority == 1 && cfg->stack_bytes == 3072 && cfg->stack == XIAOTAI_MEM_BULK);
        if (s_fail_create) return XIAOTAI_ERR_NO_MEM;
        atomic_fetch_add(&s_reload_count, 1);
        const xiaotai_err_t err = __real_xiaotai_task_create(cfg, &s_reload);
        if (err == XIAOTAI_OK) *task = s_reload;
        return err;
    }
    if (!strcmp(cfg->name, "xiaotai")) return __real_xiaotai_task_create(cfg, task);
    CHECK(!strcmp(cfg->name, "xiaotai_kv"));
    atomic_fetch_add(&s_kv_count, 1);
    const xiaotai_err_t err = __real_xiaotai_task_create(cfg, &s_kv);
    if (err == XIAOTAI_OK) *task = s_kv;
    return err;
}

static void completed(xiaotai_err_t err, void *ctx)
{
    result_t *result = ctx;
    result->result = err;
    result->state = xiaotai_identity_state();
    result->generation = xiaotai_identity_generation();
    (void)xiaotai_identity_device_id(result->id, sizeof(result->id));
    result->on_reload = xiaotai_task_is_current(&s_reload);
    result->on_kv = xiaotai_task_is_current(&s_kv);
    xiaotai_signal_give(&result->done);
}

static void wait(result_t *out, xiaotai_err_t err, bool reload)
{
    CHECK(xiaotai_signal_take(&out->done, WAIT_MS));
    CHECK(out->result == err && out->on_reload == reload && out->on_kv != reload);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    if (reload) CHECK(xiaotai_signal_take(&s_reload_exited, WAIT_MS));
}

static xiaotai_identity_stats_t stats(void)
{
    xiaotai_identity_stats_t out;
    xiaotai_identity_stats(&out);
    return out;
}

static const xiaotai_identity_t one = {.device_id = "fixture-one", .device_key = "fixture-key-one", .client_id = "fixture-client"};
static const xiaotai_identity_t two = {.device_id = "fixture-two", .device_key = "fixture-key-two"};
static const xiaotai_identity_config_t record_cfg = {.layout = XIAOTAI_IDENTITY_LAYOUT_RECORD, .space = "fixture"};

static void save(result_t *a, result_t *b)
{
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    test_set_str("tirtc_cfg", "endpoint", "obsolete");
    const uint32_t generation = xiaotai_identity_generation();
    const int64_t before = xiaotai_mem_host_outstanding();
    xiaotai_identity_t copy = one;
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_identity_save(&copy, completed, a) == XIAOTAI_OK);
    xiaotai_identity_wipe(&copy, sizeof(copy));
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(xiaotai_identity_state() == XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_save(&two, completed, b) == XIAOTAI_ERR_NO_MEM);
    CHECK(!xiaotai_signal_take(&b->done, 0));
    xiaotai_kv_host_gate(true);
    wait(a, XIAOTAI_OK, false);
    CHECK(a->state == XIAOTAI_IDENTITY_VALID && a->generation != generation && !strcmp(a->id, one.device_id));
    CHECK(xiaotai_mem_host_outstanding() == before && atomic_load(&s_reload_count) == 0);
    char text[70];
    CHECK(xiaotai_kv_get_str("tirtc_cfg", "client_id", text, sizeof(text), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp(text, one.client_id));
    CHECK(xiaotai_kv_get_str("tirtc_cfg", "endpoint", text, sizeof(text), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_kv_host_fail_key("secret", XIAOTAI_FAIL);
    CHECK(xiaotai_identity_save(&two, completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_FAIL, false);
    CHECK(a->state == XIAOTAI_IDENTITY_VALID && !strcmp(a->id, one.device_id) && stats().save_failures == 1);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    const uint32_t rejected = stats().rejected_jobs;
    xiaotai_kv_host_gate(false);
    const xiaotai_kv_op_t op = {.kind = XIAOTAI_KV_OP_ERASE, .key = "fixture"};
    /* Batches fill the real KV queue without coalescing. */
    for (unsigned i = 0; i < XIAOTAI_KV_SLOTS; ++i) {
        CHECK(xiaotai_kv_commit_async("other", &op, 1, NULL, NULL) == XIAOTAI_OK);
    }
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(xiaotai_identity_save(&two, completed, b) == XIAOTAI_ERR_NO_MEM);
    CHECK(stats().rejected_jobs == rejected + 1 && !xiaotai_signal_take(&b->done, 0));
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
}

static void reset_record(result_t *a)
{
    CHECK(test_load(&record_cfg) == XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    uint8_t written[322], expected[322] = {0};
    size_t size = sizeof(written);
    memcpy(expected, one.device_id, strlen(one.device_id));
    memcpy(expected + 65, one.device_key, strlen(one.device_key));
    CHECK(xiaotai_kv_get("fixture", "identity", written, &size, WAIT_MS) == XIAOTAI_OK);
    CHECK(size == 322 && !memcmp(written, expected, 322));
    test_set_str("fixture", "wifi", "keep");
    const uint32_t gen = a->generation;
    CHECK(xiaotai_identity_erase(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    CHECK(a->state == XIAOTAI_IDENTITY_ABSENT && a->generation != gen && stats().erases == 1);
    CHECK(xiaotai_kv_get_str("fixture", "wifi", (char *)written, sizeof(written), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp((char *)written, "keep") && atomic_load(&s_reload_count) == 0);
    CHECK(xiaotai_kv_set("fixture", "identity", expected, 12, WAIT_MS) == XIAOTAI_OK);
    CHECK(test_load(&record_cfg) == XIAOTAI_IDENTITY_UNREADABLE);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_signal_take(&a->done, 0));
    CHECK(xiaotai_identity_erase(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    size = sizeof(written);
    CHECK(xiaotai_kv_get("fixture", "identity", written, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);

    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    test_set_str("tirtc_cfg", "endpoint", "legacy");
    test_set_str("tirtc_cfg", "unrelated", "keep");
    CHECK(xiaotai_identity_erase(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    static const char *const erased[] = {"device_id", "secret", "client_id", "endpoint"};
    for (size_t i = 0; i < sizeof(erased) / sizeof(erased[0]); ++i) {
        CHECK(xiaotai_kv_get_str("tirtc_cfg", erased[i], (char *)written, sizeof(written), WAIT_MS)
              == XIAOTAI_ERR_NOT_FOUND);
    }
    CHECK(xiaotai_kv_get_str("tirtc_cfg", "unrelated", (char *)written, sizeof(written), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp((char *)written, "keep") && atomic_load(&s_reload_count) == 0);
}

static void reload(result_t *a, result_t *b)
{
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    s_fail_create = true;
    CHECK(xiaotai_identity_reload(completed, b) == XIAOTAI_ERR_NO_MEM);
    CHECK(!xiaotai_signal_take(&b->done, 0) && stats().rejected_jobs == 1);
    s_fail_create = false;
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(xiaotai_identity_reload(completed, b) == XIAOTAI_OK);
    CHECK(xiaotai_identity_reload(NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_save(&two, NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_erase(NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(atomic_load(&s_reload_count) == 1 && stats().rejected_jobs == 4);
    xiaotai_kv_host_gate(true);
    wait(a, XIAOTAI_OK, false);
    wait(b, XIAOTAI_OK, true);
    CHECK(b->state == XIAOTAI_IDENTITY_VALID && !strcmp(b->id, one.device_id) && b->generation == a->generation);
    xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_identity_reload(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_ERR_TIMEOUT, true);
    CHECK(a->state == XIAOTAI_IDENTITY_VALID && a->generation == b->generation && stats().load_failures == 1);
    xiaotai_kv_host_fail(XIAOTAI_OK);
    CHECK(xiaotai_kv_erase("tirtc_cfg", "secret", WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_identity_reload(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, true);
    CHECK(a->state == XIAOTAI_IDENTITY_INCOMPLETE && a->generation != b->generation);
    CHECK(xiaotai_kv_erase("tirtc_cfg", "device_id", WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_identity_reload(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, true);
    CHECK(a->state == XIAOTAI_IDENTITY_ABSENT);
}

static void erase_for_restart(result_t *a)
{
    static const xiaotai_identity_config_t cfg = {.layout = XIAOTAI_IDENTITY_LAYOUT_RECORD, .space = "voip_demo"};
    CHECK(test_load(&cfg) == XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    const uint32_t generation = a->generation;
    uint8_t wifi[98] = {0};
    memcpy(wifi, "fixture-net", sizeof("fixture-net"));
    CHECK(xiaotai_kv_set("voip_demo", "wifi", wifi, sizeof(wifi), WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_identity_erase_for_restart(NULL, NULL) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_kv_host_fail_key("identity", XIAOTAI_FAIL);
    CHECK(xiaotai_identity_erase_for_restart(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_FAIL, false);
    CHECK(a->state == XIAOTAI_IDENTITY_VALID && a->generation == generation && stats().erases == 0);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    uint8_t bytes[322];
    size_t size = sizeof(bytes);
    CHECK(xiaotai_kv_get("voip_demo", "identity", bytes, &size, WAIT_MS) == XIAOTAI_OK);
    CHECK(size == sizeof(bytes));
    xiaotai_identity_publish_restart_erase(); // failed commit has no absence to publish
    CHECK(xiaotai_identity_generation() == generation);

    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_identity_erase_for_restart(completed, a) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(xiaotai_identity_save(&two, NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_reload(NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_erase(NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_generation() == generation && xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID);
    xiaotai_kv_host_gate(true);
    wait(a, XIAOTAI_OK, false);
    CHECK(a->state == XIAOTAI_IDENTITY_VALID && a->generation == generation && !strcmp(a->id, one.device_id));
    size = sizeof(bytes);
    CHECK(xiaotai_kv_get("voip_demo", "identity", bytes, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    size = sizeof(bytes);
    CHECK(xiaotai_kv_get("voip_demo", "wifi", bytes, &size, WAIT_MS) == XIAOTAI_OK);
    CHECK(size == sizeof(wifi) && !memcmp(bytes, wifi, size) && stats().erases == 1);
    CHECK(xiaotai_identity_save(&two, NULL, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_identity_reload(NULL, NULL) == XIAOTAI_ERR_NO_MEM);

    // A restart handoff refusal makes the durable absence visible and frees the fence.
    xiaotai_identity_publish_restart_erase();
    const uint32_t cleared = xiaotai_identity_generation();
    CHECK(cleared != generation && xiaotai_identity_state() == XIAOTAI_IDENTITY_ABSENT);
    xiaotai_identity_publish_restart_erase();
    CHECK(xiaotai_identity_generation() == cleared && stats().erases == 1);
    CHECK(xiaotai_identity_save(&one, completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    CHECK(xiaotai_identity_erase_for_restart(completed, a) == XIAOTAI_OK);
    wait(a, XIAOTAI_OK, false);
    // Simulate a successful reboot from the same host KV image.
    CHECK(test_load(&cfg) == XIAOTAI_IDENTITY_ABSENT);
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || argc == 2);
    CHECK(xiaotai_identity_erase(NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    test_clear();
    CHECK(xiaotai_signal_init(&s_reload_exited) == XIAOTAI_OK);
    result_t a = {0}, b = {0};
    CHECK(xiaotai_signal_init(&a.done) == XIAOTAI_OK && xiaotai_signal_init(&b.done) == XIAOTAI_OK);
    if (argc == 1) save(&a, &b);
    else if (!strcmp(argv[1], "erase")) reset_record(&a);
    else if (!strcmp(argv[1], "reload")) reload(&a, &b);
    else if (!strcmp(argv[1], "erase_for_restart")) erase_for_restart(&a);
    else CHECK(false);
    CHECK(atomic_load(&s_kv_count) == 1);
    test_reset();
    xiaotai_signal_deinit(&a.done);
    xiaotai_signal_deinit(&b.done);
    xiaotai_signal_deinit(&s_reload_exited);
    return 0;
}
