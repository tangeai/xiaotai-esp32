// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::contacts_response
// Replaces legacy: tests/xmini/test_platform_routes.c::platform_routes
// Replaces legacy: tests/regression/test_device_profile_response.py
// Replaces legacy: tests/mz28/test_voip_join_lifetime.c::contacts_response
// Replaces legacy: tests/mz28/test_voip_join_lifetime.c::voip_authorization_refresh
#include <string.h>
#include "contacts_test.h"
#include "xiaotai_mem_host.h"

static const xiaotai_contact_kind_def_t *const KINDS[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};

static void test_refresh(void)
{
    const harness_setup_t setup = {.kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    CHECK(harness_refresh() == XIAOTAI_ERR_PLATFORM_OFFLINE);
    CHECK(harness_status().loading);
    harness_online();
    CHECK(fake_platform_request_count() == 0);
    harness_profile(false);
    CHECK(fake_platform_request_count() == 0);
    harness_profile(true);
    CHECK(fake_platform_request_count() == 1);
    const fake_request_t *req = fake_platform_request(0);
    CHECK(req->method == XIAOTAI_PLATFORM_GET && req->auth == XIAOTAI_PLATFORM_AUTH_SESSION);
    CHECK(!strcmp(req->service, "call") && !strcmp(req->path, "/v1/call/device/contacts"));
    CHECK((req->flags & XIAOTAI_PLATFORM_RELEASE) == 0 && !req->has_body);
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(3, 0));
    CHECK(harness_status().fresh && harness_status().total == 3);

    fake_platform_signal("callers_update");
    fake_platform_signal("contacts_update");
    CHECK(fake_platform_request_count() == 2 && !harness_status().fresh);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    CHECK(fake_platform_request_count() == 3 && harness_status().total == 3);
    fake_platform_answer(2, XIAOTAI_OK, 200, harness_numbered_reply(5, 0));
    CHECK(harness_status().fresh && harness_status().total == 5);
    CHECK(fake_platform_request_count() == 4 && strstr(fake_platform_request(3)->path, "/pending"));
    fake_platform_answer(3, XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"pending\":[]}}");

    CHECK(harness_refresh() == XIAOTAI_OK);
    harness_offline();
    harness_rebind();
    CHECK(harness_status().cached == 0 && harness_status().generation == 0);
    fake_platform_answer(4, XIAOTAI_OK, 200, harness_numbered_reply(2, 0));
    CHECK(harness_stats().stale_replies == 1 && harness_stats().cleared == 1);
    CHECK(harness_status().cached == 0);
    harness_stop();
}

static void test_source_and_first_fetch(void)
{
    const xiaotai_contacts_config_t config = {
        .source = &TEST_SOURCE_VOIP,
        .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
    };
    const harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    CHECK(fake_platform_service_count() == 1 && strcmp(fake_platform_service(0), "voip") == 0);
    harness_online();
    harness_profile(false);
    CHECK(fake_platform_request_count() == 1);
    const fake_request_t *req = fake_platform_request(0);
    CHECK(strcmp(req->service, "voip") == 0 && strcmp(req->path, TEST_SOURCE_VOIP.path) == 0);
    fake_platform_answer(0, XIAOTAI_OK, 200,
                         "{\"code\":0,\"data\":{\"contacts\":[{\"wx_open_id\":\"fixture\",\"wx_app_id\":\"app\"}]}}");
    CHECK(harness_status().fresh && harness_status().total == 1);
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(1, XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"contacts\":[]}}");
    CHECK(!harness_status().fresh && harness_status().total == 1 && harness_stats().fetch_failures == 1);
    harness_stop();
}

static void expect_voip_cached_person(bool fresh, bool loading)
{
    xiaotai_contact_t contact;
    const xiaotai_contacts_status_t status = harness_status();
    CHECK(status.total == 1 && status.cached == 1 && status.fresh == fresh && status.loading == loading);
    CHECK(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK);
    CHECK(strcmp(contact.id, "person") == 0);
    if (!fresh) CHECK(xiaotai_contacts_at(0, 0, &contact) == XIAOTAI_ERR_INVALID_STATE);
}

