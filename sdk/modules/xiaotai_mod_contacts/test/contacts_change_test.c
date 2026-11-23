// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_routes.c::platform_routes
// Replaces legacy: tests/xmini/xmini_business/test_business.c::contact_mutation_response
// Replaces legacy: tests/xmini/xmini_business/test_business.c::pending_response
// Replaces legacy: tests/xmini/xmini_business/test_business.c::pending_response
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"

static const char base[] = "/v1/call/device/contacts";
static const char good[] = "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"a\"},"
    "{\"type\":\"voip\"},{\"type\":\"device\",\"peer_device_id\":\"b\"},{\"type\":\"device\",\"peer_device_id\":\"c\"}]}}";
static xiaotai_err_t result;
static unsigned done_count;
static xiaotai_contacts_change_result_t last_done;
static xiaotai_err_t submit(xiaotai_contacts_change_kind_t kind, const char *id, const char *remark);

static void observe_change(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    CHECK(event->size == sizeof(last_done) && event->data);
    memcpy(&last_done, event->data, sizeof(last_done));
    CHECK(event->args.value == last_done.result);
    ++done_count;
}

static void good_remark_boundary(void)
{
    char remark[261] = {0};
    for (unsigned i = 0; i < 65; ++i) memcpy(remark + 4U * i, "\xf0\x9f\x98\x80", 4);
    const size_t at = fake_platform_request_count();
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", remark) == XIAOTAI_ERR_INVALID_SIZE);
    remark[256] = 'a';
    remark[257] = 0;
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", remark) == XIAOTAI_ERR_INVALID_SIZE);
    remark[256] = 0;
    strcpy(remark + 252, "\xf0\x9f\x98");
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", remark) == XIAOTAI_ERR_INVALID_ARG);
    memcpy(remark + 252, "\xf0\x9f\x98\x80", 4);
    char ascii[66];
    memset(ascii, 'a', 65);
    ascii[65] = 0;
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", ascii) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(fake_platform_request_count() == at);
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", remark) == XIAOTAI_OK);
    CHECK(strstr(fake_platform_request(at)->body, remark));
    fake_platform_answer(at, XIAOTAI_OK, 200, "{\"code\":40211}");
}

static void show_job(void *ctx) { result = xiaotai_contacts_pending_show((uint16_t)(uintptr_t)ctx); }
static xiaotai_err_t show(uint16_t page)
{
    CHECK(xiaotai_run(show_job, (void *)(uintptr_t)page) == XIAOTAI_OK);
    harness_drain();
    return result;
}

static void change_job(void *ctx) { result = xiaotai_contacts_change(ctx); }
static xiaotai_err_t submit(xiaotai_contacts_change_kind_t kind, const char *id, const char *remark)
{
    xiaotai_contacts_change_t args = {.kind = kind, .id = id, .remark = remark};
    CHECK(xiaotai_run(change_job, &args) == XIAOTAI_OK);
    harness_drain();
    return result;
}

static xiaotai_contacts_pending_status_t pending(void)
{
    xiaotai_contacts_pending_status_t s;
    xiaotai_contacts_pending_status(&s);
    return s;
}

