// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_native_nvs_barrier.py
// Replaces legacy: tests/regression/test_nvs_worker.py
/* KV worker on the host backend: types kept apart (strings, blobs, each
 * integer type), sizes, values above the slot size, integer ranges, coalesced
 * async writes of each type, a timeout that leaves the write to finish, flush,
 * erasing a namespace, batches (xiaotai_kv_commit, _commit_async) and reads
 * with a completion (xiaotai_kv_read_async), and failure reporting. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"
#include "../common/kv_backend.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

#define RUN(test)              \
    do {                       \
        if (test()) return 1;  \
    } while (0)

/* What a commit completion saw; the calls it makes from inside prove where it
 * runs (only the storage worker gets INVALID_STATE from a flush). */
typedef struct {
    int calls;
    xiaotai_err_t result;
    xiaotai_err_t flush_inside, set_inside, async_inside;
    uint32_t delay_ms;
    bool returned;
} commit_seen_t;

static void on_commit(xiaotai_err_t result, void *ctx)
{
    commit_seen_t *seen = ctx;
    const uint8_t byte = 1;
    seen->flush_inside = xiaotai_kv_flush(0);
    seen->set_inside = xiaotai_kv_set("prefs", "nested", &byte, 1, 100);
    seen->async_inside = xiaotai_kv_set_async("prefs", "nested", &byte, 1);
    if (seen->delay_ms) xiaotai_delay_ms(seen->delay_ms);
    seen->result = result;
    ++seen->calls;
    seen->returned = true;
}

typedef struct {
    int calls;
    size_t count;
    xiaotai_kv_value_t values[XIAOTAI_KV_READS_MAX];
    uint8_t bytes[XIAOTAI_KV_READS_MAX][64];
    xiaotai_err_t flush_inside;
    uint32_t delay_ms;
    bool returned;
} read_seen_t;

static void on_read(const xiaotai_kv_value_t *values, size_t count, void *ctx)
{
    read_seen_t *seen = ctx;
    seen->flush_inside = xiaotai_kv_flush(0);
    seen->count = count;
    for (size_t i = 0; i < count && i < XIAOTAI_KV_READS_MAX; ++i) {
        seen->values[i] = values[i];
        if (values[i].data) {
            memcpy(seen->bytes[i], values[i].data, values[i].size < 64 ? values[i].size : 64);
        }
    }
    if (seen->delay_ms) xiaotai_delay_ms(seen->delay_ms);
    ++seen->calls;
    seen->returned = true;
}

/* Holds the worker at the backend gate with one job running. */
static int hold_worker(void)
{
    const uint8_t byte = 0;
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_kv_set_async("prefs", "busy", &byte, 1) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(1000));
    return 0;
}

