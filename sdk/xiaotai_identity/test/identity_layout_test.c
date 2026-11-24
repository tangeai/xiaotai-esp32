// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_station_identity.py
// Replaces legacy: tests/regression/test_runtime_config.py
/* Both storage layouts as deployed firmware wrote them: the four states and
 * every boundary length of a read at engine init, storage failures, the
 * classifiers' handling of results the host storage cannot produce, and the
 * exact operations of a save and an erase (order, keys, record bytes),
 * applied to storage and read back, including the prefix a power loss leaves.
 * Ported from tests/regression/test_runtime_config.py (entry order, optional
 * client_id, failures, zeroed results) and the identity parts of the C3
 * platform network code. */
#include "identity_test.h"

#define STRINGS "tirtc_cfg"

static const xiaotai_identity_config_t k_record = {
    .layout = XIAOTAI_IDENTITY_LAYOUT_RECORD, .space = "xmini", .fingerprint = XIAOTAI_FINGERPRINT_LOWER_COLON};

static void check_not_loaded(void)
{
    xiaotai_identity_t out;
    memset(&out, 0xA5, sizeof(out));
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_ERR_NOT_FOUND);
    const uint8_t *bytes = (const uint8_t *)&out;
    for (size_t i = 0; i < sizeof(out); ++i) CHECK(bytes[i] == 0U);
    char id[8] = "x";
    CHECK(xiaotai_identity_device_id(id, sizeof(id)) == XIAOTAI_ERR_NOT_FOUND && id[0] == '\0');
}

static void check_loaded(const char *device_id, const char *key, const char *client_id)
{
    CHECK(xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID);
    xiaotai_identity_t out;
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_OK);
    CHECK(strcmp(out.device_id, device_id) == 0);
    CHECK(strcmp(out.device_key, key) == 0);
    CHECK(strcmp(out.client_id, client_id) == 0);
    xiaotai_identity_wipe(&out, sizeof(out));
}

static void store_strings(const char *id, const char *secret, const char *client)
{
    if (id) test_set_str(STRINGS, "device_id", id);
    if (secret) test_set_str(STRINGS, "secret", secret);
    if (client) test_set_str(STRINGS, "client_id", client);
}

static xiaotai_identity_state_t load_strings(const char *id, const char *secret, const char *client)
{
    test_clear();
    store_strings(id, secret, client);
    return test_load(NULL);
}

