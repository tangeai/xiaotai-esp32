// SPDX-License-Identifier: MIT
/* The product glue generated from this engine's manifest compiles and hands
 * the engine's real xiaotai_net_configure exactly the values a product
 * configuration writes: a JSON list for retry.steps_ms becomes a static table
 * with its step_count (the fixed wait table of products that never doubled),
 * nested fields and constants arrive unchanged, fields a product leaves out
 * stay zero (the contract's defaults), and every section passes the engine's
 * own checks. The glue comes from test/net_glue.py at build time, so a
 * manifest that can no longer express a product's values fails here.
 *
 * Only the configuration file of the engine is linked (src/config.c): the
 * glue runs before any engine exists. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/net.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

xiaotai_err_t net_glue_fixed_table(void);
xiaotai_err_t net_glue_doubling(void);
xiaotai_err_t net_glue_plain_serial(void);
xiaotai_err_t net_glue_szpi(void);
xiaotai_err_t net_glue_cores3(void);
xiaotai_err_t net_glue_waveshare(void);
xiaotai_err_t net_glue_wt99(void);

static xiaotai_net_config_t s_seen;
static xiaotai_net_config_t s_effective;

static void forget(void)
{
    net_config_reset();
    memset(&s_seen, 0xA5, sizeof(s_seen));
}

static void read_back(void)
{
    net_config_configured(&s_seen);
    net_config_resolve(&s_seen, &s_effective);
}

static int check_shipping_retry(const char *product, xiaotai_err_t (*configure)(void), bool fixed)
{
    /* Legacy S3 wifi_manager's six steps (including its 15-second fifth step).
     * The P4 products did not opt into that table. */
    const uint32_t steps[] = {1000, 2000, 4000, 8000, fixed ? 15000U : 16000U, 30000};
    forget();
    CHECK(configure() == XIAOTAI_OK);
    read_back();
    for (uint32_t i = 1; i <= 1000; ++i) {
        const uint32_t expected = steps[i > 6 ? 5 : i - 1];
        const uint32_t actual = net_backoff_wait_ms(&s_effective.retry, i, XIAOTAI_NET_LOSS_UNKNOWN, 0, NULL);
        if (actual != expected || net_backoff_give_up(&s_effective.retry, i)) {
            fprintf(stderr, "%s: retry %" PRIu32 " wanted %" PRIu32 " ms, got %" PRIu32 " ms\n",
                    product, i, expected, actual);
            return 1;
        }
    }
    CHECK(net_backoff_wait_ms(&s_effective.retry, UINT32_MAX, XIAOTAI_NET_LOSS_UNKNOWN, 0, NULL) == 30000);
    CHECK((s_seen.retry.steps_ms != NULL) == fixed);
    CHECK(s_seen.retry.step_count == (fixed ? 6U : 0U));
    printf("%s: retry 5 = %" PRIu32 " ms; policy capped through 1000 attempts\n", product, steps[4]);
    return 0;
}

