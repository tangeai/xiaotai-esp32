// SPDX-License-Identifier: MIT
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"

static const xiaotai_contact_kind_def_t KIND_THREE = {
    .name = "third", .alias = "three", .server_type = "three",
};
static const xiaotai_contact_kind_def_t KIND_FOUR = {
    .name = "fourth", .alias = "four", .server_type = "four",
};
static const xiaotai_contact_kind_def_t KIND_FIVE = {
    .name = "fifth", .alias = "five", .server_type = "five",
};

static void registration(void)
{
    xiaotai_contact_kind_def_t duplicate = KIND_THREE;
    duplicate.name = "device";
    const xiaotai_contact_kind_def_t *kinds[] = {&TEST_KIND_DEVICE, &duplicate};
    harness_setup_t setup = {.kinds = kinds, .kind_count = 2};
    CHECK(harness_try_start(&setup) == XIAOTAI_ERR_INVALID_ARG);
    harness_stop();

    duplicate = KIND_THREE;
    duplicate.server_type = "device";
    CHECK(harness_try_start(&setup) == XIAOTAI_ERR_INVALID_ARG);
    harness_stop();

    duplicate = KIND_THREE;
    duplicate.alias = "dev";
    CHECK(harness_try_start(&setup) == XIAOTAI_ERR_INVALID_ARG);
    harness_stop();

    const xiaotai_contact_kind_def_t *full[] = {
        &TEST_KIND_DEVICE, &TEST_KIND_WECHAT, &KIND_THREE, &KIND_FOUR, &KIND_FIVE,
    };
    setup.kinds = full;
    setup.kind_count = 5;
    CHECK(harness_try_start(&setup) == XIAOTAI_ERR_NO_MEM);
    harness_stop();
    setup.kind_count = 4;
    harness_start(&setup);
    int kind = -1;
    CHECK(xiaotai_contacts_kind_count() == 4 && xiaotai_contacts_kind(3) == &KIND_FOUR);
    CHECK(xiaotai_contacts_kind_find("fOuR", &kind) == XIAOTAI_OK && kind == 3);
    CHECK(xiaotai_contacts_kind_find("fifth", &kind) == XIAOTAI_ERR_NOT_FOUND);
    harness_stop();
}

static void missing_default_kind(void)
{
    const xiaotai_contacts_config_t config = {.source = &TEST_SOURCE_VOIP, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 1};
    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200,
                         "{\"code\":0,\"data\":{\"contacts\":[{\"wx_open_id\":\"fixture\",\"wx_app_id\":\"app\"}]}}");
    CHECK(harness_status().fresh && harness_status().skipped == 1 && harness_status().total == 0);
    CHECK(harness_stats().entries_unregistered == 1);
    harness_stop();
}

int main(void)
{
    registration();
    missing_default_kind();
    return 0;
}
