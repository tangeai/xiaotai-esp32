// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_network.c::platform_network
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "product_platform.h"
#include "product_portal.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/provisioning_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"

#define WAIT_MS 5000U

static const struct { char id[65], key[257]; } previous_identity = {
    .id = "test-device", .key = "not-a-real-key",
};
static const xiaotai_net_credentials_t previous = {.ssid = "test-old", .password = "test-password"};
static const xiaotai_net_submission_t submitted = {
    .credentials = {.ssid = "test-new", .password = "test-new-password"},
    .method = XIAOTAI_PROVISION_PORTAL,
};

typedef struct {
    xiaotai_net_credentials_t credentials;
    uint8_t identity[322];
    uint8_t marker;
} snapshot_t;
_Static_assert(sizeof(xiaotai_net_credentials_t) == 98 && sizeof(snapshot_t) == 421, "legacy fixture layout");

static snapshot_t read_snapshot(bool marked)
{
    snapshot_t snapshot = {0};
    size_t size = sizeof(snapshot.credentials);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", &snapshot.credentials, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(snapshot.credentials));
    size = sizeof(snapshot.identity);
    assert(xiaotai_kv_get_blob_strict("xmini", "identity", snapshot.identity, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(snapshot.identity));
    const xiaotai_err_t marker = xiaotai_kv_get_u8("xmini", "wifi_provision", &snapshot.marker, WAIT_MS);
    assert(marked ? marker == XIAOTAI_OK && snapshot.marker == 1U : marker == XIAOTAI_ERR_NOT_FOUND);
    return snapshot;
}

static void expect_candidate(const snapshot_t *stored)
{
    assert(!memcmp(&stored->credentials, &submitted.credentials, sizeof(stored->credentials)));
    assert(!memcmp(stored->identity, &previous_identity, sizeof(stored->identity)));
}

void product_reprovision_partial_prepare(bool cold)
{
    snapshot_t stored = {.credentials = previous, .marker = 1};
    memcpy(stored.identity, &previous_identity, sizeof(stored.identity));
    if (cold) {
        uint8_t *const bytes = (uint8_t *)&stored;
        for (size_t i = 0; i < sizeof(stored); ++i) {
            unsigned byte = 0;
            assert(scanf("%2x", &byte) == 1 && byte <= UINT8_MAX);
            bytes[i] = (uint8_t)byte;
        }
        assert(getchar() == '\n' && getchar() == EOF && stored.marker == 1U);
        expect_candidate(&stored);
    }
    product_platform_prepare(false);
    assert(xiaotai_kv_set("xmini", "wifi", &stored.credentials, sizeof(stored.credentials), WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set("xmini", "identity", &stored.identity, sizeof(stored.identity), WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_u8("xmini", "wifi_provision", stored.marker, WAIT_MS) == XIAOTAI_OK);
}

static void expect_portal(void)
{
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, WAIT_MS));
    xiaotai_net_provision_status_t net;
    xiaotai_ui_info_t ui = {0};
    const int64_t until = xiaotai_time_us() + INT64_C(5000000);
    do {
        xiaotai_net_provision_status(&net);
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (net.state == XIAOTAI_NET_PROVISION_OPEN && ui.ready && !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_SETUP)) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    assert(net.state == XIAOTAI_NET_PROVISION_OPEN && net.exclusive && net.error == XIAOTAI_OK &&
           net.reason == XIAOTAI_NET_PROVISION_REASON_REQUESTED && !net.restart_required &&
           (net.methods & XIAOTAI_PROVISION_PORTAL) && net.url[0] && product_portal_stack());
    assert(ui.ready && !ui.present_fault && !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_SETUP));
    assert(xiaotai_net_provisioning_boot() && !xiaotai_net_generation() && !xiaotai_restart_pending() &&
           !product_restarted());
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    assert(count < sizeof(records) / sizeof(records[0]));
    for (size_t i = 0; i < count; ++i) assert(records[i].call != XIAOTAI_NET_HOST_JOIN);
}

static void retry_store(void *unused)
{
    (void)unused;
    assert(xiaotai_provisioning_retry() == XIAOTAI_OK);
}

void product_reprovision_partial(bool cold)
{
    expect_portal();
    if (cold) {
        const snapshot_t snapshot = read_snapshot(true);
        expect_candidate(&snapshot);
        xiaotai_identity_t identity;
        assert(xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID && xiaotai_identity_get(&identity) == XIAOTAI_OK);
        assert(!strcmp(identity.device_id, previous_identity.id) && !strcmp(identity.device_key, previous_identity.key));
        puts("reprovision cold: marker=1, persisted candidate and bound identity intact; setup portal ready, no station join or restart");
        return;
    }

    assert(XIAOTAI_PROVISIONING_STORE_RETRY_MS == 0);
    assert(xiaotai_net_provision_submit(&submitted) == XIAOTAI_OK);
    const uint32_t token = product_platform_network_token();
    const xiaotai_net_host_step_t leave = {
        .result = XIAOTAI_OK, .event_count = 1,
        .events = {{.kind = XIAOTAI_NET_DRIVER_LOST, .token = token, .loss = XIAOTAI_NET_LOSS_REQUESTED}},
    };
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_LEAVE, &leave) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED,
           .token = token, .ssid = "test-new"}) == XIAOTAI_OK);
    xiaotai_kv_host_fail_key("wifi_provision", XIAOTAI_FAIL);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS,
           .token = token, .ipv4 = 42}) == XIAOTAI_OK);

    xiaotai_provisioning_status_t provision;
    const int64_t until = xiaotai_time_us() + INT64_C(5000000);
    do {
        xiaotai_provisioning_status(&provision);
        if (provision.phase == XIAOTAI_PROVISIONING_STORE_FAILED) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    fprintf(stderr, "partial store: phase=%d net_state=%d store_error=%d restart_pending=%u\n", provision.phase,
            provision.provision.state, provision.provision.error, (unsigned)provision.restart_pending);
    assert(provision.phase == XIAOTAI_PROVISIONING_STORE_FAILED && provision.provision.error == XIAOTAI_FAIL &&
           !provision.restart_pending && !xiaotai_restart_pending() && !product_restarted());
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    const snapshot_t snapshot = read_snapshot(true);
    expect_candidate(&snapshot);
    const uint8_t *const bytes = (const uint8_t *)&snapshot;
    fputs("XT-PARTIAL-WIFI: ", stdout);
    for (size_t i = 0; i < sizeof(snapshot); ++i) printf("%02x", (unsigned)bytes[i]);
    fputc('\n', stdout);

    product_expect_restart();
    product_on_owner(retry_store, NULL);
    const int64_t restarted = xiaotai_time_us() + INT64_C(5000000);
    while (!product_restarted() && xiaotai_time_us() < restarted) xiaotai_delay_ms(5);
    assert(product_restarted());
    const snapshot_t recovered = read_snapshot(false);
    expect_candidate(&recovered);
    puts("reprovision partial: marker erase failed without restart; explicit retry cleared the marker and restarted once");
}
