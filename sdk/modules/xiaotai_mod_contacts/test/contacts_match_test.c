// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_contacts
#include <string.h>
#include "contacts_test.h"

#define DAD_NAME "\xe7\x88\xb8\xe7\x88\xb8"
#define MOM_NAME "\xe5\xa6\x88\xe5\xa6\x88"
#define UNKNOWN_NAME "\xe6\x9c\xaa\xe7\x9f\xa5"

static void legacy_name_first(const harness_setup_t *setup)
{
    static const char ordinary[] =
        "{\"type\":\"device\",\"device_id\":\"dad\",\"remark\":\"" DAD_NAME "\"},"
        "{\"type\":\"device\",\"device_id\":\"mom\",\"remark\":\"" MOM_NAME "\"}";
    static const char duplicate[] =
        "{\"type\":\"device\",\"device_id\":\"dad\",\"remark\":\"" DAD_NAME "\"},"
        "{\"type\":\"device\",\"device_id\":\"mom\",\"remark\":\"" DAD_NAME "\"},"
        "{\"type\":\"device\",\"device_id\":\"latin\",\"remark\":\"Dad\"}";
    harness_start(setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply(ordinary));
    xiaotai_contact_t contact = {0};
    size_t count = 0;
    CHECK(xiaotai_contacts_match(DAD_NAME, XIAOTAI_CONTACT_ANY, "", 0, &contact, &count) == XIAOTAI_OK);
    CHECK(count == 1 && !strcmp(contact.id, "dad"));
    CHECK(xiaotai_contacts_match(DAD_NAME, 0, "dad", 0, &contact, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_match(DAD_NAME, XIAOTAI_CONTACT_ANY, "mom", 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_match("", XIAOTAI_CONTACT_ANY, "dad", 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_match(UNKNOWN_NAME, XIAOTAI_CONTACT_ANY, "", 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_match(DAD_NAME, 1, "dad", 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_call_reply(duplicate));
    CHECK(xiaotai_contacts_match(DAD_NAME, XIAOTAI_CONTACT_ANY, "dad", 0, &contact, &count) ==
          XIAOTAI_ERR_INVALID_STATE);
    CHECK(count == 2);
    CHECK(xiaotai_contacts_match(" DAD ", XIAOTAI_CONTACT_ANY, "latin", 0, &contact, &count) == XIAOTAI_OK);
    CHECK(count == 1 && !strcmp(contact.id, "latin"));
    harness_stop();
}

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2};
    harness_start(&setup);
    harness_online();
    const char *entries =
        "{\"type\":\"device\",\"device_id\":\"one\",\"remark\":\"A LICE\"},"
        "{\"type\":\"device\",\"device_id\":\"nameless\"},"
        "{\"type\":\"voip\",\"device_id\":\"two\",\"remark\":\"Alice\",\"wx_app_id\":\"app\"}";
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply(entries));
    xiaotai_contact_t contact = {0};
    size_t matches = 0;
    CHECK(xiaotai_contacts_match(" aLi\tCE ", 0, "one", 0, &contact, &matches) == XIAOTAI_OK);
    CHECK(matches == 1 && strcmp(contact.id, "one") == 0);
    CHECK(xiaotai_contacts_match("Alice", XIAOTAI_CONTACT_ANY, "one", 0, &contact, &matches) ==
          XIAOTAI_ERR_INVALID_STATE);
    CHECK(matches == 2 && strcmp(contact.id, "one") == 0);
    CHECK(xiaotai_contacts_match("Alice", 0, "two", 0, &contact, &matches) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(matches == 0 && strcmp(contact.id, "one") == 0);
    CHECK(xiaotai_contacts_match("", 0, "one", 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_match("ali", 0, NULL, 0, &contact, NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_match("nameless", 0, NULL, 0, &contact, NULL) == XIAOTAI_OK);
    CHECK(strcmp(contact.id, "nameless") == 0);
    CHECK(xiaotai_contacts_match("Alice", 1, NULL, 0, &contact, NULL) == XIAOTAI_OK);
    CHECK(strcmp(contact.id, "two") == 0);
    harness_stop();
    legacy_name_first(&setup);
    return 0;
}