int main(void)
{
    /* Fixed table: six steps, the exact deployed waits, and nothing else of the
     * retry policy set. The table is read after the glue returned: the engine
     * keeps the pointer (static storage), not a copy. */
    forget();
    CHECK(net_glue_fixed_table() == XIAOTAI_OK);
    read_back();
    static const uint32_t table[] = {1000, 2000, 4000, 8000, 15000, 30000};
    CHECK(s_seen.retry.step_count == 6);
    CHECK(s_seen.retry.step_count <= XIAOTAI_NET_RETRY_STEPS_MAX);
    CHECK(s_seen.retry.steps_ms != NULL);
    CHECK(memcmp(s_seen.retry.steps_ms, table, sizeof(table)) == 0);
    CHECK(s_seen.retry.fast_retries == 0 && s_seen.retry.jitter_percent == 0);
    CHECK(!s_seen.retry.grow_from_previous_base && !s_seen.retry.join_immediately_on_radio_start);
    CHECK(s_seen.retry.first_ms == 0 && s_seen.retry.max_ms == 0 && s_seen.retry.max_credentials_ms == 0);
    CHECK(s_seen.retry.retries_max == 0);
    CHECK(s_seen.store_unverified && s_seen.driver_storage);
    CHECK(s_seen.dns_fallback == XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY);
    CHECK(s_seen.layout == XIAOTAI_NET_STORE_RECORD && s_seen.space == NULL);
    CHECK(s_seen.unverified_methods == 0 && !s_seen.serial_short_passwords);
    /* As the engine uses it: the record layout's namespace of deployed
     * devices, five history entries, the table pointer kept. */
    CHECK(strcmp(s_effective.space, "wifi_cfg") == 0 && s_effective.history_capacity == 5);
    CHECK(s_effective.retry.steps_ms == s_seen.retry.steps_ms && s_effective.retry.step_count == 6);

    /* Doubling: the caps and jitter of the doubling backoff, no table. */
    forget();
    CHECK(net_glue_doubling() == XIAOTAI_OK);
    read_back();
    CHECK(s_seen.retry.fast_retries == 3 && s_seen.retry.jitter_percent == 25);
    CHECK(s_seen.retry.grow_from_previous_base && s_seen.retry.join_immediately_on_radio_start);
    CHECK(s_seen.retry.first_ms == 2000 && s_seen.retry.max_ms == 60000);
    CHECK(s_seen.retry.max_credentials_ms == 300000);
    CHECK(s_seen.retry.steps_ms == NULL && s_seen.retry.step_count == 0);
    CHECK(s_seen.dns_fallback == XIAOTAI_NET_DNS_FALLBACK_NEVER && !s_seen.driver_storage);
    CHECK(s_effective.retry.first_ms == 2000 && s_effective.retry.max_ms == 60000 &&
          s_effective.retry.max_credentials_ms == 300000);
    CHECK(s_effective.retry.grow_from_previous_base && s_effective.retry.join_immediately_on_radio_start);

    /* Plain layout: namespace string, retry limit, serial rules. */
    forget();
    CHECK(net_glue_plain_serial() == XIAOTAI_OK);
    read_back();
    CHECK(s_seen.layout == XIAOTAI_NET_STORE_PLAIN);
    CHECK(s_seen.space != NULL && strcmp(s_seen.space, "net_plain") == 0);
    CHECK(s_seen.retry.fast_retries == 10 && s_seen.retry.retries_max == 10);
    CHECK(s_seen.retry.steps_ms == NULL && s_seen.retry.step_count == 0);
    CHECK(s_seen.unverified_methods == XIAOTAI_PROVISION_SERIAL);
    CHECK(s_seen.serial_short_passwords && !s_seen.store_unverified);
    CHECK(s_seen.dns_fallback == XIAOTAI_NET_DNS_FALLBACK_NEVER);
    CHECK(strcmp(s_effective.space, "net_plain") == 0);
    /* Without a retry cap the doubling defaults apply (1 s, 30 s, 30 s). */
    CHECK(s_effective.retry.first_ms == 1000 && s_effective.retry.max_ms == 30000 &&
          s_effective.retry.max_credentials_ms == 30000);

    /* The glue of a section the engine refuses reports the refusal: the plain
     * section's serial rule without its layout is not a valid product. */
    forget();
    xiaotai_net_config_t bad = {.serial_short_passwords = true};
    CHECK(xiaotai_net_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    read_back();
    CHECK(!s_seen.serial_short_passwords);

    int mismatches = check_shipping_retry("szpi-s3", net_glue_szpi, true);
    mismatches += check_shipping_retry("cores3-lite", net_glue_cores3, true);
    mismatches += check_shipping_retry("waveshare-p4", net_glue_waveshare, false);
    mismatches += check_shipping_retry("wt99p461-s1", net_glue_wt99, false);
    if (mismatches) return 1;
    puts("net_product_glue: ok");
    return 0;
}
