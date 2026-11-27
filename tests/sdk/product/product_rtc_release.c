// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

static const size_t bytes_per_connect = 28U * 1024U;
enum {WAIT_MS = 4000};

typedef struct {
    uint32_t request;
    void *payload;
} mock_buffer_t;

static pthread_mutex_t buffers_lock = PTHREAD_MUTEX_INITIALIZER;
static mock_buffer_t buffers[2];
static unsigned submitted;
static bool ample_headroom;
static xiaotai_signal_t low_memory_submitted;

static void connected_request(void *ctx, xiaotai_link_kind_t kind, uint32_t request,
                              const char *peer, const char *token)
{
    (void)ctx;
    assert(kind == XIAOTAI_LINK_WHIP && request && peer && token);
    pthread_mutex_lock(&buffers_lock);
    assert(submitted < sizeof(buffers) / sizeof(buffers[0]));
    mock_buffer_t *buffer = &buffers[submitted++];
    assert(!buffer->request && !buffer->payload);
    *buffer = (mock_buffer_t){request, malloc(bytes_per_connect)};
    assert(buffer->payload);
    memset(buffer->payload, 0xa5, bytes_per_connect);
    pthread_mutex_unlock(&buffers_lock);
    if (!ample_headroom) xiaotai_signal_give(&low_memory_submitted);
}

static void finish_buffer(uint32_t request)
{
    unsigned freed = 0;
    pthread_mutex_lock(&buffers_lock);
    for (unsigned i = 0; i < submitted; ++i) {
        if (buffers[i].payload && (!request || request == buffers[i].request)) {
            free(buffers[i].payload);
            buffers[i].payload = NULL;
            ++freed;
        }
    }
    pthread_mutex_unlock(&buffers_lock);
    assert(!request || freed == 1);
}

static size_t held_bytes(uint32_t request)
{
    size_t bytes = 0;
    pthread_mutex_lock(&buffers_lock);
    for (unsigned i = 0; i < submitted; ++i)
        if (buffers[i].payload && (!request || request == buffers[i].request)) bytes += bytes_per_connect;
    pthread_mutex_unlock(&buffers_lock);
    return bytes;
}

xiaotai_err_t __real_xiaotai_rtc_stop(int *detail);
xiaotai_err_t __wrap_xiaotai_rtc_stop(int *detail)
{
    const xiaotai_err_t err = __real_xiaotai_rtc_stop(detail);
    if (err == XIAOTAI_OK) finish_buffer(0); /* Script emits its STOPPED acknowledgment in this call. */
    return err;
}

void __real_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
#if PRODUCT_TEST_RECOGNITION_HTTP
void product_recognition_audio_report_headroom(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
#endif
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    __real_xiaotai_mem_stats(kind, stats);
#if PRODUCT_TEST_RECOGNITION_HTTP
    product_recognition_audio_report_headroom(kind, stats);
#endif
    if (!ample_headroom || kind != XIAOTAI_MEM_INTERNAL || !stats) return;
    // Isolate the admission policy from the host port's zero heap geometry.
    stats->total = 96U * 1024U;
    stats->free = stats->total - held_bytes(0);
    stats->largest_block = stats->free;
}

