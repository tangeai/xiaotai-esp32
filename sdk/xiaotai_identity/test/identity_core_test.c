// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_runtime_config.py
/* Identity core: validation limits, the comparison a signed re-binding
 * relies on (exact device id and key, whatever differs and wherever),
 * wiping, the readers before and after the engine init, configuration
 * rules and the init's own rules. */
#include "identity_test.h"

static xiaotai_identity_t make(const char *id, const char *key, const char *client)
{
    xiaotai_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_id, id);
    strcpy(identity.device_key, key);
    strcpy(identity.client_id, client);
    return identity;
}

static void validation(void)
{
    char a[300], b[300], c[300];
    xiaotai_identity_t identity = make("test-id", "test-secret", "test-client");
    CHECK(xiaotai_identity_valid(&identity));
    CHECK(!xiaotai_identity_valid(NULL));
    identity = make(test_text(a, 64, 'd'), test_text(b, 256, 'k'), test_text(c, 64, 'c'));
    CHECK(xiaotai_identity_valid(&identity));
    identity = make("d", "k", "");
    CHECK(xiaotai_identity_valid(&identity));
    identity = make("", "k", "");
    CHECK(!xiaotai_identity_valid(&identity));
    identity = make("d", "", "");
    CHECK(!xiaotai_identity_valid(&identity));
    /* Unterminated fields (one more character than the limit). */
    identity = make("d", "k", "c");
    memset(identity.device_id, 'd', sizeof(identity.device_id));
    CHECK(!xiaotai_identity_valid(&identity));
    identity = make("d", "k", "c");
    memset(identity.device_key, 'k', sizeof(identity.device_key));
    CHECK(!xiaotai_identity_valid(&identity));
    identity = make("d", "k", "c");
    memset(identity.client_id, 'c', sizeof(identity.client_id));
    CHECK(!xiaotai_identity_valid(&identity));
}

static void comparison(void)
{
    char a[300], b[300];
    CHECK(!xiaotai_identity_matches("test-id", "test-secret")); /* nothing loaded */
    test_clear();
    test_set_str("tirtc_cfg", "device_id", "test-id");
    test_set_str("tirtc_cfg", "secret", "test-secret");
    CHECK(!xiaotai_identity_matches("test-id", "test-secret")); /* stored, not loaded */
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    CHECK(xiaotai_identity_matches("test-id", "test-secret"));
    CHECK(!xiaotai_identity_matches("test-i", "test-secret"));
    CHECK(!xiaotai_identity_matches("test-idd", "test-secret"));
    CHECK(!xiaotai_identity_matches("Test-id", "test-secret"));
    CHECK(!xiaotai_identity_matches("test-id", "test-secreT"));
    CHECK(!xiaotai_identity_matches("test-id", "test-secret!"));
    CHECK(!xiaotai_identity_matches("test-id", "test-secre"));
    CHECK(!xiaotai_identity_matches("test-id", ""));
    CHECK(!xiaotai_identity_matches("", ""));
    CHECK(!xiaotai_identity_matches(NULL, "test-secret"));
    CHECK(!xiaotai_identity_matches("test-id", NULL));

    /* Fields at their limits, and texts longer than a field can hold. */
    test_clear();
    test_set_str("tirtc_cfg", "device_id", test_text(a, 64, 'd'));
    test_set_str("tirtc_cfg", "secret", test_text(b, 256, 'k'));
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    CHECK(xiaotai_identity_matches(a, b));
    char longer_a[300], longer_b[300];
    CHECK(!xiaotai_identity_matches(test_text(longer_a, 65, 'd'), b));
    CHECK(!xiaotai_identity_matches(a, test_text(longer_b, 257, 'k')));
    CHECK(!xiaotai_identity_matches(test_text(longer_a, 63, 'd'), b));

    /* Not VALID: nothing matches, not even what half a set holds. */
    test_clear();
    test_set_str("tirtc_cfg", "device_id", "test-id");
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_INCOMPLETE);
    CHECK(!xiaotai_identity_matches("test-id", ""));
}

