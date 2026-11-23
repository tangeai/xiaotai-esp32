// SPDX-License-Identifier: MIT
#include <string.h>
#include "contacts_internal.h"
#include "contacts_test.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai_mem_host.h"

static const xiaotai_contact_kind_def_t *const kinds[] = {&TEST_KIND_DEVICE, &TEST_KIND_WECHAT};
static char first_line[XIAOTAI_AT_OUT_LINE_MAX + 1U];

static void record(void *unused, const char *line)
{
    (void)unused;
    if (!first_line[0]) (void)snprintf(first_line, sizeof(first_line), "%s", line);
}

static xiaotai_err_t at(const char *command)
{
    first_line[0] = '\0';
    const xiaotai_reply_t reply = {.write = record};
    const xiaotai_err_t result = xiaotai_command_run(command, &reply, 1000);
    harness_drain();
    return result;
}

static size_t bulk_blocks(size_t bytes)
{
    static xiaotai_mem_host_ledger_entry_t entries[XIAOTAI_MEM_HOST_LEDGER_CAPACITY];
    size_t count;
    CHECK(xiaotai_mem_host_ledger_snapshot(entries, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    size_t matches = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_mem_host_ledger_entry_t *entry = &entries[i];
        if (!entry->task && !entry->fixture && entry->kind == XIAOTAI_MEM_BULK && entry->bytes == bytes) ++matches;
    }
    return matches;
}

static void cold_boot(const xiaotai_contacts_config_t *config)
{
    const bool friends = !config->source;
    const harness_setup_t setup = {.config = config, .kinds = kinds, .kind_count = 2, .provisioning_boot = true};
    harness_start(&setup);

    const size_t bytes = config->response_bytes / CONTACTS_REPLY_BYTES_PER_TOKEN * sizeof(xiaotai_json_token_t);
    CHECK(g_contacts.provisioning_boot && bulk_blocks(bytes) == 0);
    CHECK(g_contacts.cache && g_contacts.firsts && !!g_contacts.pending_ids == friends);
    CHECK(g_contacts.started && xiaotai_contacts_kind_count() == 2);
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_CONTACTS_REFRESH) && xiaotai_intent_exists(XIAOTAI_INTENT_CONTACTS_SHOW));
    CHECK(at("AT+CONTACTS") == XIAOTAI_OK);
    CHECK(!strcmp(first_line, "+CONTACTS: cached=0 total=0 filter=all page=0 loading=0 error=0"));
    CHECK(at("AT+CONTACTS PAGE WX") == XIAOTAI_OK && strstr(first_line, "filter=wx page=0"));
    CHECK(at("AT+CONTACTS REFRESH") == XIAOTAI_ERR_PLATFORM_OFFLINE);
    CHECK(harness_query(XIAOTAI_CONTACT_ANY, 1000) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    CHECK(at("AT+CONTACTS PENDING 2") == (friends ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED));
    CHECK(at("AT+CONTACT REQUEST peer") == (friends ? XIAOTAI_ERR_PLATFORM_OFFLINE : XIAOTAI_ERR_NOT_SUPPORTED));
    CHECK(fake_platform_request_count() == 0 && harness_answer()->calls == 0);

    /* No production platform request can run in this boot. Deliberately
     * violate that contract to exercise the callback's boot-phase guard. */
    harness_online();
    fake_platform_answer(0, XIAOTAI_OK, 200, harness_call_reply("{\"type\":\"device\",\"device_id\":\"fake\"}"));
    CHECK(harness_status().cached == 0 && harness_status().error == XIAOTAI_ERR_PLATFORM_OFFLINE);
    if (friends) {
        CHECK(fake_platform_request_count() == 2);
        fake_platform_answer(1, XIAOTAI_OK, 200, "{\"code\":200,\"data\":{\"pending\":[]}}");
        xiaotai_contacts_pending_status_t pending;
        xiaotai_contacts_pending_status(&pending);
        CHECK(!pending.fresh && pending.error == XIAOTAI_ERR_PLATFORM_OFFLINE);
        CHECK(at("AT+CONTACT REQUEST peer") == XIAOTAI_OK);
        fake_platform_answer(2, XIAOTAI_OK, 200, "{\"code\":200}");
        CHECK(harness_stats().change_failures == 1);
    }
    const size_t index = fake_platform_request_count();
    CHECK(harness_query(XIAOTAI_CONTACT_ANY, 1000) == XIAOTAI_OK);
    fake_platform_answer(index, XIAOTAI_OK, 200, harness_call_reply(""));
    CHECK(harness_answer()->calls == 1 && harness_answer()->result == XIAOTAI_ERR_PLATFORM_OFFLINE);
    CHECK(bulk_blocks(bytes) == 0);
    printf("exclusive boot: %zu B parse workspace absent; cache, AT and intents retained\n", bytes);
    harness_stop();
}

static void station_boot(const xiaotai_contacts_config_t *config, const char *valid_response)
{
    const harness_setup_t setup = {.config = config, .kinds = kinds, .kind_count = 2};
    harness_start(&setup);
    const size_t tokens = config->response_bytes / CONTACTS_REPLY_BYTES_PER_TOKEN;
    const size_t bytes = tokens * sizeof(xiaotai_json_token_t);
    CHECK(!g_contacts.provisioning_boot && g_contacts.token_capacity == tokens && bulk_blocks(bytes) == 0);
    harness_online();
    CHECK(fake_platform_request_count() == 1);
    fake_platform_answer(0, XIAOTAI_OK, 200, valid_response);
    CHECK(harness_status().cached == 1 && harness_status().fresh);
    CHECK(bulk_blocks(bytes) == 0);
    printf("station boot: %zu-token limit retained without %zu B workspace; list applied\n", tokens, bytes);
    harness_stop();
}

int main(int argc, char **argv)
{
    xiaotai_mem_host_ledger_start();
    const xiaotai_contacts_config_t call = {.response_bytes = 8192, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE};
    const xiaotai_contacts_config_t voice = {
        .response_bytes = 4096, .first_fetch = XIAOTAI_CONTACTS_ON_ONLINE, .source = &TEST_SOURCE_VOIP};
    CHECK(argc == 1 || argc == 2);
    if (argc == 1) {
        cold_boot(&call);
    } else if (!strcmp(argv[1], "station")) {
        station_boot(&call, "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"peer\"}]}}");
    } else if (!strcmp(argv[1], "voice-provision")) {
        cold_boot(&voice);
    } else {
        CHECK(!strcmp(argv[1], "voice-station"));
        station_boot(&voice, "{\"code\":0,\"data\":{\"contacts\":[{\"wx_open_id\":\"peer\",\"wx_app_id\":\"app\"}]}}");
    }
    return 0;
}
