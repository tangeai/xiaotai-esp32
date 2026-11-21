// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
// Replaces legacy: tests/xmini/xmini_business/test_business.c::pending_response
#include <stdio.h>
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"

static char text[4096];
static void line(void *unused, const char *value)
{
    (void)unused;
    CHECK(strlen(text) + strlen(value) + 2 < sizeof(text));
    strcat(text, value);
    strcat(text, "\n");
}
static const xiaotai_reply_t reply = {.write = line};

static xiaotai_err_t at(const char *cmd)
{
    text[0] = 0;
    const xiaotai_err_t result = xiaotai_command_run(cmd, &reply, 1000);
    harness_drain();
    return result;
}

static void report(void *ctx) { (void)ctx; xiaotai_diag_report("contacts", &reply); }

static void pending_replies(void)
{
    const struct { xiaotai_err_t transport; int http, shown; const char *body; } cases[] = {
        {XIAOTAI_OK, 200, 40209, "{\"code\":40209,\"msg\":\"private server text\"}"},
        {XIAOTAI_OK, 200, 40211, "{\"code\":40211}"},
        {XIAOTAI_OK, 200, 45001, "{\"code\":45001}"},
        {XIAOTAI_ERR_TIMEOUT, 200, XIAOTAI_ERR_TIMEOUT, "{\"code\":40209}"},
        {XIAOTAI_OK, 503, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":40209}"},
        {XIAOTAI_ERR_INVALID_RESPONSE, 503, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":200}"},
        {XIAOTAI_OK, 200, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":0}"},
        {XIAOTAI_OK, 200, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":-3}"},
        {XIAOTAI_OK, 200, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":200}"},
        {XIAOTAI_OK, 200, XIAOTAI_ERR_INVALID_RESPONSE, "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\"}]}}"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const size_t count = fake_platform_request_count();
        CHECK(at("AT+CONTACTS PENDING 0") == XIAOTAI_OK && strstr(text, "loading=1"));
        fake_platform_answer(count, cases[i].transport, cases[i].http, cases[i].body);
        CHECK(at("AT+CONTACTS PENDING") == XIAOTAI_OK);
        char expected[76];
        (void)snprintf(expected, sizeof(expected), "+PENDING: cached=0 total=0 error=%d revision=", cases[i].shown);
        if (!strstr(text, expected)) fprintf(stderr, "Pending result [%s], expected [%s]\n", text, expected);
        CHECK(strstr(text, expected) && strstr(text, "page=0 loading=0"));
        CHECK(!strstr(text, "+PENDING-ITEM:") && !strstr(text, "private server text"));
        CHECK(fake_platform_request_count() == count + 1U);
    }
    const size_t count = fake_platform_request_count();
    CHECK(at("AT+CONTACTS PENDING 0") == XIAOTAI_OK && strstr(text, "loading=1"));
    fake_platform_answer(count, XIAOTAI_OK, 200,
        "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"foreign-peer\"}]}}");
    CHECK(at("AT+CONTACTS PENDING") == XIAOTAI_OK && strstr(text, "+PENDING: cached=1 total=1 error=0 revision="));
    CHECK(strstr(text, "loading=0") && strstr(text, "+PENDING-ITEM: index=0 id=foreign-peer"));
    CHECK(fake_platform_request_count() == count + 1U);
}

static void command_body(const char *input, const char *body)
{
    const size_t count = fake_platform_request_count();
    CHECK(at(input) == XIAOTAI_OK && fake_platform_request_count() == count + 1U);
    CHECK(!strcmp(fake_platform_request(count)->body, body));
    fake_platform_answer(count, XIAOTAI_OK, 200, "{\"code\":40211}");
}

static void old_remark_bounds(void)
{
    char cmd[768], body[384];
    const size_t arg = (size_t)snprintf(cmd, sizeof(cmd), "AT+CONTACT REMARK TIR_A ");
    const size_t start = (size_t)snprintf(body, sizeof(body), "{\"peer_id\":\"TIR_A\",\"remark\":\"");
    for (size_t i = 0; i < 256U; ++i) memcpy(cmd + arg + 2U * i, "61", 2);
    cmd[arg + 512U] = 0;
    const size_t before = fake_platform_request_count();
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_SIZE); /* Old parser admitted this; the business layer rejected it. */
    memcpy(cmd + arg + 512U, "61", 3);
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_SIZE);
    cmd[arg + 130U] = 0;
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(fake_platform_request_count() == before);
    cmd[arg + 128U] = 0;
    memset(body + start, 'a', 64U);
    memcpy(body + start + 64U, "\"}", 3U);
    command_body(cmd, body);

    for (size_t i = 0; i < 64U; ++i) {
        memcpy(cmd + arg + 8U * i, "f09f9880", 8U);
        memcpy(body + start + 4U * i, "\xf0\x9f\x98\x80", 4U);
    }
    memcpy(body + start + 256U, "\"}", 3U);
    memcpy(cmd + arg + 512U, "f09f9880", 9U);
    const size_t after_ascii = fake_platform_request_count();
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_SIZE); /* 65 characters, 260 bytes. */
    cmd[arg + 512U] = 0;
    command_body(cmd, body);
    memcpy(cmd + arg + 504U, "6161", 5U);
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_SIZE); /* 65 characters, 254 bytes. */
    memcpy(cmd + arg + 504U, "f09f98", 7U);
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_ARG); /* Truncated fourth byte. */
    CHECK(fake_platform_request_count() == after_ascii + 1U);
}