void product_rtc_release_prepare(bool report_headroom)
{
    ample_headroom = report_headroom;
    if (!ample_headroom) assert(xiaotai_signal_init(&low_memory_submitted) == XIAOTAI_OK);
    if (!strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-xmini-c3-v3")) {
        /* Use the real persisted mute-guidance setting; a boot prompt is
         * not part of the RTC cancellation contract exercised here. */
        const uint8_t audio[] = {2, 40, 24, 0};
        assert(xiaotai_kv_set("xmini_ui", "audio", audio, sizeof(audio), XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    }
    product_platform_prepare(true);
    xiaotai_transport_host_clock_manual(10000);
    xiaotai_transport_host_connect_hook(connected_request, NULL);
}

typedef struct {
    xiaotai_ai_talk_status_t talk;
    xiaotai_app_companion_status_t display;
    xiaotai_transport_status_t transport;
    xiaotai_media_status_t media;
} observed_t;

static void read_owner(void *ctx)
{
    observed_t *out = ctx;
    xiaotai_ai_talk_status(&out->talk);
    xiaotai_app_companion_status(&out->display);
    xiaotai_transport_status(&out->transport);
    xiaotai_media_status(&out->media);
}

static void settle(void)
{
    assert(xiaotai_transport_host_settle(WAIT_MS));
    product_on_owner(NULL, NULL);
}

static observed_t wait_screen(uint32_t session, const char *context, bool have_link)
{
    observed_t out;
    for (unsigned t = 0; t < WAIT_MS; t += 2) {
        product_on_owner(read_owner, &out);
        if (out.talk.session == session && (!have_link || out.talk.link) && out.display.foreground &&
            !strcmp(out.display.context, context)) return out;
        xiaotai_delay_ms(2);
    }
    fprintf(stderr, "expected session=%u context=%s; got session=%u link=%u context=%s page=%s end=%u\n",
            (unsigned)session, context, (unsigned)out.talk.session, (unsigned)out.talk.link,
            out.display.context, out.display.page, (unsigned)out.talk.end);
    abort();
}

static uint32_t start_link(uint32_t *session)
{
    *session = product_ai_request();
    const observed_t out = wait_screen(*session, XIAOTAI_APP_COMPANION_CONTEXT_TALK, true);
    assert(out.talk.phase == XIAOTAI_AI_TALK_CONNECTING && !out.media.active);
    settle();
    return out.talk.link;
}

static observed_t idle(void)
{
    const observed_t out = wait_screen(0, XIAOTAI_APP_COMPANION_CONTEXT_IDLE, false);
    assert(!out.talk.link && out.talk.phase == XIAOTAI_AI_TALK_IDLE && !out.media.active &&
           !strcmp(out.display.page, XIAOTAI_APP_COMPANION_PAGE_HOME));
    return out;
}

static uint32_t calls(xiaotai_transport_host_call_kind_t kind) { return xiaotai_transport_host_count(kind); }

static void no_pending_api(void)
{
    assert(!calls(XIAOTAI_TRANSPORT_HOST_CANCEL) && !calls(XIAOTAI_TRANSPORT_HOST_PENDING));
}

static void late(uint32_t request, uintptr_t conn)
{
    // Backend owns the modeled bytes until its close confirmation, not our UI stop.
    const uint32_t before = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    xiaotai_transport_host_connected(request, conn, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    assert(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == before + 1 && held_bytes(request) == bytes_per_connect);
    xiaotai_transport_host_call_t log[96];
    const size_t count = xiaotai_transport_host_calls(log, sizeof(log) / sizeof(log[0]));
    assert(count <= sizeof(log) / sizeof(log[0]));
    unsigned matched = 0;
    for (size_t i = 0; i < count; ++i)
        if (log[i].kind == XIAOTAI_TRANSPORT_HOST_DISCONNECT && log[i].connection == conn &&
            log[i].result == XIAOTAI_OK) ++matched;
    assert(matched == 1);
}

static void close_ack(uint32_t request, uintptr_t conn)
{
    finish_buffer(request);
    xiaotai_transport_host_closed(conn);
    settle();
    assert(!held_bytes(request));
}

static void reject_another(void *ctx)
{
    const uint32_t stale = *(const uint32_t *)ctx;
    uint32_t gen = UINT32_MAX;
    assert(xiaotai_ai_talk_start(NULL, &gen) == XIAOTAI_ERR_INVALID_STATE && !gen);
    assert(xiaotai_ai_talk_stop(stale) == XIAOTAI_ERR_INVALID_STATE);
}

static void release_pair(bool tolerant)
{
    const uint32_t base_engine = idle().transport.engine_bytes;
    uint32_t first, second;
    const uint32_t old = start_link(&first);
    assert(held_bytes(old) == bytes_per_connect);
    product_ai_stop(first);
    const observed_t home = idle();
    assert(home.talk.end == XIAOTAI_AI_TALK_END_LOCAL && home.transport.links_releasing == 1);
    late(old, 0x91U);
    const observed_t after_late = idle();
    assert(after_late.display.pages == home.display.pages && after_late.transport.late_links_closed == 1);
    assert(!calls(XIAOTAI_TRANSPORT_HOST_COMMAND));

    const uint32_t next = start_link(&second);
    assert(old != next && first != second);
    assert(held_bytes(old) == bytes_per_connect && held_bytes(next) == (tolerant ? bytes_per_connect : 0));
    const size_t overlap = held_bytes(0);
    product_on_owner(reject_another, &first);
    close_ack(old, 0x91U);
    const observed_t still_next = wait_screen(second, XIAOTAI_APP_COMPANION_CONTEXT_TALK, true);
    assert(still_next.talk.link == next && held_bytes(0) == bytes_per_connect);
    product_ai_stop(second);
    late(next, 0x92U);
    close_ack(next, 0x92U);
    const observed_t back = idle();
    assert(!back.transport.links_releasing && back.transport.engine_bytes == base_engine && !held_bytes(0));
    no_pending_api();
    printf("%s: late close keeps HOME; overlapping mock bytes=%zu -> %zu -> 0\n",
           XIAOTAI_PRODUCT_NAME, overlap, bytes_per_connect);
}

static void advance(uint32_t ms)
{
    xiaotai_transport_host_clock_advance(ms);
    settle();
}

static void release_timeout(bool tolerant, bool color)
{
    const uint32_t connect_ms = color ? 12000U : 20000U;
    const uint32_t release_ms = color ? 35000U : 15000U;
    uint32_t session;
    const uint32_t old = start_link(&session);
    advance(connect_ms - 1U);
    assert(wait_screen(session, XIAOTAI_APP_COMPANION_CONTEXT_TALK, true).talk.link == old);
    advance(1);
    observed_t out;
    product_on_owner(read_owner, &out);
    assert(!out.talk.session && out.talk.end == XIAOTAI_AI_TALK_END_TIMEOUT &&
           out.transport.links_releasing == 1 && held_bytes(old) == bytes_per_connect);
    advance(release_ms - 1U);
    assert(!calls(XIAOTAI_TRANSPORT_HOST_STOP) && held_bytes(old) == bytes_per_connect);
    advance(1);
    product_on_owner(read_owner, &out);
    assert(!out.talk.session && !out.transport.links_releasing && out.transport.release_stalls == 1);
    if (tolerant) {
        assert(out.transport.state == XIAOTAI_TRANSPORT_ONLINE && !calls(XIAOTAI_TRANSPORT_HOST_STOP));
        assert(held_bytes(old) == bytes_per_connect); /* Drop of bookkeeping is not a stack release. */
        late(old, 0x93U);
        close_ack(old, 0x93U);
    } else {
        assert(out.transport.state == XIAOTAI_TRANSPORT_FAULT && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
    }
    assert(!held_bytes(0));
    no_pending_api();
    printf("%s: connect=%u ms release=%u ms, %s; modeled heap restored\n", XIAOTAI_PRODUCT_NAME,
           (unsigned)connect_ms, (unsigned)release_ms, tolerant ? "late close only" : "confirmed STOP");
}

static void withdraw_twice(bool tolerant, bool color)
{
    uint32_t first, second;
    const uint32_t old = start_link(&first);
    product_ai_stop(first);
    (void)idle();
    if (color) {
        // This product lent its connection reserve to the first SDK request.
        // Before a callback arrives the next request gets a resource error.
        assert(product_ai_request() != first);
        const observed_t refused = idle();
        assert(refused.talk.end == XIAOTAI_AI_TALK_END_NO_MEMORY &&
               refused.transport.links_releasing == 1 && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
        late(old, 0x96U);
        close_ack(old, 0x96U);
        assert(!idle().transport.links_releasing && !held_bytes(0));
        no_pending_api();
        puts("CoreS3: second request refused while connection reserve is still lent");
        return;
    }
    const uint32_t next = start_link(&second);
    assert(old != next && held_bytes(0) == bytes_per_connect * (tolerant ? 2U : 1U));
    product_ai_stop(second);
    settle();
    observed_t out;
    product_on_owner(read_owner, &out);
    assert(!out.talk.session && !out.media.active);
    if (tolerant) {
        assert(out.transport.state == XIAOTAI_TRANSPORT_FAULT && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
    } else {
        assert(out.transport.state == XIAOTAI_TRANSPORT_ONLINE && out.transport.links_releasing == 1);
        assert(!held_bytes(next));
        late(old, 0x94U);
        close_ack(old, 0x94U);
        assert(!idle().transport.links_releasing);
    }
    assert(!held_bytes(0));
    no_pending_api();
    printf("%s: two withdrawals with one release slot -> %s\n", XIAOTAI_PRODUCT_NAME,
           tolerant ? "transport FAULT" : "queued successor never reached backend");
}

/* A conversation is hung up and the next one cancelled before the stack has
 * confirmed the first close: one link has one release record, so the second
 * withdrawal faults the transport. The product leaves the fault by itself. */
static void hang_up_then_withdraw(void)
{
    uint32_t first, second, third;
    const uint32_t old = start_link(&first);
    xiaotai_transport_host_connected(old, 0x97U, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    product_ai_stop(first);
    assert(idle().transport.links_releasing == 1);
    const uint32_t next = start_link(&second);
    assert(next != old);
    const uint32_t starts = calls(XIAOTAI_TRANSPORT_HOST_START);
    product_ai_stop(second);
    settle();
    assert(idle().transport.state == XIAOTAI_TRANSPORT_FAULT && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
    assert(!held_bytes(0));
    advance(4999U);
    assert(idle().transport.state == XIAOTAI_TRANSPORT_FAULT && calls(XIAOTAI_TRANSPORT_HOST_START) == starts);
    advance(1U);
    for (unsigned t = 0; t < WAIT_MS && idle().transport.state != XIAOTAI_TRANSPORT_ONLINE; t += 2) {
        xiaotai_delay_ms(2);
        settle();
    }
    product_platform_online();
    assert(idle().transport.connect_ready && calls(XIAOTAI_TRANSPORT_HOST_START) == starts + 1U);
    pthread_mutex_lock(&buffers_lock);
    submitted = 0; /* Both were given back by the stop. */
    memset(buffers, 0, sizeof(buffers));
    pthread_mutex_unlock(&buffers_lock);
    const uint32_t again = start_link(&third);
    assert(again != next && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 3);
    product_ai_stop(third);
    late(again, 0x98U);
    close_ack(again, 0x98U);
    assert(!idle().transport.links_releasing && !held_bytes(0));
    no_pending_api();
    puts("CoreS3: a fault left after 5000 ms, the next conversation connects");
}

static void refuse_while_yielded(void *ctx)
{
    (void)ctx;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    assert(platform.held);
    uint32_t generation = UINT32_MAX;
    assert(xiaotai_ai_talk_start(NULL, &generation) == XIAOTAI_ERR_PLATFORM_OFFLINE && !generation);
}

static void release_low_memory(void)
{
    uint32_t session;
    const uint32_t old = start_link(&session);
    // Transport can be idle while the platform is still yielding signaling.
    assert(xiaotai_signal_take(&low_memory_submitted, WAIT_MS));
    assert(held_bytes(old) == bytes_per_connect);
    settle();
    product_ai_stop(session);
    (void)idle();
    product_on_owner(refuse_while_yielded, NULL);
    assert(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1 && held_bytes(0) == bytes_per_connect);
    late(old, 0x95U);
    close_ack(old, 0x95U);
    assert(!held_bytes(0));
    no_pending_api();
    xiaotai_transport_host_connect_hook(NULL, NULL);
    xiaotai_signal_deinit(&low_memory_submitted);
    puts("CoreS3 low headroom: pending connect yields signaling and refuses new AI until late result");
}

void product_rtc_release(const char *scenario)
{
    const bool color = !strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-cores3-lite");
#if PRODUCT_TEST_RTC_XMINI_TOLERANT
    const bool tolerant = true;
#else
    const bool tolerant = color;
#endif
    product_platform_online();
    (void)idle();
    if (!strcmp(scenario, "rtc-release")) release_pair(tolerant);
    else if (!strcmp(scenario, "rtc-release-timeout")) release_timeout(tolerant, color);
    else if (!strcmp(scenario, "rtc-release-twice")) withdraw_twice(tolerant, color);
    else if (!strcmp(scenario, "rtc-release-low-memory")) release_low_memory();
    else if (!strcmp(scenario, "rtc-release-hangup")) hang_up_then_withdraw();
    else assert(false && "not an RTC release scenario");
}
