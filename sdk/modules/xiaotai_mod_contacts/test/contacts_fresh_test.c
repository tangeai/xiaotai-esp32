// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::contacts_response
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"

static void expect_lookups(uint32_t flags, xiaotai_err_t err)
{
    xiaotai_contact_t contact;
    memset(&contact, 0, sizeof(contact));
    size_t matches = 0;
    CHECK(xiaotai_contacts_at(0, flags, &contact) == err);
    CHECK(xiaotai_contacts_find("d00", 0, flags, &contact) == err);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d00", 0, flags, &contact) == err);
    CHECK(xiaotai_contacts_first(0, flags, &contact) == err);
    CHECK(xiaotai_contacts_match("Dev 00", 0, NULL, flags, &contact, &matches) == err);
    CHECK(matches == (err == XIAOTAI_OK ? 1U : 0U));
    CHECK(strcmp(contact.id, err == XIAOTAI_OK ? "d00" : "") == 0);
}

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE};
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE, .at_device_dial_prefix = 8};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 1};
    harness_start(&setup);
    expect_lookups(0, XIAOTAI_ERR_INVALID_STATE);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(2, 0));
    expect_lookups(0, XIAOTAI_OK);
    expect_lookups(8U, XIAOTAI_ERR_INVALID_ARG);
    CHECK(harness_refresh() == XIAOTAI_OK);
    const uint32_t stale = XIAOTAI_CONTACTS_STALE_OK;
    expect_lookups(0, XIAOTAI_ERR_INVALID_STATE);
    expect_lookups(stale, XIAOTAI_OK);
    fake_platform_signal("contacts_update");
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(0, 0));
    fake_platform_answer(2, XIAOTAI_ERR_TIMEOUT, 0, NULL);
    CHECK(strstr(fake_platform_request(3)->path, "/pending"));
    fake_platform_answer(3, XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"pending\":[]}}");
    CHECK(harness_stats().fetch_failures == 1);
    expect_lookups(0, XIAOTAI_ERR_INVALID_STATE);
    expect_lookups(stale, XIAOTAI_OK);
    xiaotai_contact_t display[2];
    CHECK(xiaotai_contacts_page(display, 2) == 2 && strcmp(display[1].id, "d01") == 0);
    /* Generation is already changed while binding.needed is still queued. */
    CHECK(xiaotai_identity_generation_advance() != harness_status().generation);
    CHECK(xiaotai_contacts_page(display, 2) == 0);
    harness_emit(XIAOTAI_EVENT_BINDING_NEEDED, 1, NULL);
    expect_lookups(0, XIAOTAI_ERR_INVALID_STATE);
    expect_lookups(stale, XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_contacts_page(display, 2) == 0);
    harness_stop();
    return 0;
}