static void old_command_bounds(const xiaotai_contact_kind_def_t *const *kinds)
{
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE, .capacity = 8};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2};
    harness_start(&setup);
    harness_online();
    const char *list = harness_numbered_reply(40, 2);
    fake_platform_answer(0, XIAOTAI_OK, 200, list);
    CHECK(at("AT+CONTACTS PAGE ALL") == XIAOTAI_OK && strstr(text, "filter=all page=0"));
    CHECK(at("AT+CONTACTS PAGE ALL 0") == XIAOTAI_OK && strstr(text, "filter=all page=0"));
    CHECK(at("AT+CONTACTS PAGE DEV 1") == XIAOTAI_OK && strstr(text, "filter=dev page=1 loading=1"));
    fake_platform_answer(1, XIAOTAI_OK, 200, list);
    CHECK(at("AT+CONTACTS") == XIAOTAI_OK && strstr(text, "filter=dev page=1 loading=0"));
    CHECK(strstr(text, "id=d16") && !strstr(text, "source=wechat"));
    CHECK(at("AT+CONTACTS PAGE WX 65535") == XIAOTAI_OK && strstr(text, "filter=wx page=2 loading=1"));
    fake_platform_answer(2, XIAOTAI_OK, 200, list);
    CHECK(at("AT+CONTACTS PAGE WX 00002") == XIAOTAI_OK && strstr(text, "filter=wx page=2"));
    CHECK(strstr(text, "name_hex=5778203333") && !strstr(text, "source=device"));
    const size_t pending = fake_platform_request_count();
    CHECK(at("AT+CONTACTS PENDING 65535") == XIAOTAI_OK && strstr(text, "page=65535 loading=1"));
    fake_platform_answer(pending, XIAOTAI_OK, 200,
        "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"TIR_test-01\"}]}}");
    CHECK(at("AT+CONTACTS PENDING") == XIAOTAI_OK && strstr(text, "page=0 loading=0"));
    CHECK(strstr(text, "index=0 id=TIR_test-01"));

    char cmd[96], body[96], id[66];
    memset(id, 'A', 65);
    id[64] = id[65] = 0;
    snprintf(cmd, sizeof(cmd), "AT+CONTACT REQUEST %s", id);
    snprintf(body, sizeof(body), "{\"target_device_id\":\"%s\"}", id);
    command_body(cmd, body);
    id[64] = 'A';
    snprintf(cmd, sizeof(cmd), "AT+CONTACT REQUEST %s", id);
    const size_t before_id = fake_platform_request_count();
    CHECK(at(cmd) == XIAOTAI_ERR_INVALID_ARG && fake_platform_request_count() == before_id);
    command_body("AT+CONTACT REQUEST TIR_test-01", "{\"target_device_id\":\"TIR_test-01\"}");
    command_body("AT+CONTACT ACCEPT TIR_test-01", "{\"peer_device_id\":\"TIR_test-01\",\"action\":\"accept\"}");
    command_body("AT+CONTACT REJECT TIR_test-01", "{\"peer_device_id\":\"TIR_test-01\",\"action\":\"reject\"}");
    command_body("AT+CONTACT DELETE TIR_test-01", "{\"peer_id\":\"TIR_test-01\"}");
    command_body("AT+CONTACT REMARK TIR_A 412042", "{\"peer_id\":\"TIR_A\",\"remark\":\"A B\"}");
    command_body("AT+CONTACT REMARK TIR_A -", "{\"peer_id\":\"TIR_A\",\"remark\":\"\"}");
    snprintf(body, sizeof(body), "{\"peer_id\":\"TIR_A\",\"remark\":\"%s\"}", "\xe5\xb0\x8f\xe9\x92\x9b");
    command_body("AT+CONTACT REMARK TIR_A e5b08fe9929b", body);
    command_body("AT+CONTACT REMARK TIR_A E5B08FE9929B", body);
    old_remark_bounds();

    static const char *const bad[] = {
        "AT+CONTACT", "AT+CONTACT REQUEST", "AT+CONTACT BAD TIR_A", "AT+CONTACT ACCEPT bad/id",
        "AT+CONTACT REJECT a.b", "AT+CONTACT DELETE a:b", "AT+CONTACT REQUEST a+b",
        "AT+CONTACT REQUEST id extra", "AT+CONTACT ACCEPT id extra", "AT+CONTACT REMARK id",
        "AT+CONTACT REMARK id 6", "AT+CONTACT REMARK id 6g", "AT+CONTACT REMARK id 00",
        "AT+CONTACT REMARK id 410042", "AT+CONTACT REMARK id --", "AT+CONTACT REMARK id - extra",
        "AT+CONTACT REMARK id 61 extra", "AT+CONTACT REMARK id ff", "AT+CONTACT REMARK id c0af",
        "AT+CONTACT REMARK id eda080", "AT+CONTACT REMARK id f4908080", "AT+CONTACT REMARK id e5b0",
        "AT+CONTACTS 1", "AT+CONTACTS PAGE ALL -1", "AT+CONTACTS PAGE DEV +1",
        "AT+CONTACTS PAGE WX 65536", "AT+CONTACTS PAGE WX 12345678901234567890",
        "AT+CONTACTS PAGE ALL 0 extra", "AT+CONTACTS PENDING -1", "AT+CONTACTS PENDING 65536",
        "AT+CONTACTS PENDING 0 1", "AT+CONTACTS PENDING extra",
    };
    const size_t before = fake_platform_request_count();
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        const xiaotai_err_t err = at(bad[i]);
        if (err != XIAOTAI_ERR_INVALID_ARG) fprintf(stderr, "Invalid contact command [%s] returned %d\n", bad[i], err);
        CHECK(err == XIAOTAI_ERR_INVALID_ARG);
    }
    CHECK(fake_platform_request_count() == before);
    fake_platform_refuse_next(XIAOTAI_ERR_NO_MEM);
    CHECK(at("AT+CONTACT REQUEST TIR_test-01") == XIAOTAI_ERR_NO_MEM);
    CHECK(fake_platform_request_count() == before);
    printf("contact_at id=64/65 remark_chars=64/65 remark_bytes=256/257 invalid=%zu page=65535/65536\n",
           sizeof(bad) / sizeof(*bad));
    harness_stop();
}

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2};
    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply("{\"type\":\"device\",\"device_id\":\"d01\",\"online\":true,\"remark\":\"My Peer\"},"
        "{\"type\":\"voip\",\"device_id\":\"private-wx\",\"wx_app_id\":\"app\",\"remark\":\"WX\"}"));
    CHECK(at("AT+CONTACTS") == XIAOTAI_OK);
    CHECK(strstr(text, "+CONTACTS: cached=2 total=2 filter=all page=0 loading=0 error=0"));
    CHECK(strstr(text, "+CONTACT: index=0 source=device online=1 id=d01 name_hex=4D792050656572"));
    CHECK(strstr(text, "+CONTACT: index=1 source=wechat online=- id=- name_hex=5758") && !strstr(text, "private-wx"));
    CHECK(at("AT+CONTACTS PAGE wX 65535") == XIAOTAI_OK && strstr(text, "filter=wx page=0"));
    CHECK(strstr(text, "index=0 source=wechat") && !strstr(text, "source=device"));
    CHECK(at("AT+CONTACTS PAGE device") == XIAOTAI_OK && strstr(text, "filter=dev"));
    CHECK(at("AT+CONTACTS PAGE aLl") == XIAOTAI_OK && strstr(text, "filter=all"));
    CHECK(at("AT+CONTACTS PAGE bad") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(at("AT+CONTACTS PAGE DEV 65536") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(at("AT+CONTACTS REFRESH") == XIAOTAI_OK && fake_platform_request_count() == 2);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    CHECK(at("AT+CONTACTS PENDING 0") == XIAOTAI_OK && strstr(text, "loading=1"));
    fake_platform_answer(2, XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"p01\"}]}}");
    CHECK(at("AT+CONTACTS PENDING") == XIAOTAI_OK && strstr(text, "+PENDING: cached=1 total=1 error=0 revision="));
    CHECK(strstr(text, "page=0 loading=0") && strstr(text, "+PENDING-ITEM: index=0 id=p01"));
    CHECK(at("AT+CONTACT ACCEPT nonexistent") == XIAOTAI_ERR_NOT_FOUND);
    CHECK(at("AT+CONTACT ACCEPT p01") == XIAOTAI_OK);
    CHECK(at("AT+CONTACT REJECT p01") == XIAOTAI_ERR_BUSY);
    fake_platform_answer(3, XIAOTAI_OK, 200, "{\"code\":40211}");
    CHECK(at("AT+CONTACT REJECT p01") == XIAOTAI_OK);
    fake_platform_answer(4, XIAOTAI_OK, 200, "{\"code\":40211}");
    CHECK(at("AT+CONTACT REMARK p01 610062") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(at("AT+CONTACT REMARK p01 EDA080") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(at("AT+CONTACT REMARK p01 -") == XIAOTAI_OK);
    CHECK(!strcmp(fake_platform_request(5)->body, "{\"peer_id\":\"p01\",\"remark\":\"\"}"));
    fake_platform_answer(5, XIAOTAI_OK, 200, "{\"code\":40211}");
    CHECK(at("AT+CONTACT REQUEST p01") == XIAOTAI_OK);
    fake_platform_answer(6, XIAOTAI_OK, 200, "{\"code\":40211}");
    CHECK(at("AT+CONTACT DELETE p01") == XIAOTAI_OK);
    fake_platform_answer(7, XIAOTAI_OK, 200, "{\"code\":40211}");
    CHECK(at("AT+CONTACTS?") == XIAOTAI_ERR_NOT_FOUND && at("AT+PENDING") == XIAOTAI_ERR_NOT_FOUND);
    text[0] = 0;
    CHECK(xiaotai_run(report, NULL) == XIAOTAI_OK);
    harness_drain();
    CHECK(strstr(text, "changes=5 change_failures=5 pending_fetches=1 pending_failures=0"));
    CHECK(!strstr(text, "private-wx") && !strstr(text, "p01"));
    pending_replies();
    harness_stop();
    old_command_bounds(kinds);
    return 0;
}