static void readers(void)
{
    /* Before the engine init: nothing loaded, no lock needed. */
    test_clear();
    test_set_str("tirtc_cfg", "device_id", "test-id");
    test_set_str("tirtc_cfg", "secret", "test-secret");
    CHECK(xiaotai_identity_state() == XIAOTAI_IDENTITY_ABSENT);
    xiaotai_identity_t out;
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_identity_get(NULL) == XIAOTAI_ERR_INVALID_ARG);
    char id[80];
    CHECK(xiaotai_identity_device_id(id, sizeof(id)) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_identity_device_id(NULL, 8) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_identity_device_id(id, 0) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_identity_stats_t stats;
    memset(&stats, 0xFF, sizeof(stats));
    xiaotai_identity_stats(&stats);
    CHECK(stats.loads == 0U && stats.load_failures == 0U && stats.rejected_jobs == 0U);

    /* The init reads once; a second init is refused and reads nothing. */
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_VALID);
    CHECK(xiaotai_engine_identity_init() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_identity_stats(&stats);
    CHECK(stats.loads == 1U);
    CHECK(xiaotai_identity_get(&out) == XIAOTAI_OK && strcmp(out.device_id, "test-id") == 0);
    /* The copy is the caller's: changing it changes nothing loaded. */
    strcpy(out.device_key, "changed");
    xiaotai_identity_wipe(&out, sizeof(out));
    CHECK(xiaotai_identity_matches("test-id", "test-secret"));
}

static void configuration(void)
{
    test_clear();
    const xiaotai_identity_config_t bad_layout = {.layout = (xiaotai_identity_layout_t)7};
    CHECK(xiaotai_identity_configure(&bad_layout) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_identity_config_t bad_format = {.fingerprint = (xiaotai_fingerprint_format_t)7};
    CHECK(xiaotai_identity_configure(&bad_format) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_identity_config_t long_space = {.space = "sixteen_chars_xx"};
    CHECK(xiaotai_identity_configure(&long_space) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_identity_config_t empty_space = {.space = ""};
    CHECK(xiaotai_identity_configure(&empty_space) == XIAOTAI_ERR_INVALID_ARG);
    /* A refused configuration leaves the previous one. */
    CHECK(strcmp(xiaotai_identity_settings()->space, "tirtc_cfg") == 0);

    const xiaotai_identity_config_t named = {.space = "fifteen_chars_x"};
    CHECK(xiaotai_identity_configure(&named) == XIAOTAI_OK);
    CHECK(strcmp(xiaotai_identity_settings()->space, "fifteen_chars_x") == 0);
    CHECK(xiaotai_identity_configure(NULL) == XIAOTAI_OK);
    const xiaotai_identity_settings_t *settings = xiaotai_identity_settings();
    CHECK(settings->layout == XIAOTAI_IDENTITY_LAYOUT_STRINGS && strcmp(settings->space, "tirtc_cfg") == 0 &&
          settings->fingerprint == XIAOTAI_FINGERPRINT_UPPER_COLON && settings->client_prefix == NULL &&
          !settings->storage_error_unbound);

    /* A strings layout in another namespace reads there. */
    test_set_str("mine", "device_id", "test-id");
    test_set_str("mine", "secret", "test-secret");
    const xiaotai_identity_config_t mine = {.space = "mine"};
    CHECK(test_load(&mine) == XIAOTAI_IDENTITY_VALID);
    CHECK(test_load(NULL) == XIAOTAI_IDENTITY_ABSENT);

    /* Configuration belongs before the engine init. */
    CHECK(xiaotai_identity_configure(&mine) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_identity_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
}

static void wiping(void)
{
    uint8_t data[40];
    memset(data, 0xA5, sizeof(data));
    xiaotai_identity_wipe(data + 4, 32);
    for (size_t i = 0; i < sizeof(data); ++i) CHECK(data[i] == ((i >= 4 && i < 36) ? 0U : 0xA5U));
    xiaotai_identity_wipe(NULL, 8);
}

int main(void)
{
    validation();
    comparison();
    readers();
    configuration();
    wiping();
    test_clear();
    puts("identity_core: ok");
    return 0;
}