static void strings_reads(void)
{
    char a[300], b[300], c[300];

    /* Never bound, and a namespace with unrelated entries only. */
    CHECK(load_strings(NULL, NULL, NULL) == XIAOTAI_IDENTITY_ABSENT);
    check_not_loaded();
    xiaotai_identity_stats_t stats;
    xiaotai_identity_stats(&stats);
    CHECK(stats.loads == 1U && stats.load_failures == 0U);
    test_clear();
    test_set_str(STRINGS, "other", "x");
    test_set_str("xmini", "device_id", "elsewhere");
    test_set_str("xmini", "secret", "elsewhere");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);

    /* Complete, with and without the optional client id. */
    CHECK(load_strings("test-id", "test-secret", "test-client") == XIAOTAI_IDENTITY_VALID);
    check_loaded("test-id", "test-secret", "test-client");
    char id[8];
    CHECK(xiaotai_identity_device_id(id, 7) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_identity_device_id(id, 8) == XIAOTAI_OK && strcmp(id, "test-id") == 0);
    CHECK(load_strings("test-id", "test-secret", NULL) == XIAOTAI_IDENTITY_VALID);
    check_loaded("test-id", "test-secret", "");
    CHECK(load_strings("test-id", "test-secret", "") == XIAOTAI_IDENTITY_VALID);
    check_loaded("test-id", "test-secret", "");

    /* Boundary lengths: 64 / 256 / 64 characters are the most deployed
     * firmware stores; one more is out of range and reads as unbound. */
    CHECK(load_strings(test_text(a, 64, 'd'), test_text(b, 256, 'k'), test_text(c, 64, 'c')) ==
          XIAOTAI_IDENTITY_VALID);
    check_loaded(a, b, c);
    CHECK(load_strings(test_text(a, 65, 'd'), "k", NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    check_not_loaded();
    CHECK(load_strings("d", test_text(b, 257, 'k'), NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    check_not_loaded();
    CHECK(load_strings("d", "k", test_text(c, 65, 'c')) == XIAOTAI_IDENTITY_INCOMPLETE);
    check_not_loaded();
    CHECK(load_strings("", "k", NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    CHECK(load_strings("d", "", NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    CHECK(load_strings("d", "k", NULL) == XIAOTAI_IDENTITY_VALID);
    check_loaded("d", "k", "");

    /* Half a set, as a power loss in a save leaves it: never a partial identity. */
    CHECK(load_strings("test-id", NULL, "test-client") == XIAOTAI_IDENTITY_INCOMPLETE);
    check_not_loaded();
    CHECK(load_strings(NULL, "test-secret", NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    check_not_loaded();

    /* An entry of another type is not there (typed like NVS). */
    test_clear();
    CHECK(xiaotai_kv_set(STRINGS, "device_id", "test-id", 8, WAIT_MS) == XIAOTAI_OK);
    test_set_str(STRINGS, "secret", "test-secret");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    test_clear();
    CHECK(xiaotai_kv_set(STRINGS, "device_id", "test-id", 8, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set(STRINGS, "secret", "test-secret", 12, WAIT_MS) == XIAOTAI_OK);
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);

    /* Storage failures: unreadable (nothing may overwrite it), counted, not an
     * init failure; with storage_error_unbound the read is unbound instead. */
    const xiaotai_err_t failures[] = {XIAOTAI_ERR_TIMEOUT, XIAOTAI_FAIL, XIAOTAI_ERR_NO_MEM, 0x1105};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        test_clear();
        store_strings("test-id", "test-secret", "test-client");
        xiaotai_kv_host_fail(failures[i]);
        CHECK(test_load(NULL) == XIAOTAI_IDENTITY_UNREADABLE);
        check_not_loaded();
        xiaotai_identity_stats(&stats);
        CHECK(stats.loads == 0U && stats.load_failures == 1U);
        const xiaotai_identity_config_t unbound = {.storage_error_unbound = true};
        CHECK(test_load(&unbound) == XIAOTAI_IDENTITY_INCOMPLETE);
        check_not_loaded();
        xiaotai_kv_host_fail(XIAOTAI_OK);
        /* Nothing was erased or rewritten: once storage works the identity is there. */
        CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
        check_loaded("test-id", "test-secret", "test-client");
    }
    /* The option changes storage failures only: the shapes stay as they are. */
    const xiaotai_identity_config_t unbound = {.storage_error_unbound = true};
    test_clear();
    store_strings("test-id", "test-secret", NULL);
    CHECK(test_load(&unbound) == XIAOTAI_IDENTITY_VALID);
    test_clear();
    CHECK(test_load(&unbound) == XIAOTAI_IDENTITY_ABSENT);
}

static void strings_read_failures(void)
{
    static const char *const failed_keys[] = {NULL, "device_id", "secret", "client_id"};
    test_clear();
    store_strings("test-id", "test-secret", "test-client");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    for (size_t i = 0; i < sizeof(failed_keys) / sizeof(failed_keys[0]); ++i) {
        xiaotai_kv_host_counts_t before, after;
        xiaotai_kv_host_counts(&before);
        if (failed_keys[i]) xiaotai_kv_host_fail_key(failed_keys[i], XIAOTAI_ERR_TIMEOUT);
        else xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);

        xiaotai_identity_t attempt;
        memset(&attempt, 0xA5, sizeof(attempt));
        xiaotai_identity_state_t state = XIAOTAI_IDENTITY_VALID;
        CHECK(xiaotai_identity_layout_read(xiaotai_identity_settings(), &attempt, &state) == XIAOTAI_ERR_TIMEOUT);
        CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);
        for (size_t n = 0; n < sizeof(attempt); ++n) CHECK(((const uint8_t *)&attempt)[n] == 0);
        xiaotai_kv_host_counts(&after);
        CHECK(after.writes == before.writes && after.commits == before.commits);
        CHECK(after.opens - before.opens == after.closes - before.closes + (failed_keys[i] == NULL));

        xiaotai_kv_host_fail(XIAOTAI_OK);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        CHECK(xiaotai_identity_layout_read(xiaotai_identity_settings(), &attempt, &state) == XIAOTAI_OK);
        CHECK(state == XIAOTAI_IDENTITY_VALID && !strcmp(attempt.device_id, "test-id") &&
              !strcmp(attempt.device_key, "test-secret") && !strcmp(attempt.client_id, "test-client"));
        xiaotai_identity_wipe(&attempt, sizeof(attempt));
    }
}

/* Results the host storage cannot produce on its own. */
static void strings_classifier(void)
{
    xiaotai_identity_t identity;
    xiaotai_identity_state_t state = XIAOTAI_IDENTITY_VALID;
    memset(&identity, 0x5A, sizeof(identity));
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_FAIL, XIAOTAI_ERR_NOT_FOUND, XIAOTAI_ERR_NOT_FOUND, &identity,
                                            &state) == XIAOTAI_FAIL);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);
    const uint8_t *bytes = (const uint8_t *)&identity;
    for (size_t i = 0; i < sizeof(identity); ++i) CHECK(bytes[i] == 0U);

    /* The first failure in entry order is reported. */
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_ERR_NOT_FOUND, XIAOTAI_ERR_TIMEOUT, XIAOTAI_ERR_NO_MEM, &identity,
                                            &state) == XIAOTAI_ERR_TIMEOUT);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);

    /* Required entries fine, the client id read failed in storage. */
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_id, "id");
    strcpy(identity.device_key, "key");
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_OK, XIAOTAI_OK, XIAOTAI_FAIL, &identity, &state) == XIAOTAI_FAIL);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE && identity.device_id[0] == '\0');

    /* The client id result does not count while a required entry is out of
     * range (the reader does not read it then). */
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_key, "key");
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_ERR_INVALID_SIZE, XIAOTAI_OK, XIAOTAI_ERR_TIMEOUT, &identity,
                                            &state) == XIAOTAI_OK);
    CHECK(state == XIAOTAI_IDENTITY_INCOMPLETE);

    /* Unterminated client id: out of range. */
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_id, "id");
    strcpy(identity.device_key, "key");
    memset(identity.client_id, 'c', sizeof(identity.client_id));
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_OK, XIAOTAI_OK, XIAOTAI_OK, &identity, &state) == XIAOTAI_OK);
    CHECK(state == XIAOTAI_IDENTITY_INCOMPLETE && identity.client_id[0] == '\0');

    /* VALID leaves nothing behind a terminator. */
    memset(&identity, 'z', sizeof(identity));
    strcpy(identity.device_id, "id");
    strcpy(identity.device_key, "key");
    strcpy(identity.client_id, "cl");
    CHECK(xiaotai_identity_classify_strings(XIAOTAI_OK, XIAOTAI_OK, XIAOTAI_OK, &identity, &state) == XIAOTAI_OK);
    CHECK(state == XIAOTAI_IDENTITY_VALID);
    for (size_t i = 3; i < sizeof(identity.device_id); ++i) CHECK(identity.device_id[i] == '\0');
    for (size_t i = 4; i < sizeof(identity.device_key); ++i) CHECK(identity.device_key[i] == '\0');
    for (size_t i = 3; i < sizeof(identity.client_id); ++i) CHECK(identity.client_id[i] == '\0');

    /* Record: a storage failure is reported, a wrong size is a shape. */
    CHECK(xiaotai_identity_classify_record(XIAOTAI_ERR_TIMEOUT, NULL, 0, &identity, &state) == XIAOTAI_ERR_TIMEOUT);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);
    CHECK(xiaotai_identity_classify_record(XIAOTAI_ERR_INVALID_SIZE, NULL, 400, &identity, &state) == XIAOTAI_OK);
    CHECK(state == XIAOTAI_IDENTITY_UNREADABLE);
    CHECK(xiaotai_identity_classify_record(XIAOTAI_ERR_NOT_FOUND, NULL, 0, &identity, &state) == XIAOTAI_OK);
    CHECK(state == XIAOTAI_IDENTITY_ABSENT);
}