static int test_basics(void)
{
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);

    /* Strings and blobs are separate types, like NVS. */
    CHECK(xiaotai_kv_set_str("wifi", "ssid", "Ti", 1000) == XIAOTAI_OK);
    char text[16];
    CHECK(xiaotai_kv_get_str("wifi", "ssid", text, sizeof(text), 1000) == XIAOTAI_OK && strcmp(text, "Ti") == 0);
    uint8_t blob[16];
    size_t size = sizeof(blob);
    CHECK(xiaotai_kv_get("wifi", "ssid", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_blob_strict("wifi", "ssid", blob, &size, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get_blob_strict("wifi", "missing", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_blob_strict("unknown", "key", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    const uint32_t value = 0xa5a5a5a5U;
    CHECK(xiaotai_kv_set("id", "epoch", &value, sizeof(value), 1000) == XIAOTAI_OK);
    uint32_t read = 0;
    size = sizeof(read);
    CHECK(xiaotai_kv_get("id", "epoch", &read, &size, 1000) == XIAOTAI_OK && read == value && size == 4);
    CHECK(xiaotai_kv_get_blob_strict("id", "epoch", &read, &size, 1000) == XIAOTAI_OK && read == value && size == 4);
    size = 2;
    CHECK(xiaotai_kv_get("id", "epoch", blob, &size, 1000) == XIAOTAI_ERR_INVALID_SIZE && size == 4);
    size = 2;
    CHECK(xiaotai_kv_get_blob_strict("id", "epoch", blob, &size, 1000) == XIAOTAI_ERR_INVALID_SIZE && size == 4);
    /* A blob read as a string is absent; the output is an empty string. */
    strcpy(text, "stale");
    CHECK(xiaotai_kv_get_str("id", "epoch", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND && text[0] == '\0');
    CHECK(xiaotai_kv_get_str_strict("id", "epoch", text, sizeof(text), 1000) == XIAOTAI_ERR_INVALID_ARG && !text[0]);
    CHECK(xiaotai_kv_get_str_strict("id", "missing", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND && !text[0]);
    CHECK(xiaotai_kv_get_str_strict("unknown", "key", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND && !text[0]);
    CHECK(xiaotai_kv_get_str_strict("wifi", "ssid", text, sizeof(text), 1000) == XIAOTAI_OK && !strcmp(text, "Ti"));
    CHECK(xiaotai_kv_get_str_strict("wifi", "ssid", text, 2, 1000) == XIAOTAI_ERR_INVALID_SIZE && !text[0]);

    /* Inject the exact stored byte span; public string writers cannot create it. */
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_handle_t raw;
    static const char embedded_nul[] = "prefix\0trailing";
    CHECK(xiaotai_kv_backend_open("id", true, &raw) == XIAOTAI_OK);
    CHECK(xiaotai_kv_backend_set(raw, "embedded", true, embedded_nul, sizeof(embedded_nul)) == XIAOTAI_OK);
    CHECK(xiaotai_kv_backend_commit(raw) == XIAOTAI_OK);
    xiaotai_kv_backend_close(raw);
    CHECK(xiaotai_kv_get_str("id", "embedded", text, sizeof(text), 1000) == XIAOTAI_OK);
    CHECK(!memcmp(text, embedded_nul, sizeof(embedded_nul)));
    CHECK(xiaotai_kv_get_str_strict("id", "embedded", text, sizeof(text), 1000) == XIAOTAI_FAIL && !text[0]);

    /* Names and sizes are checked before anything is queued. */
    CHECK(xiaotai_kv_set("a_namespace_too_long", "k", &value, 4, 1000) == XIAOTAI_ERR_INVALID_ARG);
    static uint8_t big[XIAOTAI_KV_VALUE_MAX + 1];
    CHECK(xiaotai_kv_set("id", "big", big, sizeof(big), 1000) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_set("id", "big", NULL, 4, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get("id", "epoch", &read, &size, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get("id", "epoch", &read, &size, XIAOTAI_KV_WAIT_MAX_MS + 1) == XIAOTAI_ERR_INVALID_ARG);

    /* Values above the slot size use a buffer of their own: the longest
     * string, and a blob read into too small a buffer (inline or large job)
     * reports the stored length. */
    static char long_text[XIAOTAI_KV_VALUE_MAX];
    static char long_read[XIAOTAI_KV_VALUE_MAX];
    memset(long_text, 'r', sizeof(long_text) - 1);
    CHECK(xiaotai_kv_set_str("xt_recog", "pending", long_text, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_str("xt_recog", "pending", long_read, sizeof(long_read), 1000) == XIAOTAI_OK);
    CHECK(strcmp(long_read, long_text) == 0);
    CHECK(xiaotai_kv_get_str("xt_recog", "pending", long_read, 600, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    /* A string one byte longer than any value is refused. */
    static char too_long[XIAOTAI_KV_VALUE_MAX + 1];
    memset(too_long, 'x', sizeof(too_long) - 1);
    CHECK(xiaotai_kv_set_str("xt_recog", "pending", too_long, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_set_str_async("xt_recog", "pending", too_long) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_set("id", "big", big, 1000, 1000) == XIAOTAI_OK);
    size = 600;
    CHECK(xiaotai_kv_get("id", "big", long_read, &size, 1000) == XIAOTAI_ERR_INVALID_SIZE && size == 1000);
    size = 100;
    CHECK(xiaotai_kv_get("id", "big", long_read, &size, 1000) == XIAOTAI_ERR_INVALID_SIZE && size == 1000);
    size = sizeof(long_read);
    CHECK(xiaotai_kv_get("id", "big", long_read, &size, 1000) == XIAOTAI_OK && size == 1000);

    /* Erase is idempotent. */
    CHECK(xiaotai_kv_erase("id", "epoch", 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_erase("id", "epoch", 1000) == XIAOTAI_OK);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("id", "epoch", &read, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    /* A namespace nobody wrote does not exist: its keys are absent. */
    CHECK(xiaotai_kv_get("nobody", "epoch", &read, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);

    /* Async writes to one key coalesce while queued: only the latest is stored. */
    xiaotai_kv_host_delay(50);
    xiaotai_kv_stats_t before, after;
    xiaotai_kv_stats(&before);
    uint32_t first = 1, second = 2, third = 3;
    CHECK(xiaotai_kv_set_async("prefs", "volume", &first, 4) == XIAOTAI_OK);   /* starts running */
    xiaotai_delay_ms(10);
    CHECK(xiaotai_kv_set_async("prefs", "volume", &second, 4) == XIAOTAI_OK);  /* queued */
    CHECK(xiaotai_kv_set_async("prefs", "volume", &third, 4) == XIAOTAI_OK);   /* replaces it */
    xiaotai_delay_ms(200);
    xiaotai_kv_stats(&after);
    CHECK(after.coalesced == before.coalesced + 1);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("prefs", "volume", &read, &size, 1000) == XIAOTAI_OK && read == 3);

    /* A queued small write replaced by a large one takes the larger buffer. */
    xiaotai_kv_host_delay(50);
    CHECK(xiaotai_kv_set_async("prefs", "volume", &first, 4) == XIAOTAI_OK);   /* starts running */
    xiaotai_delay_ms(10);
    CHECK(xiaotai_kv_set_async("prefs", "blob", &second, 4) == XIAOTAI_OK);    /* queued */
    memset(big, 7, 900);
    CHECK(xiaotai_kv_set_async("prefs", "blob", big, 900) == XIAOTAI_OK);      /* grows it */
    xiaotai_delay_ms(200);
    xiaotai_kv_host_delay(0);
    size = sizeof(long_read);
    CHECK(xiaotai_kv_get("prefs", "blob", long_read, &size, 1000) == XIAOTAI_OK && size == 900);
    CHECK(memcmp(long_read, big, 900) == 0);

    /* A timeout abandons the wait, not the write: it still lands. */
    xiaotai_kv_host_delay(300);
    const uint32_t late = 42;
    CHECK(xiaotai_kv_set("prefs", "late", &late, 4, 50) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_delay_ms(400);
    xiaotai_kv_host_delay(0);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("prefs", "late", &read, &size, 1000) == XIAOTAI_OK && read == 42);

    /* Flush waits for every job queued before it, asynchronous ones included;
     * 0 only checks; with nothing queued it returns at once. */
    CHECK(xiaotai_kv_flush(0) == XIAOTAI_OK);
    xiaotai_kv_host_delay(60);
    const uint32_t a = 10, b = 11;
    CHECK(xiaotai_kv_set_async("prefs", "fa", &a, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_async("prefs", "fb", &b, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(0) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_flush(20) == XIAOTAI_ERR_TIMEOUT);
    const int64_t flush_from = xiaotai_time_us();
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_time_us() - flush_from < 400 * 1000);
    xiaotai_kv_host_delay(0);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("prefs", "fb", &read, &size, 1000) == XIAOTAI_OK && read == 11);
    CHECK(xiaotai_kv_flush(0) == XIAOTAI_OK);

    /* A flush also waits for a write whose caller timed out while it ran, and
     * a write failing does not stop the flush. */
    xiaotai_kv_host_gate(false);
    const uint32_t abandoned = 77;
    CHECK(xiaotai_kv_set("prefs", "gone", &abandoned, 4, 20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_host_wait_held(1000)); /* the write itself is running */
    CHECK(xiaotai_kv_flush(20) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("prefs", "gone", &read, &size, 1000) == XIAOTAI_OK && read == 77);
    xiaotai_kv_host_fail_key("bad", XIAOTAI_FAIL);
    CHECK(xiaotai_kv_set_async("prefs", "bad", &abandoned, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);

    /* String writes coalesce with string writes only: a queued blob write to
     * the same key keeps its own job, and the jobs run in order. */
    xiaotai_kv_host_delay(50);
    xiaotai_kv_stats(&before);
    CHECK(xiaotai_kv_set_async("prefs", "busy", &first, 4) == XIAOTAI_OK);     /* starts running */
    xiaotai_delay_ms(10);
    CHECK(xiaotai_kv_set_str_async("prefs", "name", "one") == XIAOTAI_OK);    /* queued */
    CHECK(xiaotai_kv_set_str_async("prefs", "name", "two") == XIAOTAI_OK);    /* replaces it */
    CHECK(xiaotai_kv_set_async("prefs", "name", &third, 4) == XIAOTAI_OK);    /* blob: own job */
    CHECK(xiaotai_kv_set_str_async("prefs", "name", NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_host_delay(0);
    xiaotai_kv_stats(&after);
    CHECK(after.coalesced == before.coalesced + 1);
    CHECK(after.submitted == before.submitted + 3);
    size = sizeof(read);
    CHECK(xiaotai_kv_get("prefs", "name", &read, &size, 1000) == XIAOTAI_OK && read == 3);

    /* Backend failures reach the caller; async failures are only counted. */
    xiaotai_kv_host_fail(XIAOTAI_FAIL);
    CHECK(xiaotai_kv_set("prefs", "x", &late, 4, 1000) == XIAOTAI_FAIL);
    xiaotai_kv_stats(&before);
    CHECK(xiaotai_kv_set_async("prefs", "y", &late, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    CHECK(after.failures == before.failures + 1);
    xiaotai_kv_host_fail(XIAOTAI_OK);
    return 0;
}

/* Each integer type: its limits round-trip, values outside are refused before
 * anything is queued, and an entry is found only with the type it was
 * written with. */
static int test_integers(void)
{
    static const struct {
        xiaotai_kv_int_t type;
        int64_t min, max;
        bool bounded;
        int64_t below, above;
    } types[] = {
        {XIAOTAI_KV_U8, 0, UINT8_MAX, true, -1, UINT8_MAX + 1},
        {XIAOTAI_KV_I8, INT8_MIN, INT8_MAX, true, INT8_MIN - 1, INT8_MAX + 1},
        {XIAOTAI_KV_U16, 0, UINT16_MAX, true, -1, UINT16_MAX + 1},
        {XIAOTAI_KV_I16, INT16_MIN, INT16_MAX, true, INT16_MIN - 1, INT16_MAX + 1},
        {XIAOTAI_KV_U32, 0, (int64_t)UINT32_MAX, true, -1, (int64_t)UINT32_MAX + 1},
        {XIAOTAI_KV_I32, INT32_MIN, INT32_MAX, true, (int64_t)INT32_MIN - 1, (int64_t)INT32_MAX + 1},
        {XIAOTAI_KV_U64, 0, (int64_t)UINT64_MAX, false, 0, 0}, /* max: all 64 bits set */
        {XIAOTAI_KV_I64, INT64_MIN, INT64_MAX, false, 0, 0},
    };
    const size_t count = sizeof(types) / sizeof(types[0]);
    xiaotai_kv_stats_t before, after;
    for (size_t t = 0; t < count; ++t) {
        char key[16];
        snprintf(key, sizeof(key), "int%u", (unsigned)t);
        int64_t value = 1;
        CHECK(xiaotai_kv_set_int("ints", key, types[t].type, types[t].min, 1000) == XIAOTAI_OK);
        CHECK(xiaotai_kv_get_int("ints", key, types[t].type, &value, 1000) == XIAOTAI_OK && value == types[t].min);
        CHECK(xiaotai_kv_set_int("ints", key, types[t].type, types[t].max, 1000) == XIAOTAI_OK);
        CHECK(xiaotai_kv_get_int("ints", key, types[t].type, &value, 1000) == XIAOTAI_OK && value == types[t].max);
        if (types[t].bounded) {
            xiaotai_kv_stats(&before);
            CHECK(xiaotai_kv_set_int("ints", key, types[t].type, types[t].below, 1000) == XIAOTAI_ERR_INVALID_ARG);
            CHECK(xiaotai_kv_set_int("ints", key, types[t].type, types[t].above, 1000) == XIAOTAI_ERR_INVALID_ARG);
            CHECK(xiaotai_kv_set_int_async("ints", key, types[t].type, types[t].below) == XIAOTAI_ERR_INVALID_ARG);
            CHECK(xiaotai_kv_set_int_async("ints", key, types[t].type, types[t].above) == XIAOTAI_ERR_INVALID_ARG);
            xiaotai_kv_stats(&after);
            CHECK(after.submitted == before.submitted);
            CHECK(xiaotai_kv_get_int("ints", key, types[t].type, &value, 1000) == XIAOTAI_OK && value == types[t].max);
        }
        /* Found only with its own type. */
        for (size_t u = 0; u < count; ++u) {
            if (u == t) continue;
            CHECK(xiaotai_kv_get_int("ints", key, types[u].type, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);
        }
        uint8_t blob[16];
        size_t size = sizeof(blob);
        char text[16];
        CHECK(xiaotai_kv_get("ints", key, blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
        CHECK(xiaotai_kv_get_str("ints", key, text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND);
    }
    int64_t value = 0;
    CHECK(xiaotai_kv_get_int("ints", "int6", XIAOTAI_KV_U64, &value, 1000) == XIAOTAI_OK);
    CHECK((uint64_t)value == UINT64_MAX);
    CHECK(xiaotai_kv_set_int("ints", "int0", (xiaotai_kv_int_t)8, 1, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get_int("ints", "int0", (xiaotai_kv_int_t)8, &value, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get_int("ints", "int0", XIAOTAI_KV_U8, NULL, 1000) == XIAOTAI_ERR_INVALID_ARG);

    /* The deployed recognition scope (u32) is not found as u8, i32 or a blob. */
    CHECK(xiaotai_kv_set_int("xt_recognition", "scope", XIAOTAI_KV_U32, 41, 1000) == XIAOTAI_OK);
    uint8_t byte = 0;
    CHECK(xiaotai_kv_get_u8("xt_recognition", "scope", &byte, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_int("xt_recognition", "scope", XIAOTAI_KV_I32, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);
    uint32_t raw = 0;
    size_t size = sizeof(raw);
    CHECK(xiaotai_kv_get("xt_recognition", "scope", &raw, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_int("xt_recognition", "scope", XIAOTAI_KV_U32, &value, 1000) == XIAOTAI_OK && value == 41);

    /* The u8 shorthands are the U8 type: the provisioning flag. */
    CHECK(xiaotai_kv_set_u8("xmini", "wifi_provision", 1, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_u8("xmini", "wifi_provision", &byte, 1000) == XIAOTAI_OK && byte == 1);
    CHECK(xiaotai_kv_get_int("xmini", "wifi_provision", XIAOTAI_KV_U8, &value, 1000) == XIAOTAI_OK && value == 1);
    CHECK(xiaotai_kv_get_int("xmini", "wifi_provision", XIAOTAI_KV_I8, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_u8("xmini", "absent", &byte, 1000) == XIAOTAI_ERR_NOT_FOUND && byte == 1);
    return 0;
}

static int test_typed_read_stages(void)
{
    CHECK(xiaotai_kv_set_u8("ints", "num", 7, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str("ints", "txt", "stored", 1000) == XIAOTAI_OK);
    xiaotai_kv_read_stage_t stage = XIAOTAI_KV_READ_STAGE_JOB;
    int64_t number = -1;
    char text[16];
    CHECK(xiaotai_kv_get_int_with_stage("ints", "num", XIAOTAI_KV_U8, &number, &stage, 1000) == XIAOTAI_OK);
    CHECK(stage == XIAOTAI_KV_READ_STAGE_ENTRY && number == 7);
    CHECK(xiaotai_kv_get_str_with_stage("ints", "txt", text, sizeof(text), &stage, 1000) == XIAOTAI_OK);
    CHECK(stage == XIAOTAI_KV_READ_STAGE_ENTRY && !strcmp(text, "stored"));

    xiaotai_kv_host_fail_open_space("ints", XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_get_int_with_stage("ints", "num", XIAOTAI_KV_U8, &number, &stage, 1000) ==
          XIAOTAI_ERR_TIMEOUT && stage == XIAOTAI_KV_READ_STAGE_OPEN);
    CHECK(xiaotai_kv_get_str_with_stage("ints", "txt", text, sizeof(text), &stage, 1000) ==
          XIAOTAI_ERR_TIMEOUT && stage == XIAOTAI_KV_READ_STAGE_OPEN && !text[0]);
    CHECK(xiaotai_kv_get_u8("xmini", "wifi_provision", &(uint8_t){0}, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("ints", "num", 7, 1000) == XIAOTAI_OK);
    xiaotai_kv_host_fail_open_space(NULL, XIAOTAI_OK);

    xiaotai_kv_host_fail_key("num", XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_get_int_with_stage("ints", "num", XIAOTAI_KV_U8, &number, &stage, 1000) ==
          XIAOTAI_ERR_TIMEOUT && stage == XIAOTAI_KV_READ_STAGE_ENTRY && number == 7);
    xiaotai_kv_host_fail_key("txt", XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_get_str_with_stage("ints", "txt", text, sizeof(text), &stage, 1000) ==
          XIAOTAI_ERR_TIMEOUT && stage == XIAOTAI_KV_READ_STAGE_ENTRY && !text[0]);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);

    CHECK(xiaotai_kv_get_int_with_stage("absent-space", "num", XIAOTAI_KV_U8, &number, &stage, 1000) ==
          XIAOTAI_ERR_NOT_FOUND && stage == XIAOTAI_KV_READ_STAGE_OPEN);
    CHECK(xiaotai_kv_get_int_with_stage("ints", "absent", XIAOTAI_KV_U8, &number, &stage, 1000) ==
          XIAOTAI_ERR_NOT_FOUND && stage == XIAOTAI_KV_READ_STAGE_ENTRY);

    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_kv_get_int_with_stage("ints", "num", XIAOTAI_KV_U8, &number, &stage, 20) ==
          XIAOTAI_ERR_TIMEOUT && stage == XIAOTAI_KV_READ_STAGE_JOB);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(stage == XIAOTAI_KV_READ_STAGE_JOB && number == 7);
    return 0;
}

/* Integer async writes coalesce with a queued write of the same key and type
 * only. */
static int test_int_async(void)
{
    xiaotai_kv_stats_t before, after;
    xiaotai_kv_host_counts_t counts_before, counts_after;
    int64_t value = 0;

    xiaotai_kv_stats(&before);
    xiaotai_kv_host_counts(&counts_before);
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_set_int_async("prefs", "vol", XIAOTAI_KV_U8, 1) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int_async("prefs", "vol", XIAOTAI_KV_U8, 2) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    xiaotai_kv_host_counts(&counts_after);
    CHECK(after.coalesced == before.coalesced + 1 && after.submitted == before.submitted + 2);
    CHECK(counts_after.writes == counts_before.writes + 2); /* busy, then vol once */
    CHECK(xiaotai_kv_get_int("prefs", "vol", XIAOTAI_KV_U8, &value, 1000) == XIAOTAI_OK && value == 2);

    /* Another integer type, a string, a blob: each its own job, in order. */
    xiaotai_kv_stats(&before);
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_set_int_async("prefs", "mix", XIAOTAI_KV_U8, 5) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int_async("prefs", "mix", XIAOTAI_KV_U16, 6) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str_async("prefs", "mix", "seven") == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    CHECK(after.coalesced == before.coalesced && after.submitted == before.submitted + 4);
    char text[8];
    CHECK(xiaotai_kv_get_str("prefs", "mix", text, sizeof(text), 1000) == XIAOTAI_OK && strcmp(text, "seven") == 0);
    CHECK(xiaotai_kv_get_int("prefs", "mix", XIAOTAI_KV_U16, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);

    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_set_str_async("prefs", "mix", "eight") == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int_async("prefs", "mix", XIAOTAI_KV_I32, -9) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_int("prefs", "mix", XIAOTAI_KV_I32, &value, 1000) == XIAOTAI_OK && value == -9);
    CHECK(xiaotai_kv_get_str("prefs", "mix", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND);
    return 0;
}

/* Erasing a namespace is one operation and touches no other namespace. */
static int test_erase_space(void)
{
    const uint8_t creds[99] = {1, 2, 3};
    CHECK(xiaotai_kv_set("wifi_cfg", "credentials", creds, sizeof(creds), 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str("wifi_cfg", "ssid", "Ti", 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("wifi_cfg", "provision", 1, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str("keep", "ssid", "Other", 1000) == XIAOTAI_OK);

    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    CHECK(xiaotai_kv_erase_space("wifi_cfg", 1000) == XIAOTAI_OK);
    xiaotai_kv_host_counts(&after);
    CHECK(after.erase_alls == before.erase_alls + 1 && after.writes == before.writes + 1);
    CHECK(after.opens == before.opens + 1 && after.commits == before.commits + 1 && after.closes == before.closes + 1);

    uint8_t blob[128];
    size_t size = sizeof(blob);
    char text[16];
    uint8_t byte = 0;
    CHECK(xiaotai_kv_get("wifi_cfg", "credentials", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_str("wifi_cfg", "ssid", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_u8("wifi_cfg", "provision", &byte, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_str("keep", "ssid", text, sizeof(text), 1000) == XIAOTAI_OK && strcmp(text, "Other") == 0);

    CHECK(xiaotai_kv_erase_space("never_used", 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_erase_space("", 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_erase_space("a_namespace_too_long", 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_erase_space("wifi_cfg", 0) == XIAOTAI_ERR_INVALID_ARG);
    return 0;
}

/* Batches: in order, one open and one commit, stop at the first failure, the
 * limits, and a large batch in a buffer of its own that is returned. */
static int test_commit(void)
{
    const uint8_t creds[99] = {9, 8, 7};
    const uint8_t one = 1;
    CHECK(xiaotai_kv_set_str("wifi_cfg", "old", "gone", 1000) == XIAOTAI_OK);
    const xiaotai_kv_op_t save[] = {
        {.kind = XIAOTAI_KV_OP_SET_STR, .key = "ssid", .data = "Ti"},
        {.kind = XIAOTAI_KV_OP_SET, .key = "credentials", .data = creds, .size = sizeof(creds)},
        {.kind = XIAOTAI_KV_OP_SET_U8, .key = "provision", .data = &one},
        {.kind = XIAOTAI_KV_OP_SET_INT, .key = "scope", .int_type = XIAOTAI_KV_U32, .number = 7},
        {.kind = XIAOTAI_KV_OP_ERASE, .key = "old"},
        {.kind = XIAOTAI_KV_OP_ERASE, .key = "never"},
    };
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    CHECK(xiaotai_kv_commit("wifi_cfg", save, 6, 1000) == XIAOTAI_OK);
    xiaotai_kv_host_counts(&after);
    CHECK(after.opens == before.opens + 1 && after.commits == before.commits + 1 && after.closes == before.closes + 1);
    CHECK(after.writes == before.writes + 6);
    char text[16];
    uint8_t blob[128];
    size_t size = sizeof(blob);
    uint8_t byte = 0;
    int64_t value = 0;
    CHECK(xiaotai_kv_get_str("wifi_cfg", "ssid", text, sizeof(text), 1000) == XIAOTAI_OK && strcmp(text, "Ti") == 0);
    CHECK(xiaotai_kv_get("wifi_cfg", "credentials", blob, &size, 1000) == XIAOTAI_OK && size == 99);
    CHECK(memcmp(blob, creds, sizeof(creds)) == 0);
    CHECK(xiaotai_kv_get_u8("wifi_cfg", "provision", &byte, 1000) == XIAOTAI_OK && byte == 1);
    CHECK(xiaotai_kv_get_int("wifi_cfg", "scope", XIAOTAI_KV_U32, &value, 1000) == XIAOTAI_OK && value == 7);
    CHECK(xiaotai_kv_get_str("wifi_cfg", "old", text, sizeof(text), 1000) == XIAOTAI_ERR_NOT_FOUND);

    /* The first failure stops the batch and nothing is committed; what ran
     * before it stays (not a transaction). */
    const uint32_t a = 1, b = 2, c = 3;
    const xiaotai_kv_op_t chain[] = {
        {.kind = XIAOTAI_KV_OP_SET, .key = "a", .data = &a, .size = 4},
        {.kind = XIAOTAI_KV_OP_SET, .key = "b", .data = &b, .size = 4},
        {.kind = XIAOTAI_KV_OP_SET, .key = "c", .data = &c, .size = 4},
    };
    xiaotai_kv_stats_t stats_before, stats_after;
    xiaotai_kv_stats(&stats_before);
    xiaotai_kv_host_counts(&before);
    xiaotai_kv_host_fail_key("b", XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_kv_commit("chain", chain, 3, 1000) == XIAOTAI_ERR_NO_MEM);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_kv_host_counts(&after);
    xiaotai_kv_stats(&stats_after);
    CHECK(after.commits == before.commits && after.writes == before.writes + 2);
    CHECK(stats_after.failures == stats_before.failures + 1);
    uint32_t read = 0;
    size = sizeof(read);
    CHECK(xiaotai_kv_get("chain", "a", &read, &size, 1000) == XIAOTAI_OK && read == 1);
    CHECK(xiaotai_kv_get("chain", "b", &read, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get("chain", "c", &read, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);

    /* Limits, checked before anything is queued. */
    static uint8_t data[XIAOTAI_KV_VALUE_MAX];
    static char unterminated[XIAOTAI_KV_VALUE_MAX + 1];
    memset(unterminated, 'u', sizeof(unterminated) - 1);
    xiaotai_kv_op_t ops[XIAOTAI_KV_OPS_MAX + 1];
    for (size_t i = 0; i < XIAOTAI_KV_OPS_MAX + 1; ++i) {
        ops[i] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = "k"};
    }
    xiaotai_kv_stats(&stats_before);
    CHECK(xiaotai_kv_commit("limits", ops, 0, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_commit("limits", ops, XIAOTAI_KV_OPS_MAX + 1, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_commit("limits", NULL, 1, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_commit("a_namespace_too_long", ops, 1, 1000) == XIAOTAI_ERR_INVALID_ARG);
    ops[0] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET, .key = "x", .data = data, .size = 1024};
    ops[1] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET, .key = "y", .data = data, .size = 1025};
    CHECK(xiaotai_kv_commit("limits", ops, 2, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    ops[1] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET, .key = "y", .data = data, .size = 2041};
    ops[0] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_INT, .key = "x", .int_type = XIAOTAI_KV_I64, .number = -1};
    CHECK(xiaotai_kv_commit("limits", ops, 2, 1000) == XIAOTAI_ERR_INVALID_SIZE); /* 8 + 2041 */
    ops[1].size = 2040;
    ops[2] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_INT, .key = "z", .int_type = XIAOTAI_KV_U8, .number = 256};
    CHECK(xiaotai_kv_commit("limits", ops, 3, 1000) == XIAOTAI_ERR_INVALID_ARG);
    ops[2] = (xiaotai_kv_op_t){.kind = (xiaotai_kv_op_kind_t)9, .key = "z"};
    CHECK(xiaotai_kv_commit("limits", ops, 3, 1000) == XIAOTAI_ERR_INVALID_ARG);
    ops[2] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_STR, .key = "z", .data = unterminated};
    CHECK(xiaotai_kv_commit("limits", ops, 3, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    ops[2] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_U8, .key = "z", .data = NULL};
    CHECK(xiaotai_kv_commit("limits", ops, 3, 1000) == XIAOTAI_ERR_INVALID_ARG);
    ops[2] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = ""};
    CHECK(xiaotai_kv_commit("limits", ops, 3, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_commit_async("limits", ops, 3, NULL, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_commit("limits", ops, 2, 0) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_kv_stats(&stats_after);
    CHECK(stats_after.submitted == stats_before.submitted);

    /* Exactly the limit: the integer counts 8 bytes, 8 + 2040 = 2048. The
     * batch takes one BULK buffer for its job and gives it back. */
    for (size_t i = 0; i < 2040; ++i) data[i] = (uint8_t)i;
    xiaotai_mem_host_clear();
    CHECK(xiaotai_kv_commit("limits", ops, 2, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 1 && xiaotai_mem_host_outstanding() == 0);
    static uint8_t back[XIAOTAI_KV_VALUE_MAX];
    size = sizeof(back);
    CHECK(xiaotai_kv_get("limits", "y", back, &size, 1000) == XIAOTAI_OK && size == 2040);
    CHECK(memcmp(back, data, 2040) == 0);
    CHECK(xiaotai_kv_get_int("limits", "x", XIAOTAI_KV_I64, &value, 1000) == XIAOTAI_OK && value == -1);

    /* A small batch stays in the permanent slot. */
    xiaotai_mem_host_clear();
    CHECK(xiaotai_kv_commit("limits", save, 3, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0);
    return 0;
}

static int test_commit_async(void)
{
    xiaotai_kv_stats_t before, after;
    commit_seen_t seen;
    int64_t value = 0;

    /* Copied before the call returns: the caller's operations, keys and data
     * may change at once. The completion runs once, on the storage worker
     * (where synchronous calls and flush are refused), with the result. */
    memset(&seen, 0, sizeof(seen));
    char key[8] = "first";
    uint8_t payload[4] = {1, 2, 3, 4};
    xiaotai_kv_op_t ops[2] = {
        {.kind = XIAOTAI_KV_OP_SET, .key = key, .data = payload, .size = sizeof(payload)},
        {.kind = XIAOTAI_KV_OP_SET_INT, .key = "count", .int_type = XIAOTAI_KV_U16, .number = 300},
    };
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_commit_async("batch", ops, 2, on_commit, &seen) == XIAOTAI_OK);
    strcpy(key, "other");
    memset(payload, 0xee, sizeof(payload));
    ops[1].number = 5;
    ops[1].int_type = XIAOTAI_KV_U8;
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.calls == 1 && seen.result == XIAOTAI_OK);
    CHECK(seen.flush_inside == XIAOTAI_ERR_INVALID_STATE);
    CHECK(seen.set_inside == XIAOTAI_ERR_INVALID_STATE && seen.async_inside == XIAOTAI_ERR_INVALID_STATE);
    uint8_t blob[8];
    size_t size = sizeof(blob);
    CHECK(xiaotai_kv_get("batch", "first", blob, &size, 1000) == XIAOTAI_OK && size == 4 && blob[3] == 4);
    CHECK(xiaotai_kv_get("batch", "other", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get_int("batch", "count", XIAOTAI_KV_U16, &value, 1000) == XIAOTAI_OK && value == 300);
    CHECK(xiaotai_kv_get("prefs", "nested", blob, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);

    /* The completion's result is the synchronous commit's: a failing batch
     * gives the same error both ways, and each counts once in failures. */
    const uint32_t a = 1, b = 2;
    const xiaotai_kv_op_t failing[] = {
        {.kind = XIAOTAI_KV_OP_SET, .key = "a", .data = &a, .size = 4},
        {.kind = XIAOTAI_KV_OP_SET, .key = "b", .data = &b, .size = 4},
    };
    xiaotai_kv_host_fail_key("b", XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_kv_stats(&before);
    const xiaotai_err_t sync_result = xiaotai_kv_commit("fails", failing, 2, 1000);
    memset(&seen, 0, sizeof(seen));
    CHECK(xiaotai_kv_commit_async("fails", failing, 2, on_commit, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    CHECK(sync_result == XIAOTAI_ERR_INVALID_SIZE && seen.calls == 1 && seen.result == sync_result);
    CHECK(after.failures == before.failures + 2);

    /* No free slot: NO_MEM, counted in full, nothing queued, no completion;
     * a read with a completion likewise. */
    read_seen_t read_seen;
    memset(&seen, 0, sizeof(seen));
    memset(&read_seen, 0, sizeof(read_seen));
    const xiaotai_kv_read_t one_read = {.kind = XIAOTAI_KV_READ_INT, .key = "count", .int_type = XIAOTAI_KV_U16};
    CHECK(hold_worker() == 0);
    for (unsigned i = 0; i < XIAOTAI_KV_SLOTS - 1; ++i) {
        char filler[16];
        snprintf(filler, sizeof(filler), "fill%u", i);
        CHECK(xiaotai_kv_set_int_async("batch", filler, XIAOTAI_KV_U8, i) == XIAOTAI_OK);
    }
    xiaotai_kv_stats(&before);
    CHECK(xiaotai_kv_commit_async("batch", ops, 1, on_commit, &seen) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_kv_read_async("batch", &one_read, 1, on_read, &read_seen) == XIAOTAI_ERR_NO_MEM);
    xiaotai_kv_stats(&after);
    CHECK(after.full == before.full + 2 && after.submitted == before.submitted);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.calls == 0 && read_seen.calls == 0);

    /* Never merged with an asynchronous write of the same key, and run in
     * queue order: the later single write coalesces into the earlier one,
     * which runs before the batch, so the batch's value stays. */
    xiaotai_kv_stats(&before);
    memset(&seen, 0, sizeof(seen));
    const xiaotai_kv_op_t set_two = {.kind = XIAOTAI_KV_OP_SET_INT, .key = "k", .int_type = XIAOTAI_KV_U8, .number = 2};
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_set_int_async("order", "k", XIAOTAI_KV_U8, 1) == XIAOTAI_OK);
    CHECK(xiaotai_kv_commit_async("order", &set_two, 1, on_commit, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int_async("order", "k", XIAOTAI_KV_U8, 3) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    CHECK(after.coalesced == before.coalesced + 1 && after.submitted == before.submitted + 3);
    CHECK(seen.calls == 1 && seen.result == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_int("order", "k", XIAOTAI_KV_U8, &value, 1000) == XIAOTAI_OK && value == 2);

    /* Flush returns only after the completion returned. */
    memset(&seen, 0, sizeof(seen));
    seen.delay_ms = 80;
    CHECK(xiaotai_kv_commit_async("order", &set_two, 1, on_commit, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.returned && seen.calls == 1);

    /* No completion: the job still runs. */
    const xiaotai_kv_op_t set_four = {.kind = XIAOTAI_KV_OP_SET_INT, .key = "k", .int_type = XIAOTAI_KV_U8, .number = 4};
    CHECK(xiaotai_kv_commit_async("order", &set_four, 1, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_int("order", "k", XIAOTAI_KV_U8, &value, 1000) == XIAOTAI_OK && value == 4);
    return 0;
}

static int test_read_async(void)
{
    read_seen_t seen;
    xiaotai_kv_stats_t before, after;
    const uint8_t blob[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    CHECK(xiaotai_kv_set_str("ident", "name", "TiTai", 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int("ident", "scope", XIAOTAI_KV_U32, 4000000000LL, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set("ident", "blob", blob, sizeof(blob), 1000) == XIAOTAI_OK);

    /* Each kind of outcome in one job; one entry's failure does not stop the
     * others. The values are in the job's slot, wiped once done returned;
     * no buffer is taken. */
    memset(&seen, 0, sizeof(seen));
    const xiaotai_kv_read_t mixed[] = {
        {.kind = XIAOTAI_KV_READ_STR, .key = "name", .capacity = 16},
        {.kind = XIAOTAI_KV_READ_BLOB, .key = "missing", .capacity = 8},
        {.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = XIAOTAI_KV_U8},
        {.kind = XIAOTAI_KV_READ_BLOB, .key = "blob", .capacity = 4},
    };
    xiaotai_mem_host_clear();
    CHECK(xiaotai_kv_read_async("ident", mixed, 4, on_read, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(seen.calls == 1 && seen.count == 4 && seen.flush_inside == XIAOTAI_ERR_INVALID_STATE);
    CHECK(seen.values[0].result == XIAOTAI_OK && seen.values[0].size == 6 && seen.values[0].data);
    CHECK(strcmp((const char *)seen.bytes[0], "TiTai") == 0);
    CHECK(seen.values[1].result == XIAOTAI_ERR_NOT_FOUND && !seen.values[1].data);
    CHECK(seen.values[2].result == XIAOTAI_ERR_NOT_FOUND && !seen.values[2].data);
    CHECK(seen.values[3].result == XIAOTAI_ERR_INVALID_SIZE && seen.values[3].size == 10 && !seen.values[3].data);
    const uint8_t *in_slot = seen.values[0].data;
    for (size_t i = 0; i < 6; ++i) CHECK(in_slot[i] == 0);

    /* Integer value and blob bytes. */
    memset(&seen, 0, sizeof(seen));
    const xiaotai_kv_read_t values[] = {
        {.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = XIAOTAI_KV_U32},
        {.kind = XIAOTAI_KV_READ_BLOB, .key = "blob", .capacity = 16},
    };
    CHECK(xiaotai_kv_read_async("ident", values, 2, on_read, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.calls == 1 && seen.values[0].result == XIAOTAI_OK && seen.values[0].number == 4000000000LL);
    CHECK(!seen.values[0].data);
    CHECK(seen.values[1].result == XIAOTAI_OK && seen.values[1].size == 10 && memcmp(seen.bytes[1], blob, 10) == 0);
    in_slot = seen.values[1].data;
    for (size_t i = 0; i < 10; ++i) CHECK(in_slot[i] == 0);

    /* An absent namespace: every entry absent. */
    memset(&seen, 0, sizeof(seen));
    CHECK(xiaotai_kv_read_async("nobody", values, 2, on_read, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.values[0].result == XIAOTAI_ERR_NOT_FOUND && seen.values[1].result == XIAOTAI_ERR_NOT_FOUND);

    /* Strict and ordinary string reads can coexist in a single job. */
    memset(&seen, 0, sizeof(seen));
    const xiaotai_kv_read_t strings[] = {
        {.kind = XIAOTAI_KV_READ_STR_STRICT, .key = "name", .capacity = 16},
        {.kind = XIAOTAI_KV_READ_STR_STRICT, .key = "blob", .capacity = 16},
        {.kind = XIAOTAI_KV_READ_STR_STRICT, .key = "missing", .capacity = 16},
        {.kind = XIAOTAI_KV_READ_STR, .key = "blob", .capacity = 16},
    };
    CHECK(xiaotai_kv_read_async("ident", strings, 4, on_read, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.calls == 1 && seen.count == 4);
    CHECK(seen.values[0].result == XIAOTAI_OK && !strcmp((const char *)seen.bytes[0], "TiTai"));
    CHECK(seen.values[1].result == XIAOTAI_ERR_INVALID_ARG && !seen.values[1].data);
    CHECK(seen.values[2].result == XIAOTAI_ERR_NOT_FOUND && !seen.values[2].data);
    CHECK(seen.values[3].result == XIAOTAI_ERR_NOT_FOUND && !seen.values[3].data);

    /* A write queued before the read is seen by it. */
    memset(&seen, 0, sizeof(seen));
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_set_str_async("ident", "name", "Next") == XIAOTAI_OK);
    CHECK(xiaotai_kv_read_async("ident", mixed, 1, on_read, &seen) == XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.values[0].result == XIAOTAI_OK && strcmp((const char *)seen.bytes[0], "Next") == 0);

    /* The request is copied: changing it, keys included, after the call
     * changes nothing. */
    memset(&seen, 0, sizeof(seen));
    char key[8] = "name";
    xiaotai_kv_read_t request = {.kind = XIAOTAI_KV_READ_STR, .key = key, .capacity = 16};
    CHECK(hold_worker() == 0);
    CHECK(xiaotai_kv_read_async("ident", &request, 1, on_read, &seen) == XIAOTAI_OK);
    strcpy(key, "blob");
    request.kind = XIAOTAI_KV_READ_BLOB;
    request.capacity = 2;
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.values[0].result == XIAOTAI_OK && strcmp((const char *)seen.bytes[0], "Next") == 0);

    /* Limits: the capacities fit one slot (an integer counts 8 bytes);
     * nothing is queued and done is not called otherwise. */
    memset(&seen, 0, sizeof(seen));
    xiaotai_kv_read_t limit[5] = {
        {.kind = XIAOTAI_KV_READ_BLOB, .key = "blob", .capacity = XIAOTAI_KV_SLOT_BYTES - 7},
        {.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = XIAOTAI_KV_U32},
    };
    for (size_t i = 2; i < 5; ++i) {
        limit[i] = (xiaotai_kv_read_t){.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = XIAOTAI_KV_U32};
    }
    xiaotai_kv_stats(&before);
    CHECK(xiaotai_kv_read_async("ident", limit, 2, on_read, &seen) == XIAOTAI_ERR_INVALID_SIZE); /* 505 + 8 */
    CHECK(xiaotai_kv_read_async("ident", limit, 0, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_read_async("ident", limit, XIAOTAI_KV_READS_MAX + 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_read_async("ident", limit, 1, NULL, &seen) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_read_async("ident", NULL, 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_read_async("a_namespace_too_long", limit, 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_kv_read_t empty = {.kind = XIAOTAI_KV_READ_STR, .key = "name", .capacity = 0};
    CHECK(xiaotai_kv_read_async("ident", &empty, 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_kv_read_t unknown = {.kind = (xiaotai_kv_read_kind_t)127, .key = "name", .capacity = 4};
    CHECK(xiaotai_kv_read_async("ident", &unknown, 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_kv_read_t bad_type = {.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = (xiaotai_kv_int_t)8};
    CHECK(xiaotai_kv_read_async("ident", &bad_type, 1, on_read, &seen) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_kv_stats(&after);
    CHECK(after.submitted == before.submitted);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.calls == 0);
    limit[0].capacity = XIAOTAI_KV_SLOT_BYTES - 8;
    xiaotai_mem_host_clear();
    CHECK(xiaotai_kv_read_async("ident", limit, 2, on_read, &seen) == XIAOTAI_OK); /* exactly one slot */
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(seen.calls == 1 && seen.values[0].size == 10 && seen.values[1].number == 4000000000LL);

    /* Flush returns only after the completion returned. */
    memset(&seen, 0, sizeof(seen));
    seen.delay_ms = 80;
    CHECK(xiaotai_kv_read_async("ident", values, 1, on_read, &seen) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(seen.returned && seen.calls == 1);
    return 0;
}

int main(void)
{
    RUN(test_basics);
    RUN(test_integers);
    RUN(test_typed_read_stages);
    RUN(test_int_async);
    RUN(test_erase_space);
    RUN(test_commit);
    RUN(test_commit_async);
    RUN(test_read_async);
    printf("PASS: kv types, sizes, large values, erase, coalescing, timeout keeps the write, flush, "
           "string writes, failures, integers of every type, integer async writes, erase_space, commit, "
           "commit_async, read_async\n");
    return 0;
}
