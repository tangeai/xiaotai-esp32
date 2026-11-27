// SPDX-License-Identifier: MIT
/* Binding generation: starts at 1 and is never 0, advances on every trigger
 * (explicit unbind, a save of another binding, a reload that replaces or
 * loses the binding, an erase) and only then, wraps past 0, and stays unique
 * under concurrent advances. A server-side unbind advances without touching
 * the loaded identity. */
#include <stdatomic.h>
#include <stdint.h>

#include "identity_test.h"
#include "xiaotai/port/os.h"

static xiaotai_identity_t make(const char *id, const char *key, const char *client)
{
    xiaotai_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_id, id);
    strcpy(identity.device_key, key);
    strcpy(identity.client_id, client);
    return identity;
}

static uint32_t advances(void)
{
    xiaotai_identity_stats_t stats;
    xiaotai_identity_stats(&stats);
    return stats.generation_advances;
}

/* Applies a job result and checks whether the generation moved by one. */
static void expect_apply(xiaotai_identity_apply_t kind, const xiaotai_identity_t *identity,
                         xiaotai_identity_state_t state, bool advance, xiaotai_identity_state_t after)
{
    const uint32_t before = xiaotai_identity_generation();
    const uint32_t counted = advances();
    CHECK(xiaotai_identity_apply(kind, identity, state) == advance);
    CHECK(xiaotai_identity_generation() == (advance ? before + 1U : before));
    CHECK(advances() == counted + (advance ? 1U : 0U));
    CHECK(xiaotai_identity_state() == after);
}

static void expect_loaded(const xiaotai_identity_t *expected)
{
    xiaotai_identity_t out;
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_OK);
    CHECK(memcmp(&out, expected, sizeof(out)) == 0);
}

static bool s_observe_unlock;
static uint32_t s_unlocked_generation;
static xiaotai_identity_t s_unlocked_identity;

void __real_xiaotai_mutex_unlock(xiaotai_mutex_t *mutex);

void __wrap_xiaotai_mutex_unlock(xiaotai_mutex_t *mutex)
{
    __real_xiaotai_mutex_unlock(mutex);
    if (!s_observe_unlock) return;
    /* Another owner may now acquire the identity: observe at the exact
     * release, not after apply() has finished changing the generation. */
    s_observe_unlock = false;
    CHECK(xiaotai_identity_get(&s_unlocked_identity) == XIAOTAI_OK);
    s_unlocked_generation = xiaotai_identity_generation();
}

#define TASKS 4U
#define PER_TASK 20000U
static atomic_uchar s_seen[TASKS * PER_TASK + 2U];
static atomic_uint s_finished;
static atomic_uint s_duplicates;

static void advancer(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < PER_TASK; ++i) {
        const uint32_t value = xiaotai_identity_generation_advance();
        if (value == 0U || value >= TASKS * PER_TASK + 2U || atomic_exchange(&s_seen[value], 1U) != 0U) {
            atomic_fetch_add(&s_duplicates, 1U);
        }
    }
    atomic_fetch_add(&s_finished, 1U);
    xiaotai_task_exit();
}