/* A record as deployed firmware writes it: device_id[65] then device_key[257]. */
static void make_record(uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES], const char *id, const char *key)
{
    memset(record, 0, XIAOTAI_IDENTITY_RECORD_BYTES);
    memcpy(record, id, strlen(id));
    memcpy(record + 65, key, strlen(key));
}

static xiaotai_identity_state_t load_record(const void *data, size_t size)
{
    test_clear();
    /* Other owners' entries share the namespace. */
    CHECK(xiaotai_kv_set("xmini", "wifi", "ssid", 5, WAIT_MS) == XIAOTAI_OK);
    if (data) CHECK(xiaotai_kv_set("xmini", "identity", data, size, WAIT_MS) == XIAOTAI_OK);
    return test_load(&k_record);
}

static void record_reads(void)
{
    char a[300], b[300];
    uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES + 1];

    /* The record layout has no default namespace. */
    test_reset();
    const xiaotai_identity_config_t no_space = {.layout = XIAOTAI_IDENTITY_LAYOUT_RECORD};
    CHECK(xiaotai_identity_configure(&no_space) == XIAOTAI_ERR_INVALID_ARG);

    CHECK(load_record(NULL, 0) == XIAOTAI_IDENTITY_ABSENT);
    check_not_loaded();

    make_record(record, "dev-1", "key-1");
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_VALID);
    check_loaded("dev-1", "key-1", "");
    make_record(record, test_text(a, 64, 'd'), test_text(b, 256, 'k'));
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_VALID);
    check_loaded(a, b, "");

    /* Wrong shapes: unreadable, never overwritten. */
    make_record(record, "dev-1", "key-1");
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES - 1) == XIAOTAI_IDENTITY_UNREADABLE);
    check_not_loaded();
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES + 1) == XIAOTAI_IDENTITY_UNREADABLE);
    memset(record, 'd', 65);
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_UNREADABLE);
    make_record(record, "dev-1", "");
    memset(record + 65, 'k', 257);
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_UNREADABLE);
    make_record(record, "", "key-1");
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_UNREADABLE);
    make_record(record, "dev-1", "");
    CHECK(load_record(record, XIAOTAI_IDENTITY_RECORD_BYTES) == XIAOTAI_IDENTITY_UNREADABLE);

    /* A string entry of that name is another type: not there. */
    test_clear();
    test_set_str("xmini", "identity", "dev-1");
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_ABSENT);

    /* Storage failure. */
    make_record(record, "dev-1", "key-1");
    test_clear();
    CHECK(xiaotai_kv_set("xmini", "identity", record, XIAOTAI_IDENTITY_RECORD_BYTES, WAIT_MS) == XIAOTAI_OK);
    xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_UNREADABLE);
    xiaotai_kv_host_fail(XIAOTAI_OK);
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_VALID);

    /* Another product's namespace, same layout. */
    test_clear();
    CHECK(xiaotai_kv_set("voip_demo", "identity", record, XIAOTAI_IDENTITY_RECORD_BYTES, WAIT_MS) == XIAOTAI_OK);
    const xiaotai_identity_config_t other = {.layout = XIAOTAI_IDENTITY_LAYOUT_RECORD, .space = "voip_demo"};
    CHECK(test_load(&other) == XIAOTAI_IDENTITY_VALID);
    check_loaded("dev-1", "key-1", "");
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_ABSENT);
}

