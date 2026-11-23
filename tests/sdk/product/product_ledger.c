// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_ai.h"
#include "product_platform.h"
#include "product_portal.h"
#include "product_test.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_transport_host.h"

#define WAIT 10000U
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ledger fixture contract failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); \
} } while (0)

static xiaotai_mem_host_ledger_entry_t entries[XIAOTAI_MEM_HOST_LEDGER_CAPACITY];
static uintptr_t ai_peer;

void product_ledger_measurement_start(void)
{
    CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
    xiaotai_mem_host_ledger_start();
}

void product_ledger_prepare(bool provisioned)
{
    if (provisioned) {
        product_platform_prepare(true);
        product_ai_prepare_retirement_backend();
    }
}

static bool have_task(size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (entries[i].task && !strcmp(entries[i].name, name)) return true;
    return false;
}

static void item(const char *phase, const char *category, size_t bytes, uintptr_t site,
                 const char *task, bool fixture, bool counted)
{
    printf("ledger-item %s %s %zu %" PRIxPTR " %s %u %u\n",
           phase, category, bytes, site, task && *task ? task : "-", (unsigned)fixture, (unsigned)counted);
}

static bool snapshot(const char *phase, bool provisioned, bool mqtt, uint32_t ai_session)
{
    product_check_owner();
    if (ai_session) {
        xiaotai_ai_talk_status_t ai;
        xiaotai_media_status_t media;
        xiaotai_ai_talk_status(&ai);
        xiaotai_media_status(&media);
        CHECK(ai.session == ai_session && ai.phase == XIAOTAI_AI_TALK_LISTENING &&
              ai.link && media.generation == ai.link && media.active);
    }
    size_t count = 0;
    CHECK(xiaotai_mem_host_ledger_snapshot(entries, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    /* A borrowed owner uses the separately accounted native main stack. */
    CHECK(have_task(count, "xiaotai") == !XIAOTAI_FRAMEWORK_OWNER_ON_MAIN);
    CHECK(have_task(count, "xiaotai_kv") && have_task(count, "xiaotai_net"));
    CHECK(have_task(count, "xiaotai_dns") == !provisioned &&
          have_task(count, "xt_transport") == provisioned &&
          have_task(count, "xiaotai_mqtt") == mqtt);
    size_t stack = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
    size_t heap[3] = {0};
    size_t fixture = 0;
    item(phase, "native-stack", CONFIG_ESP_MAIN_TASK_STACK_SIZE, 0, "main", false, true);
    const size_t portal = product_portal_stack();
    if (!provisioned) {
        CHECK(portal > 0);
        stack += portal;
        item(phase, "native-stack", portal, 0, "esp_http_server", false, true);
    }
    static const char *const kind[] = {"bulk", "internal", "dma"};
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_mem_host_ledger_entry_t *e = &entries[i];
        CHECK((unsigned)e->kind < sizeof(kind) / sizeof(kind[0]) && e->bytes);
        item(phase, e->task ? "task" : kind[e->kind], e->bytes, e->site, e->name, e->fixture, !e->fixture);
        if (e->fixture) fixture += e->bytes;
        else if (e->task) stack += e->bytes;
        else heap[e->kind] += e->bytes;
    }
    const size_t total = stack + heap[XIAOTAI_MEM_INTERNAL] + heap[XIAOTAI_MEM_DMA] + heap[XIAOTAI_MEM_BULK];
    const int64_t delta = (int64_t)total - XIAOTAI_PRODUCT_INTERNAL_BASELINE_BYTES;
    const bool under_limit = total <= XIAOTAI_PRODUCT_INTERNAL_BUDGET_BYTES;
    printf("ledger-total %s stack=%zu internal=%zu dma=%zu bulk=%zu fixture=%zu excluded=0"
           " total=%zu baseline=%u budget=%u delta=%+" PRId64 " %s\n",
           phase, stack, heap[XIAOTAI_MEM_INTERNAL], heap[XIAOTAI_MEM_DMA], heap[XIAOTAI_MEM_BULK],
           fixture, total, XIAOTAI_PRODUCT_INTERNAL_BASELINE_BYTES,
           XIAOTAI_PRODUCT_INTERNAL_BUDGET_BYTES, delta, under_limit ? "within-budget" : "over-budget");
    return under_limit;
}

static void await_profile(void)
{
    for (unsigned elapsed = 0; elapsed < WAIT; elapsed += 5) {
        xiaotai_platform_status_t status;
        xiaotai_platform_status(&status);
        if (status.profile_ready && !xiaotai_platform_exchanges_busy()) return;
        xiaotai_delay_ms(5);
    }
    CHECK(false);
}

static bool signaling_matches(bool online)
{
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    return status.held != online && status.signaling == online &&
           product_platform_has_client(PRODUCT_FIXTURE_CLIENT) == online;
}

static bool saw_ai_pause(uint32_t *after, uint32_t session)
{
    xiaotai_eventlog_entry_t entries[8];
    size_t n;
    do {
        n = xiaotai_eventlog_read(*after, entries, sizeof(entries) / sizeof(entries[0]));
        for (size_t i = 0; i < n; ++i) {
            *after = entries[i].sequence;
            if (entries[i].generation == session && !strcmp(entries[i].source, "ai_talk") &&
                !strcmp(entries[i].label, "signaling.paused")) return true;
        }
    } while (n == sizeof(entries) / sizeof(entries[0]));
    return false;
}

static void await_signaling(bool online, uint32_t session, uint32_t *after)
{
    bool paused = false;
    for (unsigned elapsed = 0; elapsed < WAIT; elapsed += 5) {
        if (after && !paused) paused = saw_ai_pause(after, session);
        CHECK(!online || !paused);
        if (signaling_matches(online) && (online || paused)) return;
        xiaotai_delay_ms(5);
    }
    CHECK(false);
}

static void observe_ai_peer(uintptr_t peer) { ai_peer = peer; }

static void exercise_audio(void)
{
    CHECK(ai_peer);
    const uint32_t sent_before = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_SEND);
    xiaotai_media_stats_t stats;
    unsigned elapsed = 0;
    for (; elapsed < WAIT; elapsed += 5) {
        xiaotai_media_stats(&stats);
        if (stats.tx_sent && xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_SEND) > sent_before) break;
        xiaotai_delay_ms(5);
    }
    CHECK(elapsed < WAIT);
    const uint32_t received_before = stats.rx_received, played_before = stats.rx_played;
    const uint32_t written_before = stats.pcm_written;
    static const uint8_t alaw[160];
    xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = 1, .timestamp_ms = 1000,
        .format = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U}};
    for (unsigned i = 0; i < 4; ++i, frame.timestamp_ms += 20U)
        xiaotai_transport_host_frame(ai_peer, &frame, alaw, sizeof(alaw));
    for (elapsed = 0; elapsed < WAIT; elapsed += 5) {
        xiaotai_media_stats(&stats);
        if (stats.rx_received > received_before && stats.rx_played > played_before &&
            stats.pcm_written > written_before) break;
        xiaotai_delay_ms(5);
    }
    CHECK(elapsed < WAIT);
    printf("ledger audio: capture sent=%u downlink=%u played=%u pcm_written=%u\n",
           (unsigned)stats.tx_sent, (unsigned)(stats.rx_received - received_before),
           (unsigned)(stats.rx_played - played_before), (unsigned)(stats.pcm_written - written_before));
}