static void test_voip_response_lifetime(void)
{
    static const char empty[] = "{\"code\":0,\"data\":{\"contacts\":[]}}";
    static const char person[] = "{\"code\":0,\"data\":{\"contacts\":["
                                 "{\"wx_open_id\":\"person\",\"wx_app_id\":\"app\"}]}}";
    const struct { xiaotai_err_t result; int http; const char *body; } failures[] = {
        {XIAOTAI_OK, 201, empty}, {XIAOTAI_OK, 503, empty}, {XIAOTAI_FAIL, 0, NULL},
        {XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":0.5,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":0,\"code\":200,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":0,\"data\":{\"contacts\":[]}} trailing"},
        {XIAOTAI_OK, 200, "{\"code\":0,\"data\":{\"contacts\":{}}}"},
    };
    const xiaotai_contacts_config_t config = {.source = &TEST_SOURCE_VOIP, .response_bytes = 4096};
    const harness_setup_t setup = {.config = &config, .kinds = &KINDS[1], .kind_count = 1};
    const int64_t baseline = xiaotai_mem_host_outstanding();
    harness_start(&setup);
    const int64_t resident = xiaotai_mem_host_outstanding();
    harness_online();
    harness_profile(true);
    CHECK(fake_platform_request_count() == 1);
    const fake_request_t *req = fake_platform_request(0);
    CHECK(req->method == XIAOTAI_PLATFORM_GET && req->auth == XIAOTAI_PLATFORM_AUTH_SESSION && !req->has_body);
    CHECK(strcmp(req->service, "voip") == 0 && strcmp(req->path, "/v1/voip/device/contacts") == 0);
    fake_platform_answer(0, XIAOTAI_OK, 200, person);
    expect_voip_cached_person(true, false);

    const size_t count = sizeof(failures) / sizeof(failures[0]);
    for (size_t i = 0; i < count; ++i) {
        CHECK(harness_refresh() == XIAOTAI_OK);
        CHECK(fake_platform_request_count() == i + 2);
        fake_platform_answer(i + 1, failures[i].result, failures[i].http, failures[i].body);
        expect_voip_cached_person(false, false);
        CHECK(harness_stats().fetch_failures == i + 1);
    }

    CHECK(harness_refresh() == XIAOTAI_OK);
    const size_t pending = fake_platform_request_count();
    fake_platform_signal("callers_update");
    CHECK(fake_platform_request_count() == pending);
    expect_voip_cached_person(false, true);
    fake_platform_answer(pending - 1, XIAOTAI_OK, 200, empty);
    CHECK(fake_platform_request_count() == pending + 1);
    expect_voip_cached_person(false, true);
    fake_platform_answer(pending, XIAOTAI_OK, 503, empty);
    expect_voip_cached_person(false, false);

    CHECK(harness_refresh() == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == pending + 2);
    expect_voip_cached_person(false, true);
    const xiaotai_contacts_status_t previous = harness_status();
    fake_platform_answer(pending + 1, XIAOTAI_OK, 200,
                         "{\"code\":0,\"data\":{\"contacts\":["
                         "{\"wx_open_id\":\"still-authorized\",\"wx_app_id\":\"app\"}]}}");
    const xiaotai_contacts_status_t updated = harness_status();
    CHECK(updated.fresh && !updated.loading && updated.total == 1 && updated.cached == 1);
    CHECK(updated.generation == previous.generation && updated.revision > previous.revision);
    xiaotai_contact_t accepted;
    CHECK(xiaotai_contacts_at(0, 0, &accepted) == XIAOTAI_OK);
    CHECK(strcmp(accepted.id, "still-authorized") == 0 && strcmp(accepted.extra[0], "app") == 0);

    CHECK(harness_refresh() == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == pending + 3);
    fake_platform_answer(pending + 2, XIAOTAI_OK, 200, empty);
    const xiaotai_contacts_status_t status = harness_status();
    CHECK(!status.total && !status.cached && status.fresh && !status.loading);
    CHECK(xiaotai_mem_host_outstanding() == resident);
    harness_stop();
    CHECK(xiaotai_mem_host_outstanding() == baseline);
}

static void collect_line(void *ctx, const char *line)
{
    char *out = ctx;
    CHECK(strlen(out) + strlen(line) + 2 < 2048);
    strcat(out, line);
    strcat(out, "\n");
}

static void expect_original_contacts(void)
{
    xiaotai_contact_t row;
    CHECK(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &row) == XIAOTAI_OK);
    CHECK(!strcmp(row.id, "device-b") && !strcmp(row.name, "Original"));
    CHECK(xiaotai_contacts_first(harness_kind(0), XIAOTAI_CONTACTS_STALE_OK, &row) == XIAOTAI_OK);
    CHECK(!strcmp(row.id, "device-b"));
    CHECK(xiaotai_contacts_first(harness_kind(1), XIAOTAI_CONTACTS_STALE_OK, &row) == XIAOTAI_OK);
    CHECK(!strcmp(row.id, "openid") && !strcmp(row.extra[0], "app"));
    CHECK(xiaotai_contacts_find("openid", harness_kind(1), XIAOTAI_CONTACTS_STALE_OK, &row) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_find("other", harness_kind(0), XIAOTAI_CONTACTS_STALE_OK, &row) == XIAOTAI_ERR_NOT_FOUND);
}

