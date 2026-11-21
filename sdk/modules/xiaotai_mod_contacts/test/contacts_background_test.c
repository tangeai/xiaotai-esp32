// SPDX-License-Identifier: MIT
#include "contacts_test.h"

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE};
    const xiaotai_contacts_config_t config = {
        .fetch = XIAOTAI_CONTACTS_FETCH_IDLE_ONLY, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
    };
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 1};
    harness_start(&setup);
    harness_online();
    CHECK(fake_platform_request_count() == 1);
    CHECK(fake_platform_request(0)->flags == XIAOTAI_PLATFORM_BACKGROUND);
    harness_activity_begin();
    fake_platform_answer(0, XIAOTAI_ERR_BUSY, 0, NULL);
    CHECK(fake_platform_request_count() == 1 && harness_status().loading);
    CHECK(harness_stats().fetches_deferred > 0 && harness_stats().fetch_failures == 0);
    CHECK(harness_refresh() == XIAOTAI_OK && fake_platform_request_count() == 1);
    harness_activity_end();
    CHECK(fake_platform_request_count() == 2);
    CHECK(fake_platform_request(1)->flags == XIAOTAI_PLATFORM_BACKGROUND);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(2, 0));
    CHECK(!harness_status().loading && harness_status().fresh && harness_status().total == 2);
    harness_stop();
    return 0;
}