static void strings_operations(void)
{
    xiaotai_identity_t identity = {.device_id = "test-id", .device_key = "test-secret", .client_id = "test-client"};
    uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES];
    xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX];

    test_clear();
    CHECK(xiaotai_identity_configure(NULL) == XIAOTAI_OK);
    const xiaotai_identity_settings_t *settings = xiaotai_identity_settings();
    CHECK(strcmp(settings->space, "tirtc_cfg") == 0);

    /* Deployed order: device_id, secret, client_id, then the obsolete entry. */
    CHECK(xiaotai_identity_layout_save_ops(settings, &identity, record, ops) == 4U);
    CHECK(ops[0].kind == XIAOTAI_KV_OP_SET_STR && strcmp(ops[0].key, "device_id") == 0 &&
          strcmp(ops[0].data, "test-id") == 0);
    CHECK(ops[1].kind == XIAOTAI_KV_OP_SET_STR && strcmp(ops[1].key, "secret") == 0 &&
          strcmp(ops[1].data, "test-secret") == 0);
    CHECK(ops[2].kind == XIAOTAI_KV_OP_SET_STR && strcmp(ops[2].key, "client_id") == 0 &&
          strcmp(ops[2].data, "test-client") == 0);
    CHECK(ops[3].kind == XIAOTAI_KV_OP_ERASE && strcmp(ops[3].key, "endpoint") == 0);

    /* Applied: read back complete; the obsolete entry is gone. */
    test_set_str(STRINGS, "endpoint", "https://old.example");
    CHECK(xiaotai_kv_commit(STRINGS, ops, 4, WAIT_MS) == XIAOTAI_OK);
    char text[64];
    CHECK(xiaotai_kv_get_str(STRINGS, "endpoint", text, sizeof(text), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    check_loaded("test-id", "test-secret", "test-client");

    /* An empty client id is still written (deployed firmware always wrote it). */
    strcpy(identity.client_id, "");
    CHECK(xiaotai_identity_layout_save_ops(settings, &identity, record, ops) == 4U);
    CHECK(ops[2].kind == XIAOTAI_KV_OP_SET_STR && strcmp(ops[2].data, "") == 0);
    strcpy(identity.client_id, "test-client");

    /* Every prefix a power loss can leave reads as unbound or as the new identity. */
    for (size_t applied = 1; applied <= 4; ++applied) {
        test_clear();
        CHECK(xiaotai_identity_layout_save_ops(xiaotai_identity_settings(), &identity, record, ops) == 4U);
        CHECK(xiaotai_kv_commit(STRINGS, ops, applied, WAIT_MS) == XIAOTAI_OK);
        const xiaotai_identity_state_t state = test_load(NULL);
        if (applied == 1) {
            CHECK(state == XIAOTAI_IDENTITY_INCOMPLETE);
        } else {
            CHECK(state == XIAOTAI_IDENTITY_VALID);
            check_loaded("test-id", "test-secret", applied == 2 ? "" : "test-client");
        }
    }

    /* Erase: the three entries and the obsolete one, by name; a later owner's
     * entry in the namespace stays. */
    test_clear();
    store_strings("test-id", "test-secret", "test-client");
    test_set_str(STRINGS, "endpoint", "https://old.example");
    test_set_str(STRINGS, "other", "kept");
    CHECK(xiaotai_identity_layout_erase_ops(xiaotai_identity_settings(), ops) == 4U);
    const char *erased[] = {"device_id", "secret", "client_id", "endpoint"};
    for (size_t i = 0; i < 4; ++i) CHECK(ops[i].kind == XIAOTAI_KV_OP_ERASE && strcmp(ops[i].key, erased[i]) == 0);
    CHECK(xiaotai_kv_commit(STRINGS, ops, 4, WAIT_MS) == XIAOTAI_OK);
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(xiaotai_kv_get_str(STRINGS, erased[i], text, sizeof(text), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    }
    CHECK(xiaotai_kv_get_str(STRINGS, "other", text, sizeof(text), WAIT_MS) == XIAOTAI_OK && strcmp(text, "kept") == 0);
}

static void record_operations(void)
{
    xiaotai_identity_t identity = {.device_id = "dev-1", .device_key = "key-1", .client_id = "ignored"};
    uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES];
    uint8_t expected[XIAOTAI_IDENTITY_RECORD_BYTES];
    xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX];

    test_clear();
    CHECK(xiaotai_identity_configure(&k_record) == XIAOTAI_OK);
    memset(record, 0xEE, sizeof(record));
    CHECK(xiaotai_identity_layout_save_ops(xiaotai_identity_settings(), &identity, record, ops) == 1U);
    CHECK(ops[0].kind == XIAOTAI_KV_OP_SET && strcmp(ops[0].key, "identity") == 0 && ops[0].data == record &&
          ops[0].size == 322U);
    make_record(expected, "dev-1", "key-1");
    CHECK(memcmp(record, expected, sizeof(expected)) == 0);

    CHECK(xiaotai_kv_set("xmini", "wifi", "ssid", 5, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_commit("xmini", ops, 1, WAIT_MS) == XIAOTAI_OK);
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_VALID);
    check_loaded("dev-1", "key-1", "");

    /* Erase removes the identity entry only. */
    CHECK(xiaotai_identity_layout_erase_ops(xiaotai_identity_settings(), ops) == 1U);
    CHECK(ops[0].kind == XIAOTAI_KV_OP_ERASE && strcmp(ops[0].key, "identity") == 0);
    CHECK(xiaotai_kv_commit("xmini", ops, 1, WAIT_MS) == XIAOTAI_OK);
    CHECK(test_load(&k_record) == XIAOTAI_IDENTITY_ABSENT);
    char wifi[8];
    size_t size = sizeof(wifi);
    CHECK(xiaotai_kv_get("xmini", "wifi", wifi, &size, WAIT_MS) == XIAOTAI_OK && size == 5U);
}

int main(void)
{
    strings_reads();
    strings_read_failures();
    strings_classifier();
    record_reads();
    strings_operations();
    record_operations();
    test_clear();
    puts("identity_layout: ok");
    return 0;
}