int main(void)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
    const xiaotai_contacts_config_t config = {.first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
        .fetch = XIAOTAI_CONTACTS_FETCH_IDLE_ONLY, .pending_page_size = 2};
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2, .change_done = observe_change};
    harness_start(&setup);
    CHECK(submit(XIAOTAI_CONTACTS_REMOVE, "d00", NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(2, 0));
    CHECK(submit(XIAOTAI_CONTACTS_ACCEPT, "a", NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(submit(XIAOTAI_CONTACTS_REQUEST, "", NULL) == XIAOTAI_ERR_INVALID_ARG);

    harness_activity_begin();
    CHECK(show(UINT16_MAX) == XIAOTAI_OK && pending().loading && fake_platform_request_count() == 1);
    harness_activity_end();
    CHECK(fake_platform_request_count() == 2 && fake_platform_request(1)->flags == XIAOTAI_PLATFORM_BACKGROUND);
    const fake_request_t *wire = fake_platform_request(1);
    CHECK(wire->method == XIAOTAI_PLATFORM_GET && !wire->has_body && !strcmp(wire->service, "call"));
    CHECK(!strcmp(wire->path, "/v1/call/device/contacts/pending"));
    CHECK(show(0) == XIAOTAI_OK); /* The old page may not publish. */
    fake_platform_answer(1, XIAOTAI_OK, 200, good);
    CHECK(pending().loading && !pending().fresh && fake_platform_request_count() == 3);
    fake_platform_answer(2, XIAOTAI_OK, 200, good);
    char ids[2][XIAOTAI_CONTACT_ID_MAX + 1U];
    CHECK(pending().fresh && pending().total == 3 && pending().pages == 2 && pending().page == 0);
    CHECK(xiaotai_contacts_pending_page(ids, 2) == 2 && !strcmp(ids[0], "a") && !strcmp(ids[1], "b"));
    CHECK(show(UINT16_MAX) == XIAOTAI_OK);
    fake_platform_answer(3, XIAOTAI_OK, 200, good);
    CHECK(pending().page == 1 && xiaotai_contacts_pending_page(ids, 2) == 1 && !strcmp(ids[0], "c"));
    CHECK(submit(XIAOTAI_CONTACTS_REJECT, "a", NULL) == XIAOTAI_ERR_NOT_FOUND);

    const struct {
        xiaotai_contacts_change_kind_t kind;
        const char *suffix, *body;
        xiaotai_platform_method_t http;
        const char *success;
    } ops[] = {
        {XIAOTAI_CONTACTS_REQUEST, "/request", "{\"target_device_id\":\"c\"}", XIAOTAI_PLATFORM_POST,
            "{\"code\":200,\"data\":{\"status\":\"pending\",\"source\":\"manual\"}}"},
        {XIAOTAI_CONTACTS_ACCEPT, "/respond", "{\"peer_device_id\":\"c\",\"action\":\"accept\"}", XIAOTAI_PLATFORM_POST,
            "{\"code\":200,\"data\":{\"status\":\"accepted\"}}"},
        {XIAOTAI_CONTACTS_REJECT, "/respond", "{\"peer_device_id\":\"c\",\"action\":\"reject\"}", XIAOTAI_PLATFORM_POST,
            "{\"code\":200,\"data\":{\"status\":\"rejected\"}}"},
        {XIAOTAI_CONTACTS_REMARK, "/remark", "{\"peer_id\":\"c\",\"remark\":\"a\\\"b\"}", XIAOTAI_PLATFORM_PUT,
            "{\"code\":200}"},
        {XIAOTAI_CONTACTS_REMOVE, "", "{\"peer_id\":\"c\"}", XIAOTAI_PLATFORM_DELETE, "{\"code\":200}"},
    };
    const struct { xiaotai_err_t result; int http; const char *body; int32_t code; } failures[] = {
        {XIAOTAI_OK, 201, "{\"code\":200}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 204, "{\"code\":200}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_FAIL, 503, "{\"code\":200}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_ERR_TIMEOUT, 0, NULL, XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{\"code\":0}", 0},
        {XIAOTAI_OK, 200, "{\"code\":200.5}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{\"code\":200,\"code\":40205}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{\"code\":200} trailing", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{\"code\":\"200\"}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{}", XIAOTAI_PLATFORM_NO_CODE},
        {XIAOTAI_OK, 200, "{\"code\":40209}", 40209}, {XIAOTAI_OK, 200, "{\"code\":40211}", 40211},
    };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        const xiaotai_contacts_change_kind_t kind = ops[i].kind;
        const bool response = kind == XIAOTAI_CONTACTS_ACCEPT || kind == XIAOTAI_CONTACTS_REJECT;
        for (unsigned f = 0; f < sizeof(failures) / sizeof(failures[0]); ++f) {
            const size_t at = fake_platform_request_count();
            const uint32_t failed = harness_stats().change_failures;
            const unsigned before = done_count;
            CHECK(submit(kind, "c", "a\"b") == XIAOTAI_OK);
            const fake_request_t *r = fake_platform_request(at);
            CHECK(r && r->method == ops[i].http && r->auth == XIAOTAI_PLATFORM_AUTH_SESSION && !r->flags);
            CHECK(!strcmp(r->service, "call"));
            CHECK(!strncmp(r->path, base, sizeof(base) - 1U) && !strcmp(r->path + sizeof(base) - 1U, ops[i].suffix));
            CHECK(!strcmp(r->body, ops[i].body));
            CHECK(submit(XIAOTAI_CONTACTS_REQUEST, "b", NULL) == XIAOTAI_ERR_BUSY);
            fake_platform_answer(at, failures[f].result, failures[f].http, failures[f].body);
            harness_drain();
            CHECK(done_count == before + 1 && last_done.kind == kind && !strcmp(last_done.id, "c"));
            CHECK(last_done.result == (failures[f].result == XIAOTAI_OK ? XIAOTAI_FAIL : failures[f].result));
            CHECK(last_done.code == failures[f].code);
            CHECK(fake_platform_request_count() == at + 1 && harness_status().fresh && harness_status().total == 2);
            CHECK(harness_stats().change_failures == failed + 1);
        }
        const size_t at = fake_platform_request_count();
        const unsigned before = done_count;
        CHECK(submit(kind, "c", "a\"b") == XIAOTAI_OK);
        fake_platform_answer(at, XIAOTAI_OK, 200, ops[i].success);
        harness_drain();
        CHECK(done_count == before + 1 && last_done.kind == kind && !strcmp(last_done.id, "c"));
        CHECK(last_done.result == XIAOTAI_OK && last_done.code == 200);
        CHECK(fake_platform_request_count() == at + 2 && !harness_status().fresh && harness_status().total == 2);
        fake_platform_answer(at + 1, XIAOTAI_OK, 200, harness_numbered_reply(2, 0));
        if (response) {
            CHECK(strstr(fake_platform_request(at + 2)->path, "/pending") && pending().loading);
            fake_platform_answer(at + 2, XIAOTAI_OK, 200, good);
        }
    }
    for (unsigned f = 0; f < sizeof(failures) / sizeof(failures[0]); ++f) {
        const size_t at = fake_platform_request_count();
        const uint32_t failed = harness_stats().pending_failures;
        CHECK(show(0) == XIAOTAI_OK);
        CHECK(pending().loading && pending().error == XIAOTAI_OK && !pending().code);
        fake_platform_answer(at, failures[f].result, failures[f].http, failures[f].body);
        const xiaotai_contacts_pending_status_t s = pending();
        const int32_t code = failures[f].code > 0 ? failures[f].code : 0;
        const xiaotai_err_t error = failures[f].result != XIAOTAI_OK ? failures[f].result :
                                    code ? XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_RESPONSE;
        CHECK(!s.loading && !s.fresh && !s.page_count && !s.total);
        CHECK(s.error == error && s.code == code);
        CHECK(fake_platform_request_count() == at + 1 && harness_stats().pending_failures == failed + 1U);
    }
    good_remark_boundary();
    const size_t at = fake_platform_request_count();
    CHECK(show(0) == XIAOTAI_OK);
    fake_platform_answer(at, XIAOTAI_OK, 200, good);
    CHECK(pending().fresh && pending().page_count == 2 && pending().error == XIAOTAI_OK && !pending().code);
    CHECK(submit(XIAOTAI_CONTACTS_REMOVE, "d00", NULL) == XIAOTAI_OK);
    harness_offline();
    harness_rebind();
    const unsigned before = done_count;
    fake_platform_answer(at + 1, XIAOTAI_OK, 200, "{\"code\":200}");
    harness_drain();
    CHECK(done_count == before + 1 && last_done.kind == XIAOTAI_CONTACTS_REMOVE);
    CHECK(last_done.result == XIAOTAI_ERR_PLATFORM_STALE && last_done.code == XIAOTAI_PLATFORM_NO_CODE && !last_done.id[0]);
    CHECK(fake_platform_request_count() == at + 2 && harness_stats().stale_replies == 1);
    CHECK(!pending().fresh && !xiaotai_contacts_pending_page(ids, 2));
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", "\xed\xa0\x80") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", "\xf4\x90\x80\x80") == XIAOTAI_ERR_INVALID_ARG);
    harness_stop();

    const xiaotai_contacts_config_t voip = {.source = &TEST_SOURCE_VOIP};
    const harness_setup_t voice_setup = {.config = &voip, .kinds = kinds, .kind_count = 2};
    harness_start(&voice_setup);
    CHECK(show(0) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(submit(XIAOTAI_CONTACTS_REMARK, "a", NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(fake_platform_request_count() == 0);
    harness_stop();
    return 0;
}
