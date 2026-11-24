// SPDX-License-Identifier: MIT
/* mod_contacts_view: the view of the list. By kind and page; a page beyond
 * the end converges on the last one; totals by kind number; a list that fits
 * the cache serves every kind and page from memory; a list longer than the
 * cache fetches again for a page outside the cached range (loading, page
 * count 0 until the reply), and the reply's range holds that page; the page
 * is copied in the server's order, at() addresses it; the default view is
 * every kind, page 0; status events carry the revision. Also the memory the
 * module takes at init (XIAOTAI_MEM_BULK, three blocks, all returned). */
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"
#include "xiaotai_mem_host.h"

static const xiaotai_contact_kind_def_t *const KINDS[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};

static size_t page_ids(char ids[][8], size_t capacity)
{
    xiaotai_contact_t page[32];
    size_t count = xiaotai_contacts_page(page, capacity < 32 ? capacity : 32);
    for (size_t i = 0; i < count; ++i) {
        size_t length = strlen(page[i].id);
        CHECK(length < 8);
        memcpy(ids[i], page[i].id, length + 1U);
    }
    return count;
}

static void expect_page(const char *const *ids, size_t count)
{
    char got[32][8];
    size_t n = page_ids(got, 32);
    if (n != count) fprintf(stderr, "page has %zu entries, expected %zu\n", n, count);
    CHECK(n == count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(got[i], ids[i]) != 0) fprintf(stderr, "entry %zu: %s, expected %s\n", i, got[i], ids[i]);
        CHECK(strcmp(got[i], ids[i]) == 0);
    }
}

static void post_show(const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    snprintf(args.text, sizeof(args.text), "%s", text);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_CONTACTS_SHOW, &args) == XIAOTAI_OK);
    harness_drain();
}