static void test_deferred_refresh_preserves_cache(void)
{
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
        .reject_invalid_known_rows = true, .capacity = 2, .page_size = 2};
    const harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply(
        "{\"type\":\"device\",\"device_id\":\"device-b\",\"name\":\"Original\"},"
        "{\"type\":\"voip\",\"device_id\":\"openid\",\"wx_app_id\":\"app\"}"));
    CHECK(harness_status().fresh);
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(1, XIAOTAI_ERR_TIMEOUT, 0, NULL);
    harness_drain();
    const xiaotai_contacts_status_t before = harness_status();
    const xiaotai_contacts_stats_t stats = harness_stats();
    const size_t requests = fake_platform_request_count();
    const int events = harness_changed_events();
    CHECK(before.total == 2 && before.cached == 2 && before.error == XIAOTAI_ERR_TIMEOUT && !before.loading);
    expect_original_contacts();

    /* The Xmini migration accepts deferral instead of a new NO_MEM error. */
    fake_platform_refuse_next(XIAOTAI_ERR_NO_MEM);
    CHECK(harness_refresh() == XIAOTAI_OK && fake_platform_request_count() == requests);
    const xiaotai_contacts_status_t waiting = harness_status();
    const xiaotai_contacts_stats_t deferred = harness_stats();
    CHECK(waiting.loading && !waiting.fresh && waiting.error == before.error);
    CHECK(waiting.generation == before.generation && waiting.total == before.total && waiting.cached == before.cached);
    CHECK(waiting.kind == before.kind && waiting.page == before.page && waiting.pages == before.pages &&
          waiting.page_count == before.page_count);
    CHECK(!memcmp(waiting.totals, before.totals, sizeof(waiting.totals)));
    CHECK(deferred.fetches == stats.fetches && deferred.fetches_deferred == stats.fetches_deferred + 1U &&
          deferred.fetch_failures == stats.fetch_failures && deferred.replies_malformed == stats.replies_malformed);
    CHECK(harness_changed_events() == events + 1 && harness_last_changed().revision == waiting.revision);
    expect_original_contacts();
    xiaotai_contact_t row;
    CHECK(xiaotai_contacts_find("openid", harness_kind(1), 0, &row) == XIAOTAI_ERR_INVALID_STATE);

    harness_emit(XIAOTAI_EVENT_PLATFORM_IDLE, 0, NULL);
    CHECK(fake_platform_request_count() == requests + 1U && harness_stats().fetches == stats.fetches + 1U);
    const fake_request_t *retry = fake_platform_request(requests);
    CHECK(retry->generation == before.generation && retry->method == XIAOTAI_PLATFORM_GET);
    CHECK(!strcmp(retry->service, "call") && !strcmp(retry->path, "/v1/call/device/contacts"));
    harness_emit(XIAOTAI_EVENT_PLATFORM_IDLE, 0, NULL);
    CHECK(fake_platform_request_count() == requests + 1U && harness_changed_events() == events + 1);
    CHECK(harness_status().loading && !harness_status().fresh && harness_status().error == before.error);
    expect_original_contacts();

    fake_platform_answer(requests, XIAOTAI_OK, 200, harness_call_reply(""));
    harness_drain();
    const xiaotai_contacts_status_t recovered = harness_status();
    CHECK(recovered.fresh && !recovered.loading && recovered.error == XIAOTAI_OK);
    CHECK(recovered.generation == before.generation && recovered.total == 0 && recovered.cached == 0);
    CHECK(xiaotai_contacts_find("openid", harness_kind(1), 0, &row) == XIAOTAI_ERR_NOT_FOUND);
    harness_emit(XIAOTAI_EVENT_PLATFORM_IDLE, 0, NULL);
    CHECK(fake_platform_request_count() == requests + 1U && harness_stats().fetch_failures == stats.fetch_failures);
    puts("contacts: NO_MEM deferral kept last good cache and retried once on platform idle");
    harness_stop();
}

