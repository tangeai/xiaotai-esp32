// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_contact_query.py
#include <string.h>
#include <stdatomic.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static _Atomic int64_t owner_clock_us;
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t now = atomic_load(&owner_clock_us);
    return now ? now : __real_xiaotai_time_us();
}

typedef struct {
    xiaotai_signal_t entered, resume, delivered;
    const fake_request_t *request;
    const char *body;
} pressure_t;

static void block_owner(void *ctx)
{
    pressure_t *pressure = ctx;
    xiaotai_signal_give(&pressure->entered);
    CHECK(xiaotai_signal_take(&pressure->resume, 5000));
}

static void noop(void *unused) { (void)unused; }

static void deliver_retained_reply(void *ctx)
{
    pressure_t *pressure = ctx;
    const xiaotai_platform_reply_t reply = {
        .result = XIAOTAI_OK, .http_status = 200, .code = 200, .body = pressure->body,
        .size = strlen(pressure->body), .generation = pressure->request->generation,
    };
    pressure->request->reply(&reply, pressure->request->ctx);
    xiaotai_signal_give(&pressure->delivered);
}

static void deadline_case(const harness_setup_t *setup, int64_t delta_us)
{
    harness_start(setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    const int64_t now = __real_xiaotai_time_us();
    atomic_store(&owner_clock_us, now);
    CHECK(harness_query(0, 0) == XIAOTAI_OK);
    pressure_t pressure = {.request = fake_platform_request(1), .body = harness_numbered_reply(5, 0)};
    CHECK(pressure.request->timeout_ms == 4000 && !pressure.request->flags);
    CHECK(xiaotai_signal_init(&pressure.entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&pressure.resume) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&pressure.delivered) == XIAOTAI_OK);
    CHECK(xiaotai_run(block_owner, &pressure) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pressure.entered, 5000));
    unsigned filled = 0;
    xiaotai_err_t queued;
    while ((queued = xiaotai_run(noop, NULL)) == XIAOTAI_OK) CHECK(++filled < 1024U);
    CHECK(filled && queued == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_run(deliver_retained_reply, &pressure) == XIAOTAI_ERR_NO_MEM);
    CHECK(!harness_answer()->calls && harness_status().total == 1);

    atomic_store(&owner_clock_us, now + INT64_C(10000000) + delta_us);
    xiaotai_signal_give(&pressure.resume);
    const int64_t stop = __real_xiaotai_time_us() + INT64_C(2000000);
    do {
        queued = xiaotai_run(deliver_retained_reply, &pressure);
        if (queued != XIAOTAI_ERR_NO_MEM) break;
        xiaotai_delay_ms(1);
    } while (__real_xiaotai_time_us() < stop);
    CHECK(queued == XIAOTAI_OK && xiaotai_signal_take(&pressure.delivered, 5000));
    harness_drain();
    const bool before = delta_us < 0;
    CHECK(harness_answer()->calls == 1 && harness_answer()->result == (before ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT));
    CHECK(harness_answer()->deadline_expired == !before);
    CHECK(harness_stats().query_timeouts == (before ? 0U : 1U));
    CHECK(harness_status().total == (before ? 5 : 1) && fake_platform_request_count() == 2);
    atomic_store(&owner_clock_us, 0);
    xiaotai_signal_deinit(&pressure.delivered);
    xiaotai_signal_deinit(&pressure.resume);
    xiaotai_signal_deinit(&pressure.entered);
    harness_stop();
}

