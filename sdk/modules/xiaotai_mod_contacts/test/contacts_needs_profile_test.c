// SPDX-License-Identifier: MIT
#include "contacts_test.h"

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE};
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 1};
    harness_start(&setup);
    harness_online();
    harness_profile(false);
    CHECK(harness_status().loading && fake_platform_request_count() == 0);
    CHECK(harness_refresh() == XIAOTAI_OK && fake_platform_request_count() == 0);
    harness_profile(true);
    CHECK(fake_platform_request_count() == 1);
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    CHECK(harness_status().fresh && harness_status().total == 1);
    harness_offline();
    harness_online();
    CHECK(fake_platform_request_count() == 1);
    harness_profile(true);
    CHECK(fake_platform_request_count() == 2);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(0, 0));
    harness_stop();
    return 0;
}