static void test_known_bad_row_policy(bool atomic)
{
    static const char malformed[] = "{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"other\"},{\"type\":\"voip\",\"device_id\":\"missing-app\"}]}}";
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
        .reject_invalid_known_rows = atomic, .capacity = 2, .page_size = 1};
    const harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply(
        "{\"type\":\"device\",\"device_id\":\"device-b\",\"name\":\"Original\"},"
        "{\"type\":\"voip\",\"device_id\":\"openid\",\"wx_app_id\":\"app\"}"));
    CHECK(harness_show(harness_kind(0), 0) == XIAOTAI_OK); /* The malformed voip row is outside this view. */
    const xiaotai_contacts_status_t before = harness_status();
    CHECK(before.fresh && before.total == 2 && before.cached == 2);
    const xiaotai_contacts_stats_t stats = harness_stats();

    CHECK(harness_refresh() == XIAOTAI_OK);
    harness_drain();
    const int notices = harness_changed_events();
    fake_platform_answer(1, XIAOTAI_OK, 200, malformed);
    harness_drain();
    const xiaotai_contacts_status_t after = harness_status();
    CHECK(fake_platform_request_count() == 2 && !after.loading && harness_changed_events() == notices + 1);
    if (!atomic) {
        xiaotai_contact_t current;
        CHECK(after.fresh && after.error == XIAOTAI_OK && after.total == 1 && after.cached == 1);
        CHECK(harness_stats().entries_skipped == stats.entries_skipped + 1);
        CHECK(xiaotai_contacts_at(0, 0, &current) == XIAOTAI_OK && !strcmp(current.id, "other"));
        harness_stop();
        return;
    }
    if (after.error != XIAOTAI_ERR_INVALID_RESPONSE)
        fprintf(stderr, "mixed known row result: error=%d total=%u cached=%u fresh=%u\n",
                after.error, (unsigned)after.total, (unsigned)after.cached, (unsigned)after.fresh);
    CHECK(after.error == XIAOTAI_ERR_INVALID_RESPONSE && !after.fresh);
    CHECK(after.generation == before.generation && after.total == before.total && after.cached == before.cached);
    CHECK(after.kind == before.kind && after.page == before.page && after.pages == before.pages);
    CHECK(!memcmp(after.totals, before.totals, sizeof(before.totals)));
    CHECK(harness_stats().fetch_failures == stats.fetch_failures + 1U &&
        harness_stats().replies_malformed == stats.replies_malformed + 1U &&
        harness_stats().entries_skipped == stats.entries_skipped && harness_stats().entries_dropped == stats.entries_dropped);
    CHECK(harness_last_changed().error == XIAOTAI_ERR_INVALID_RESPONSE);
    expect_original_contacts();

    char text[2048] = {0};
    CHECK(xiaotai_command_run("AT+CONTACTS", &(const xiaotai_reply_t){.ctx = text, .write = collect_line},
                              1000) == XIAOTAI_OK);
    CHECK(strstr(text, "+CONTACTS: cached=2 total=2 filter=dev page=0 loading=0 error=264"));
    CHECK(strstr(text, "id=device-b") && !strstr(text, "id=other"));

    CHECK(harness_query(harness_kind(0), 1000) == XIAOTAI_OK);
    fake_platform_answer(2, XIAOTAI_OK, 200, malformed);
    const harness_answer_t *answer = harness_answer();
    CHECK(answer->calls == 1 && answer->result == XIAOTAI_ERR_INVALID_RESPONSE && !answer->count && !answer->truncated);
    CHECK(answer->response_bytes == sizeof(malformed) - 1U && fake_platform_request_count() == 3);
    CHECK(harness_stats().fetch_failures == stats.fetch_failures + 1U &&
        harness_stats().replies_malformed == stats.replies_malformed + 2U);
    expect_original_contacts();

    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 0) == XIAOTAI_OK);
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(3, XIAOTAI_OK, 200, harness_call_reply(
        "{\"type\":\"device\",\"device_id\":\"new\"},{\"type\":\"device\",\"device_id\":\"more\"},"
        "{\"type\":\"device\",\"device_id\":\"last-device\"},null,{\"type\":5},"
        "{\"type\":\"future\"},{\"untyped\":true},"
        "{\"type\":\"voip\",\"device_id\":\"new-wx\",\"wx_app_id\":\"app\",\"wx_model_id\":"
        "\"exceeds-64-bytes-012345678901234567890123456789012345678901234567890123456789\"}"));
    CHECK(harness_status().fresh && harness_status().error == XIAOTAI_OK && harness_status().cached == 2 &&
        harness_status().total == 4 && harness_stats().fetch_failures == stats.fetch_failures + 1U);
    xiaotai_contact_t current;
    CHECK(xiaotai_contacts_first(harness_kind(0), 0, &current) == XIAOTAI_OK && !strcmp(current.id, "new"));
    CHECK(xiaotai_contacts_first(harness_kind(1), 0, &current) == XIAOTAI_OK && !strcmp(current.id, "new-wx"));
    CHECK(!current.extra[1][0]); /* Old Xmini did not read optional wx_model_id. */
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(4, XIAOTAI_OK, 200, harness_call_reply("null,{}, {\"type\":\"future\"}"));
    CHECK(harness_status().fresh && harness_status().error == XIAOTAI_OK && !harness_status().total);
    harness_stop();
}

int main(void)
{
    test_refresh();
    test_source_and_first_fetch();
    test_voip_response_lifetime();
    test_deferred_refresh_preserves_cache();
    test_known_bad_row_policy(false);
    test_known_bad_row_policy(true);
    return 0;
}