/* The whole list in memory: views of every kind without a request. */
static void test_small_list(void)
{
    xiaotai_contacts_config_t config = {.capacity = 64, .page_size = 4, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
                                         .at_device_dial_prefix = 8};
    harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    int device = harness_kind(0), wechat = harness_kind(1);
    CHECK(device == 0 && wechat == 1);

    xiaotai_contacts_status_t s = harness_status();
    CHECK(s.kind == XIAOTAI_CONTACT_ANY && s.page == 0 && s.pages == 1 && s.page_size == 4);
    CHECK(s.page_count == 0 && !s.fresh && s.generation == 0);

    harness_online();
    CHECK(fake_platform_request_count() == 1);
    /* 10 entries, every third a voip one: d00 d01 w02 d03 d04 w05 d06 d07 w08 d09. */
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_numbered_reply(10, 3));
    s = harness_status();
    CHECK(s.fresh && !s.loading && s.total == 10 && s.totals[device] == 7 && s.totals[wechat] == 3);
    CHECK(s.cached == 10 && !s.truncated && s.pages == 3 && s.page_count == 4);
    CHECK(harness_last_changed().revision == s.revision);
    expect_page((const char *[]){"d00", "d01", "w02", "d03"}, 4);

    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 2) == XIAOTAI_OK);
    expect_page((const char *[]){"w08", "d09"}, 2);
    CHECK(harness_status().page_count == 2);
    CHECK(harness_show(wechat, 0) == XIAOTAI_OK);
    expect_page((const char *[]){"w02", "w05", "w08"}, 3);
    s = harness_status();
    CHECK(s.kind == wechat && s.pages == 1 && s.page_count == 3 && !s.truncated);
    /* Devices: d00 d01 d03 d04 | d06 d07 d09. */
    CHECK(harness_show(device, 1) == XIAOTAI_OK);
    expect_page((const char *[]){"d06", "d07", "d09"}, 3);
    /* Beyond the end: the last page. */
    CHECK(harness_show(device, 40) == XIAOTAI_OK);
    s = harness_status();
    CHECK(s.page == 1 && s.pages == 2);
    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 0xFFFF) == XIAOTAI_OK);
    CHECK(harness_status().page == 2);
    CHECK(fake_platform_request_count() == 1);
    CHECK(harness_status().fresh);

    /* at() addresses the selected page. */
    CHECK(harness_show(wechat, 0) == XIAOTAI_OK);
    xiaotai_contact_t c;
    CHECK(xiaotai_contacts_at(1, 0, &c) == XIAOTAI_OK && strcmp(c.id, "w05") == 0 && c.kind == wechat);
    CHECK(strcmp(c.name, "Wx 05") == 0 && strcmp(c.extra[0], "app") == 0);
    CHECK(xiaotai_contacts_at(3, 0, &c) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(harness_show(device, 0) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_at(0, 0, &c) == XIAOTAI_OK && strcmp(c.id, "d00") == 0 && c.online_known && c.online);
    CHECK(xiaotai_contacts_at(1, 0, &c) == XIAOTAI_OK && strcmp(c.id, "d01") == 0 && c.online_known && !c.online);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d00", device, 0, &c) == XIAOTAI_OK && !strcmp(c.id, "d00"));
    CHECK(xiaotai_contacts_find_unique_online_prefix("d01", device, 0, &c) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_find_unique_online_prefix("w05", wechat, 0, &c) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d09", device, 0, &c) == XIAOTAI_ERR_NOT_FOUND);

    /* Unknown kinds are refused and change nothing. */
    uint32_t revision = harness_status().revision;
    CHECK(harness_show(2, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(harness_show(-2, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(harness_status().revision == revision && harness_status().kind == device);

    /* A smaller copy is cut at its capacity. */
    xiaotai_contact_t two[2];
    CHECK(xiaotai_contacts_page(two, 2) == 2 && strcmp(two[1].id, "d01") == 0);
    CHECK(xiaotai_contacts_page(NULL, 2) == 0);

    /* The intent: an alias (page beyond the end: the last), "all", a name in
     * another case, "" (keeps the kind), an unknown name (refused). */
    post_show("WX", 1);
    CHECK(harness_status().kind == wechat && harness_status().page == 0);
    post_show("all", 1);
    CHECK(harness_status().kind == XIAOTAI_CONTACT_ANY && harness_status().page == 1);
    post_show("Device", 0);
    CHECK(harness_status().kind == device);
    post_show("", 1);
    CHECK(harness_status().kind == device && harness_status().page == 1);
    revision = harness_status().revision;
    post_show("telephone", 0);
    CHECK(harness_status().revision == revision && harness_status().kind == device && harness_status().page == 1);
    harness_emit(XIAOTAI_EVENT_PLATFORM_IDLE, 0, NULL);
    CHECK(fake_platform_request_count() == 1);
    harness_stop();
}

/* A list longer than the cache: pages outside the range are fetched. */
static void test_long_list(void)
{
    xiaotai_contacts_config_t config = {.capacity = 8, .page_size = 4, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE,
                                         .at_device_dial_prefix = 8};
    harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    harness_start(&setup);
    int device = harness_kind(0);
    harness_online();
    const char *list = harness_numbered_reply(20, 0);
    static char body[8192];
    snprintf(body, sizeof(body), "%s", list);
    fake_platform_answer(0, XIAOTAI_OK, 200, body);
    xiaotai_contacts_status_t s = harness_status();
    CHECK(s.total == 20 && s.cached == 8 && s.truncated && s.pages == 5 && s.page_count == 4);
    expect_page((const char *[]){"d00", "d01", "d02", "d03"}, 4);
    CHECK(harness_stats().entries_dropped == 12);

    /* Page 1 is in memory (d00..d07). */
    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 1) == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == 1);
    expect_page((const char *[]){"d04", "d05", "d06", "d07"}, 4);

    /* Page 3 is not: a fetch, nothing on the page until it is answered. */
    int events = harness_changed_events();
    CHECK(harness_show(XIAOTAI_CONTACT_ANY, 3) == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == 2);
    s = harness_status();
    CHECK(s.loading && s.page_count == 0 && !s.fresh && s.page == 3);
    CHECK(harness_changed_events() > events);
    CHECK(harness_last_changed().page_count == 0 && harness_last_changed().loading);
    xiaotai_contact_t c;
    CHECK(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &c) == XIAOTAI_ERR_NOT_FOUND);
    char none[32][8];
    CHECK(page_ids(none, 32) == 0);
    fake_platform_answer(1, XIAOTAI_OK, 200, body);
    s = harness_status();
    CHECK(!s.loading && s.fresh && s.page_count == 4 && s.cached == 8);
    expect_page((const char *[]){"d12", "d13", "d14", "d15"}, 4);
    CHECK(xiaotai_contacts_at(3, 0, &c) == XIAOTAI_OK && strcmp(c.id, "d15") == 0);
    /* The first contact of a kind is known outside the range. */
    CHECK(xiaotai_contacts_first(device, 0, &c) == XIAOTAI_OK && strcmp(c.id, "d00") == 0);
    CHECK(xiaotai_contacts_find("d00", XIAOTAI_CONTACT_ANY, 0, &c) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_find("d01", XIAOTAI_CONTACT_ANY, 0, &c) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d00", device, 0, &c) == XIAOTAI_OK);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d12", device, 0, &c) == XIAOTAI_ERR_NOT_FOUND);

    CHECK(harness_show(device, 3) == XIAOTAI_OK);
    CHECK(fake_platform_request_count() == 3);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d00", device, XIAOTAI_CONTACTS_STALE_OK, &c) == XIAOTAI_OK);
    fake_platform_answer(2, XIAOTAI_OK, 200, body);
    expect_page((const char *[]){ "d12", "d13", "d14", "d15" }, 4);
    CHECK(xiaotai_contacts_find_unique_online_prefix("d00", device, 0, &c) == XIAOTAI_OK);

    /* The last page through the intent with a page beyond the end. */
    post_show("all", 99);
    CHECK(fake_platform_request_count() == 4);
    fake_platform_answer(3, XIAOTAI_OK, 200, body);
    s = harness_status();
    CHECK(s.page == 4 && s.page_count == 4);
    expect_page((const char *[]){"d16", "d17", "d18", "d19"}, 4);

    /* A changed list shortens the view: the page converges again. */
    CHECK(harness_refresh() == XIAOTAI_OK);
    fake_platform_answer(4, XIAOTAI_OK, 200, harness_numbered_reply(6, 0));
    s = harness_status();
    CHECK(s.total == 6 && s.pages == 2 && s.page == 1 && s.page_count == 2 && !s.truncated);
    expect_page((const char *[]){"d04", "d05"}, 2);
    harness_stop();
}

/* The friendship source takes three blocks (cache, first entries, pending
 * page) and gives back what it took when anything after them fails. */
static void test_memory(void)
{
    xiaotai_contacts_config_t config = {.capacity = 16, .response_bytes = 4096};
    for (uint32_t n = 1; n <= 3; ++n) {
        CHECK(xiaotai_contacts_configure(&config) == XIAOTAI_OK);
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(n);
        CHECK(xiaotai_module_contacts_init() == XIAOTAI_ERR_NO_MEM);
        CHECK(xiaotai_mem_host_allocations() == n);
        CHECK(xiaotai_mem_host_outstanding() == 0);
        xiaotai_contacts_reset();
        harness_stop();
    }
    /* A failed registration releases everything without parse scratch. */
    CHECK(xiaotai_contacts_configure(&config) == XIAOTAI_OK);
    xiaotai_mem_host_clear();
    CHECK(xiaotai_module_contacts_init() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_mem_host_allocations() == 3 && xiaotai_mem_host_outstanding() == 0);
    xiaotai_contacts_reset();
    harness_stop();

    /* A whole start and stop leaves nothing of the module behind. */
    harness_setup_t setup = {.config = &config, .kinds = KINDS, .kind_count = 2};
    xiaotai_mem_host_clear();
    harness_start(&setup);
    CHECK(xiaotai_mem_host_outstanding() > 0);
    harness_stop();
    CHECK(xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    test_small_list();
    test_long_list();
    test_memory();
    puts("mod_contacts_view: PASS");
    return 0;
}
