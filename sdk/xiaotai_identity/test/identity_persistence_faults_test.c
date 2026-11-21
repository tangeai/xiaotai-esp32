// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_runtime_config.py
#include <stdatomic.h>

#include "identity_test.h"
#include "kv_backend.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"

#define SPACE "tirtc_cfg"
#define FAULT XIAOTAI_ERR_TIMEOUT
static const char *const keys[] = {"device_id", "secret", "client_id", "endpoint"};
enum { KEY_COUNT = sizeof(keys) / sizeof(keys[0]), FAIL_COMMIT = KEY_COUNT + 1 };
static const xiaotai_identity_t before = {
    .device_id = "fixture-prior-id", .device_key = "fixture-prior-key", .client_id = "fixture-prior-client",
};
static const xiaotai_identity_t after = {
    .device_id = "fixture-next-id", .device_key = "fixture-next-key", .client_id = "fixture-next-client",
};

static atomic_bool fail_next_commit;
static atomic_uint commit_attempts;

xiaotai_err_t __real_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);
xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    ++commit_attempts;
    if (atomic_exchange(&fail_next_commit, false)) return FAULT;
    return __real_xiaotai_kv_backend_commit(handle);
}

typedef struct {
    xiaotai_signal_t done;
    xiaotai_err_t error;
    xiaotai_identity_state_t state;
    uint32_t generation;
    unsigned called;
    bool previous_identity;
} completion_t;

static void completed(xiaotai_err_t result, void *ctx)
{
    completion_t *completion = ctx;
    CHECK(!xiaotai_on_owner());
    ++completion->called;
    completion->error = result;
    completion->state = xiaotai_identity_state();
    completion->generation = xiaotai_identity_generation();
    completion->previous_identity = xiaotai_identity_matches(before.device_id, before.device_key);
    xiaotai_signal_give(&completion->done);
}

static void submit(bool erase, completion_t *completion)
{
    const xiaotai_err_t accepted = erase ? xiaotai_identity_erase(completed, completion) :
        xiaotai_identity_save(&after, completed, completion);
    CHECK(accepted == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&completion->done, WAIT_MS));
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(completion->called == 1 && !xiaotai_signal_take(&completion->done, 0));
}

