// SPDX-License-Identifier: MIT
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/identity.h"

static void expect_contact(int kind, uint16_t index, uint32_t flags, const char *id)
{
    xiaotai_contact_t contact = {0};
    CHECK(xiaotai_contacts_at_kind(kind, index, flags, &contact) == XIAOTAI_OK);
    CHECK(contact.kind == kind && strcmp(contact.id, id) == 0);
}

static void expect_error(int kind, uint16_t index, uint32_t flags, xiaotai_err_t expected)
{
    xiaotai_contact_t contact = {.id = "sentinel"};
    CHECK(xiaotai_contacts_at_kind(kind, index, flags, &contact) == expected);
    CHECK(strcmp(contact.id, "sentinel") == 0);
}

int main(void)
{
    static const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
    const xiaotai_contacts_config_t config = {.capacity = 8, .page_size = 4, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2};
    harness_start(&setup);
    int device = harness_kind(0), wechat = harness_kind(1);
    const uint32_t stale = XIAOTAI_CONTACTS_STALE_OK;
    expect_error(wechat, 1, stale, XIAOTAI_ERR_INVALID_STATE);
    expect_error(XIAOTAI_CONTACT_ANY, 1, 0, XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_contacts_at_kind(wechat, 1, 0, NULL) == XIAOTAI_ERR_INVALID_ARG);

    harness_online();
    const char *list = harness_numbered_reply(30, 2);
    fake_platform_answer(0, XIAOTAI_OK, 200, list);
    CHECK(harness_status().truncated);
    expect_contact(wechat, 1, 0, "w03");
    expect_contact(wechat, 3, 0, "w07");
    expect_error(wechat, 4, 0, XIAOTAI_ERR_NOT_FOUND);
    expect_error(wechat, 1, 8U, XIAOTAI_ERR_INVALID_ARG);
    CHECK(fake_platform_request_count() == 1 && harness_status().page == 0);

    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 3) == XIAOTAI_OK);
    expect_error(wechat, 1, 0, XIAOTAI_ERR_INVALID_STATE);
    expect_contact(wechat, 1, stale, "w03");
    CHECK(fake_platform_request_count() == 2);
    fake_platform_answer(1, XIAOTAI_OK, 200, list);
    /* An unfiltered suffix contains w13 but cannot establish its absolute
     * ordinal. The selected page and separately kept first remain valid. */
    xiaotai_contact_t current;
    CHECK(xiaotai_contacts_at(1, 0, &current) == XIAOTAI_OK && strcmp(current.id, "w13") == 0);
    expect_error(wechat, 1, stale, XIAOTAI_ERR_NOT_FOUND);
    expect_error(wechat, 6, stale, XIAOTAI_ERR_NOT_FOUND);
    expect_contact(wechat, 0, 0, "w01");
    CHECK(fake_platform_request_count() == 2 && harness_status().page == 3);

    /* The typed window's offset was counted within that kind by the parser. */
    CHECK(harness_show(wechat, 2) == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == 3);
    fake_platform_answer(2, XIAOTAI_OK, 200, list);
    CHECK(xiaotai_contacts_at(0, 0, &current) == XIAOTAI_OK && strcmp(current.id, "w17") == 0);
    expect_contact(wechat, 8, 0, "w17");
    expect_error(wechat, 1, 0, XIAOTAI_ERR_NOT_FOUND);
    expect_error(device, 8, 0, XIAOTAI_ERR_NOT_FOUND);
    expect_contact(device, 0, 0, "d00");
    CHECK(fake_platform_request_count() == 3 && harness_status().page == 2 && harness_status().kind == wechat);

    CHECK(xiaotai_identity_generation_advance() != harness_status().generation);
    expect_error(wechat, 8, stale, XIAOTAI_ERR_INVALID_STATE);
    expect_error(wechat, 0, stale, XIAOTAI_ERR_INVALID_STATE);
    harness_stop();
    return 0;
}