int product_ledger(bool provisioned)
{
    CHECK(!PRODUCT_HOST_EXTERNAL_RAM && xiaotai_net_provisioning_boot() == !provisioned);
    if (!provisioned) {
        CHECK(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, WAIT));
        CHECK(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
        xiaotai_net_provision_status_t status;
        xiaotai_net_provision_status(&status);
        CHECK(status.state == XIAOTAI_NET_PROVISION_OPEN &&
              status.reason == XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED &&
              status.methods & XIAOTAI_PROVISION_PORTAL);
        CHECK(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        return snapshot("provision-portal", false, false, 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    product_platform_online();
    await_profile();
    CHECK(xiaotai_transport_host_settle(WAIT));
    CHECK(signaling_matches(true));
    const bool idle = snapshot("online-idle", true, true, 0);
    product_ai_prepare();
    uint32_t signaling_cursor = xiaotai_eventlog_latest();
    const uint32_t session = product_ai_request();
    product_ai_connect_armed(session, observe_ai_peer);
    const bool signaling_active = !PRODUCT_HOST_AI_PAUSES_SIGNALING;
    await_signaling(signaling_active, session, &signaling_cursor);
    exercise_audio();
    CHECK(signaling_matches(signaling_active));
    if (signaling_active) CHECK(!saw_ai_pause(&signaling_cursor, session));
    printf("ledger AI active: session=%u signaling=%s\n", (unsigned)session,
           signaling_active ? "online" : "paused");
    const bool active = snapshot("media-active", true, signaling_active, session);
    const uint32_t link = product_ai_link(session);
    const uint32_t disconnects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    product_ai_stop(session);
    product_ai_wait_disconnect(disconnects);
    xiaotai_transport_host_closed_for(ai_peer, link);
    await_signaling(true, signaling_active ? session : 0, signaling_active ? &signaling_cursor : NULL);
    puts("ledger AI ended: signaling online");
    return idle && active ? EXIT_SUCCESS : EXIT_FAILURE;
}