int main(void)
{
    /* 1 on every boot, before and after the engine init. */
    test_clear();
    CHECK(xiaotai_identity_generation() == 1U);
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_generation() == 1U && advances() == 0U);

    /* Wrap-around skips 0. */
    CHECK(xiaotai_identity_generation_next(1U) == 2U);
    CHECK(xiaotai_identity_generation_next(UINT32_MAX - 1U) == UINT32_MAX);
    CHECK(xiaotai_identity_generation_next(UINT32_MAX) == 1U);
    CHECK(xiaotai_identity_generation_next(0U) == 1U);

    /* Server-side unbind: advance, nothing erased. */
    const xiaotai_identity_t a = make("dev-a", "key-a", "client-a");
    const xiaotai_identity_t a_client = make("dev-a", "key-a", "client-b");
    const xiaotai_identity_t a_key = make("dev-a", "key-b", "client-b");
    const xiaotai_identity_t b = make("dev-b", "key-b", "");
    expect_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &a, XIAOTAI_IDENTITY_VALID, true, XIAOTAI_IDENTITY_VALID);
    expect_loaded(&a);
    CHECK(xiaotai_identity_generation_advance() == 3U);
    CHECK(xiaotai_identity_generation() == 3U && advances() == 2U);
    expect_loaded(&a);

    /* Saves: same device id and key keep the binding (the client id may
     * change); another key or device id is another binding. */
    expect_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &a_client, XIAOTAI_IDENTITY_VALID, false, XIAOTAI_IDENTITY_VALID);
    expect_loaded(&a_client);
    expect_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &a_key, XIAOTAI_IDENTITY_VALID, true, XIAOTAI_IDENTITY_VALID);
    const uint32_t old_generation = xiaotai_identity_generation();
    const uint32_t old_advances = advances();
    s_observe_unlock = true;
    CHECK(xiaotai_identity_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &b, XIAOTAI_IDENTITY_VALID));
    CHECK(!s_observe_unlock && memcmp(&s_unlocked_identity, &b, sizeof(b)) == 0);
    CHECK(s_unlocked_generation == old_generation + 1U && advances() == old_advances + 1U);
    expect_loaded(&b);

    /* An invalid identity changes nothing. */
    const xiaotai_identity_t invalid = make("", "key", "");
    expect_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &invalid, XIAOTAI_IDENTITY_VALID, false, XIAOTAI_IDENTITY_VALID);
    expect_loaded(&b);

    /* Reloads: the same binding, another one, the binding disappearing, and a
     * binding appearing where none was known (no advance: nothing cached
     * belonged to a binding). */
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, &b, XIAOTAI_IDENTITY_VALID, false, XIAOTAI_IDENTITY_VALID);
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, &a, XIAOTAI_IDENTITY_VALID, true, XIAOTAI_IDENTITY_VALID);
    expect_loaded(&a);
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, NULL, XIAOTAI_IDENTITY_INCOMPLETE, true, XIAOTAI_IDENTITY_INCOMPLETE);
    xiaotai_identity_t out;
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_ERR_NOT_FOUND);
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, &a, XIAOTAI_IDENTITY_VALID, false, XIAOTAI_IDENTITY_VALID);
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, NULL, XIAOTAI_IDENTITY_UNREADABLE, true, XIAOTAI_IDENTITY_UNREADABLE);
    expect_apply(XIAOTAI_IDENTITY_APPLY_RELOAD, NULL, XIAOTAI_IDENTITY_ABSENT, false, XIAOTAI_IDENTITY_ABSENT);

    /* Erase always advances, even with nothing loaded. */
    expect_apply(XIAOTAI_IDENTITY_APPLY_SAVE, &a, XIAOTAI_IDENTITY_VALID, true, XIAOTAI_IDENTITY_VALID);
    expect_apply(XIAOTAI_IDENTITY_APPLY_ERASE, NULL, XIAOTAI_IDENTITY_ABSENT, true, XIAOTAI_IDENTITY_ABSENT);
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_ERR_NOT_FOUND);
    expect_apply(XIAOTAI_IDENTITY_APPLY_ERASE, NULL, XIAOTAI_IDENTITY_ABSENT, true, XIAOTAI_IDENTITY_ABSENT);

    /* Concurrent advances: every returned value is distinct and none is lost. */
    test_reset();
    CHECK(xiaotai_identity_generation() == 1U && advances() == 0U);
    for (unsigned i = 0; i < TASKS; ++i) {
        const xiaotai_task_config_t config = {.name = "advance", .entry = advancer, .stack_bytes = 8192U,
                                              .stack = XIAOTAI_MEM_INTERNAL, .priority = 1U,
                                              .core = XIAOTAI_CORE_ANY};
        xiaotai_task_t task;
        CHECK(xiaotai_task_create(&config, &task) == XIAOTAI_OK);
    }
    for (unsigned waited = 0; atomic_load(&s_finished) < TASKS && waited < 20000U; waited += 10U) {
        xiaotai_delay_ms(10);
    }
    CHECK(atomic_load(&s_finished) == TASKS);
    CHECK(atomic_load(&s_duplicates) == 0U);
    CHECK(xiaotai_identity_generation() == 1U + TASKS * PER_TASK);
    CHECK(advances() == TASKS * PER_TASK);
    for (uint32_t value = 2U; value <= TASKS * PER_TASK + 1U; ++value) CHECK(atomic_load(&s_seen[value]) == 1U);

    /* A reset (a reboot for the tests) starts over at 1. */
    xiaotai_identity_reset();
    CHECK(xiaotai_identity_generation() == 1U);
    test_clear();
    puts("identity_generation: ok");
    return 0;
}