static void verify_reloaded(bool erase)
{
    char text[65];
    CHECK(xiaotai_kv_get_str(SPACE, "unrelated", text, sizeof(text), WAIT_MS) == XIAOTAI_OK &&
          !strcmp(text, "keep"));
    CHECK(xiaotai_kv_get_str("other", "device_id", text, sizeof(text), WAIT_MS) == XIAOTAI_OK &&
          !strcmp(text, "other-owner"));
    CHECK(xiaotai_kv_get_str(SPACE, "endpoint", text, sizeof(text), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    if (erase) {
        for (size_t i = 0; i < KEY_COUNT; ++i) {
            CHECK(xiaotai_kv_get_str(SPACE, keys[i], text, sizeof(text), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
        }
        CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    } else {
        CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
        CHECK(xiaotai_identity_matches(after.device_id, after.device_key));
        CHECK(xiaotai_kv_get_str(SPACE, "client_id", text, sizeof(text), WAIT_MS) == XIAOTAI_OK &&
              !strcmp(text, after.client_id));
    }
}

static void check_failure(bool erase, unsigned stage)
{
    test_clear();
    test_set_str(SPACE, keys[0], before.device_id);
    test_set_str(SPACE, keys[1], before.device_key);
    test_set_str(SPACE, keys[2], before.client_id);
    test_set_str(SPACE, keys[3], "obsolete");
    test_set_str(SPACE, "unrelated", "keep");
    test_set_str("other", "device_id", "other-owner");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    xiaotai_kv_host_counts_t initial, failed, retried;
    xiaotai_kv_host_counts(&initial);
    const unsigned attempts = atomic_load(&commit_attempts);
    const uint32_t generation = xiaotai_identity_generation();

    if (stage == 0) xiaotai_kv_host_fail(FAULT);
    else if (stage == FAIL_COMMIT) atomic_store(&fail_next_commit, true);
    else xiaotai_kv_host_fail_key(keys[stage - 1], FAULT);
    completion_t failure = {0};
    CHECK(xiaotai_signal_init(&failure.done) == XIAOTAI_OK);
    submit(erase, &failure);
    CHECK(failure.error == FAULT && failure.generation == generation &&
          failure.state == XIAOTAI_IDENTITY_VALID && failure.previous_identity);
    xiaotai_signal_deinit(&failure.done);
    CHECK(!atomic_load(&fail_next_commit));

    xiaotai_kv_host_counts(&failed);
    CHECK(failed.opens == initial.opens + 1 && failed.closes == initial.closes + (stage != 0));
    CHECK(failed.writes == initial.writes + (stage == FAIL_COMMIT ? KEY_COUNT : stage));
    CHECK(atomic_load(&commit_attempts) == attempts + (stage == FAIL_COMMIT));
    CHECK(failed.commits == initial.commits && failed.erase_alls == initial.erase_alls);

    /* A failed commit does not promise rollback of earlier NVS operations.
     * Verify a healthy retry, not what bytes a failed operation left behind. */
    xiaotai_kv_host_fail(XIAOTAI_OK);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    completion_t success = {0};
    CHECK(xiaotai_signal_init(&success.done) == XIAOTAI_OK);
    submit(erase, &success);
    CHECK(success.error == XIAOTAI_OK && success.generation != generation && !success.previous_identity);
    CHECK(success.state == (erase ? XIAOTAI_IDENTITY_ABSENT : XIAOTAI_IDENTITY_VALID));
    xiaotai_signal_deinit(&success.done);
    xiaotai_kv_host_counts(&retried);
    CHECK(retried.writes == failed.writes + KEY_COUNT && retried.commits == failed.commits + 1);
    CHECK(retried.erase_alls == failed.erase_alls);
    verify_reloaded(erase);
}

static void check_no_backend_io(const xiaotai_kv_host_counts_t *prior)
{
    xiaotai_kv_host_counts_t current;
    xiaotai_kv_host_counts(&current);
    CHECK(current.opens == prior->opens && current.closes == prior->closes);
    CHECK(current.writes == prior->writes && current.commits == prior->commits &&
          current.erase_alls == prior->erase_alls);
}

static void check_admission(void)
{
    test_clear();
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    const uint32_t generation = xiaotai_identity_generation();
    completion_t result = {0};
    CHECK(xiaotai_signal_init(&result.done) == XIAOTAI_OK);

    xiaotai_kv_host_counts_t backend;
    xiaotai_kv_stats_t prior, current;
    xiaotai_kv_host_counts(&backend);
    xiaotai_kv_stats(&prior);
    xiaotai_identity_t invalid = after;
    memset(invalid.client_id, 'c', sizeof(invalid.client_id));
    CHECK(xiaotai_identity_save(&invalid, completed, &result) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(result.called == 0 && !xiaotai_signal_take(&result.done, 0));
    xiaotai_kv_stats(&current);
    CHECK(current.submitted == prior.submitted);
    check_no_backend_io(&backend);

    /* Fill every real, non-coalescing KV slot before testing admission. */
    const xiaotai_kv_op_t filler = {.kind = XIAOTAI_KV_OP_ERASE, .key = "fixture"};
    xiaotai_kv_host_gate(false);
    for (unsigned i = 0; i < XIAOTAI_KV_SLOTS; ++i) {
        CHECK(xiaotai_kv_commit_async("other", &filler, 1, NULL, NULL) == XIAOTAI_OK);
    }
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    xiaotai_kv_host_counts(&backend);
    xiaotai_kv_stats(&prior);
    CHECK(xiaotai_identity_save(&after, completed, &result) == XIAOTAI_ERR_NO_MEM);
    xiaotai_identity_t bytes;
    memset(&bytes, 0xA5, sizeof(bytes));
    xiaotai_identity_state_t state = XIAOTAI_IDENTITY_VALID;
    CHECK(xiaotai_identity_layout_read(xiaotai_identity_settings(), &bytes, &state) == XIAOTAI_ERR_NO_MEM);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);
    for (size_t i = 0; i < sizeof(bytes); ++i) CHECK(((const unsigned char *)&bytes)[i] == 0);
    xiaotai_kv_stats(&current);
    CHECK(current.submitted == prior.submitted && current.full == prior.full + 2);
    check_no_backend_io(&backend);

    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(result.called == 0 && !xiaotai_signal_take(&result.done, 0));
    CHECK(xiaotai_identity_generation() == generation && xiaotai_identity_state() == XIAOTAI_IDENTITY_ABSENT);
    xiaotai_signal_deinit(&result.done);
    puts("identity: invalid save and full KV admission reject without I/O or callbacks; failed read clears output");
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "erase")));
    for (unsigned stage = 0; stage <= FAIL_COMMIT; ++stage) check_failure(argc == 2, stage);
    if (argc == 1) check_admission();
    test_clear();
    puts("identity: open, four key operations and commit propagate errors and permit retry");
    return 0;
}
