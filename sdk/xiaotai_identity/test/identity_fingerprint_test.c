// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_station_identity.py
/* Fingerprint and client id: the three spellings (upper-case colons,
 * lower-case colons, the compact legacy form kept only for an exact match
 * with the device's MAC), the default client id with the system port's chip
 * name or a configured prefix, the stored client id taking precedence, and
 * capacities. Ported from tests/regression/test_station_identity.py (same
 * MAC, same stored client ids that must not change the fingerprint) and the
 * C3 platform's "c3-<mac>" and lower-case colon form. */
#include "identity_test.h"

static const uint8_t k_mac[XIAOTAI_IDENTITY_MAC_BYTES] = {2, 4, 6, 8, 10, 12};
static const uint8_t k_other_mac[XIAOTAI_IDENTITY_MAC_BYTES] = {0xF0, 0x0D, 0xAB, 0xCD, 0xEF, 0x01};

static void expect_fingerprint(const uint8_t *mac, const char *expected)
{
    char out[32];
    memset(out, 'x', sizeof(out));
    CHECK(xiaotai_identity_fingerprint(mac, out, sizeof(out)) == XIAOTAI_OK);
    CHECK(strcmp(out, expected) == 0);
}

static void expect_client_id(const uint8_t *mac, const char *expected)
{
    char out[80];
    CHECK(xiaotai_identity_client_id(mac, out, sizeof(out)) == XIAOTAI_OK);
    CHECK(strcmp(out, expected) == 0);
}

/* Bound device with this stored client id (strings layout). */
static void load_bound(const xiaotai_identity_config_t *config, const char *client_id)
{
    test_clear();
    test_set_str("tirtc_cfg", "device_id", "test-id");
    test_set_str("tirtc_cfg", "secret", "test-secret");
    if (client_id) test_set_str("tirtc_cfg", "client_id", client_id);
    CHECK(test_load(config) == XIAOTAI_IDENTITY_VALID);
}

int main(void)
{
    /* Before the engine init: configured spelling, chip-name prefix. */
    test_clear();
    expect_fingerprint(k_mac, "02:04:06:08:0A:0C");
    expect_client_id(k_mac, "host-020406080a0c");
    expect_fingerprint(k_other_mac, "F0:0D:AB:CD:EF:01");
    expect_client_id(k_other_mac, "host-f00dabcdef01");

    /* Never bound. */
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);
    expect_fingerprint(k_mac, "02:04:06:08:0A:0C");
    expect_client_id(k_mac, "host-020406080a0c");

    /* Stored client ids that are not the compact MAC leave the fingerprint
     * alone; a non-empty stored client id is the client id. */
    const char *unchanged[] = {"", "custom-device", "FFFFFFFFFFFF", "020406080a0c", "host-020406080a0c",
                               "020406080A0C0", "020406080A0"};
    for (size_t i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i) {
        load_bound(NULL, unchanged[i]);
        expect_fingerprint(k_mac, "02:04:06:08:0A:0C");
        expect_client_id(k_mac, unchanged[i][0] ? unchanged[i] : "host-020406080a0c");
    }
    load_bound(NULL, NULL);
    expect_fingerprint(k_mac, "02:04:06:08:0A:0C");
    expect_client_id(k_mac, "host-020406080a0c");

    /* Legacy binding: the exact upper-case compact MAC is the fingerprint and
     * the client id; for any other MAC it is only a client id. */
    load_bound(NULL, "020406080A0C");
    expect_fingerprint(k_mac, "020406080A0C");
    expect_client_id(k_mac, "020406080A0C");
    expect_fingerprint(k_other_mac, "F0:0D:AB:CD:EF:01");
    char out[32];
    CHECK(xiaotai_identity_fingerprint(k_mac, out, 12) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(xiaotai_identity_fingerprint(k_mac, out, 13) == XIAOTAI_OK && strcmp(out, "020406080A0C") == 0);

    /* The rule holds for every configured spelling. */
    const xiaotai_identity_config_t lower = {.fingerprint = XIAOTAI_FINGERPRINT_LOWER_COLON};
    load_bound(&lower, "020406080A0C");
    expect_fingerprint(k_mac, "020406080A0C");
    load_bound(&lower, "custom-device");
    expect_fingerprint(k_mac, "02:04:06:08:0a:0c");

    /* Only a complete identity counts: half a set keeps the default forms. */
    test_clear();
    test_set_str("tirtc_cfg", "device_id", "test-id");
    test_set_str("tirtc_cfg", "client_id", "020406080A0C");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    expect_fingerprint(k_mac, "02:04:06:08:0A:0C");
    expect_client_id(k_mac, "host-020406080a0c");

    /* Record layout with lower-case colons and a configured prefix. */
    test_clear();
    const xiaotai_identity_config_t record = {.layout = XIAOTAI_IDENTITY_LAYOUT_RECORD,
                                              .space = "xmini",
                                              .fingerprint = XIAOTAI_FINGERPRINT_LOWER_COLON,
                                              .client_prefix = "c3"};
    CHECK(test_load(&record) == XIAOTAI_IDENTITY_ABSENT);
    expect_fingerprint(k_mac, "02:04:06:08:0a:0c");
    expect_client_id(k_mac, "c3-020406080a0c");
    expect_fingerprint(k_other_mac, "f0:0d:ab:cd:ef:01");
    expect_client_id(k_other_mac, "c3-f00dabcdef01");

    /* Capacities: the text and its terminator, or XIAOTAI_ERR_INVALID_SIZE and "". */
    memset(out, 'x', sizeof(out));
    CHECK(xiaotai_identity_fingerprint(k_mac, out, 17) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(xiaotai_identity_fingerprint(k_mac, out, 18) == XIAOTAI_OK && strlen(out) == 17U);
    CHECK(xiaotai_identity_client_id(k_mac, out, 15) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(xiaotai_identity_client_id(k_mac, out, 16) == XIAOTAI_OK && strcmp(out, "c3-020406080a0c") == 0);
    CHECK(xiaotai_identity_fingerprint(NULL, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_identity_fingerprint(k_mac, NULL, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_identity_fingerprint(k_mac, out, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_identity_client_id(NULL, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_identity_client_id(k_mac, out, 0) == XIAOTAI_ERR_INVALID_ARG);

    /* A stored client id of 64 characters is returned whole. */
    char long_id[80];
    load_bound(NULL, test_text(long_id, 64, 'c'));
    expect_client_id(k_mac, long_id);
    CHECK(xiaotai_identity_client_id(k_mac, out, sizeof(out)) == XIAOTAI_ERR_INVALID_SIZE);

    /* Prefix bounds: the longest prefix fills the 64-character client id. */
    char prefix[64];
    test_clear();
    const xiaotai_identity_config_t longest = {.client_prefix = test_text(prefix, 51, 'p')};
    CHECK(test_load(&longest) == XIAOTAI_IDENTITY_ABSENT);
    char client[80];
    CHECK(xiaotai_identity_client_id(k_mac, client, sizeof(client)) == XIAOTAI_OK && strlen(client) == 64U);
    CHECK(strncmp(client, prefix, 51) == 0 && strcmp(client + 51, "-020406080a0c") == 0);
    test_reset();
    const xiaotai_identity_config_t too_long = {.client_prefix = test_text(prefix, 52, 'p')};
    CHECK(xiaotai_identity_configure(&too_long) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_identity_config_t empty = {.client_prefix = ""};
    CHECK(xiaotai_identity_configure(&empty) == XIAOTAI_ERR_INVALID_ARG);

    test_clear();
    puts("identity_fingerprint: ok");
    return 0;
}