static void failed_reply_preserves_cache(const harness_setup_t *setup)
{
    static const struct { xiaotai_err_t result; int http; const char *body; } cases[] = {
        {XIAOTAI_OK, 201, "{\"code\":200,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 503, "{\"code\":200,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":0,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":200.5,\"data\":{\"contacts\":[]}}"},
        {XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"contacts\":[]}} trailing"},
        {XIAOTAI_ERR_TIMEOUT, 0, NULL},
    };
    harness_start(setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(3, 0));
    const xiaotai_contacts_status_t seeded = harness_status();
    xiaotai_contact_t first, last;
    CHECK(xiaotai_contacts_at(0, 0, &first) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_at(2, 0, &last) == XIAOTAI_OK);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(harness_query(0, 0) == XIAOTAI_OK);
        fake_platform_answer(1U + i * 2U, cases[i].result, cases[i].http, cases[i].body);
        CHECK(harness_answer()->calls == (int)i + 1 && harness_answer()->result != XIAOTAI_OK &&
              !harness_answer()->count);
        if (cases[i].result == XIAOTAI_ERR_TIMEOUT) {
            CHECK(harness_answer()->result == XIAOTAI_ERR_TIMEOUT && !harness_answer()->deadline_expired);
        }
        CHECK(harness_refresh() == XIAOTAI_OK);
        fake_platform_answer(2U + i * 2U, cases[i].result, cases[i].http, cases[i].body);
        CHECK(harness_stats().fetch_failures == i + 1U);
        const xiaotai_contacts_status_t cached = harness_status();
        CHECK(cached.total == seeded.total && cached.cached == seeded.cached &&
              cached.generation == seeded.generation && !cached.fresh);
        xiaotai_contact_t contact;
        CHECK(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK);
        CHECK(!strcmp(contact.id, first.id) && !strcmp(contact.name, first.name));
        CHECK(xiaotai_contacts_at(2, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK);
        CHECK(!strcmp(contact.id, last.id) && !strcmp(contact.name, last.name));
    }
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(fake_platform_request_count() - 1U, XIAOTAI_OK, 200, harness_numbered_reply(0, 0));
    CHECK(!harness_status().cached && !harness_status().total && harness_status().fresh);
    harness_stop();
}

static void wait_for_answer(void)
{
    const int64_t deadline = xiaotai_time_us() + 2000000;
    do {
        harness_drain();
        if (harness_answer()->calls != 0) return;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < deadline);
    CHECK(harness_answer()->calls != 0);
}

int main(int argc, char **argv)
{
    const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
    const xiaotai_contacts_config_t config = {
        .fetch = XIAOTAI_CONTACTS_FETCH_IDLE_ONLY, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
    };
    const harness_setup_t setup = {.config = &config, .kinds = kinds, .kind_count = 2};
    if (argc > 1 && !strcmp(argv[1], "owner-deadline")) {
        for (int64_t delta = -1; delta <= 1; ++delta) deadline_case(&setup, delta);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "failed-reply-cache")) {
        failed_reply_preserves_cache(&setup);
        return 0;
    }
    harness_start(&setup);
    CHECK(harness_query(0, 0) == XIAOTAI_ERR_PLATFORM_OFFLINE && harness_answer()->calls == 0);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    harness_activity_begin();
    CHECK(harness_refresh() == XIAOTAI_OK && fake_platform_request_count() == 1);
    CHECK(harness_query(2, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(harness_query(0, 0) == XIAOTAI_OK && fake_platform_request_count() == 2);
    CHECK(harness_query(0, 0) == XIAOTAI_ERR_BUSY);
    CHECK(fake_platform_request(1)->timeout_ms == 4000 && fake_platform_request(1)->flags == 0);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(4, 2));
    CHECK(harness_answer()->calls == 1 && harness_answer()->result == XIAOTAI_OK);
    CHECK(harness_answer()->count == 2 && strcmp(harness_answer()->contacts[1].id, "d02") == 0);
    CHECK(harness_status().total == 4 && !harness_status().fresh);
    harness_activity_end();
    CHECK(fake_platform_request_count() == 3);
    fake_platform_answer(2, XIAOTAI_OK, 200, harness_numbered_reply(3, 0));
    CHECK(harness_status().total == 3 && harness_status().fresh);
    harness_stop();

    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    CHECK(harness_query(0, 25) == XIAOTAI_OK);
    wait_for_answer();
    CHECK(harness_answer()->calls == 1 && harness_answer()->result == XIAOTAI_ERR_TIMEOUT);
    CHECK(harness_stats().query_timeouts == 1);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(5, 0));
    CHECK(harness_answer()->calls == 1 && harness_status().total == 1);

    CHECK(harness_query(0, 0) == XIAOTAI_OK);
    harness_rebind();
    fake_platform_answer(2, XIAOTAI_OK, 200, harness_numbered_reply(5, 0));
    CHECK(harness_answer()->calls == 2 && harness_answer()->result == XIAOTAI_ERR_PLATFORM_STALE);
    CHECK(harness_status().total == 0);
    harness_stop();

    harness_start(&setup);
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(1, 0));
    CHECK(harness_query(0, 0) == XIAOTAI_OK);
    CHECK(harness_query_cancel(true) == XIAOTAI_ERR_INVALID_STATE && harness_answer()->calls == 0);
    CHECK(harness_query_cancel(false) == XIAOTAI_OK);
    CHECK(harness_answer()->calls == 1 && harness_answer()->result == XIAOTAI_ERR_INVALID_STATE);
    CHECK(harness_query(0, 0) == XIAOTAI_OK);
    fake_platform_answer(1, XIAOTAI_OK, 200, harness_numbered_reply(5, 0));
    CHECK(harness_answer()->calls == 1 && harness_status().total == 1);
    static const char remark[] = "A remark that is longer than one cached text entry but is present in the original reply";
    static const char reply[] = " \t{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"voip\",\"device_id\":\"ignored\",\"wx_app_id\":\"app\"},"
        "{\"type\":\"device\",\"device_id\":\"new\",\"remark\":\"A remark that is longer than one cached text entry "
        "but is present in the original reply\"}]}} \r\n";
    fake_platform_answer(2, XIAOTAI_OK, 200, reply);
    CHECK(harness_answer()->calls == 2 && harness_answer()->count == 1 && !strcmp(harness_answer()->contacts[0].id, "new"));
    CHECK(strlen(harness_answer()->contacts[0].name) == XIAOTAI_CONTACT_NAME_MAX);
    CHECK(!strcmp(harness_answer()->first_remark, remark) && harness_answer()->response_bytes == strlen(reply));
    harness_stop();
    return 0;
}
