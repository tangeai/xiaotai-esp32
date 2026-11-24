// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_control.c::platform_control
// Replaces legacy: tests/regression/test_ai_token_response.py
// Replaces legacy: tests/regression/test_xmini_ai_token_http.py
// Replaces legacy: tests/regression/test_platform_http_trace.py
// Replaces legacy: tests/regression/test_queued_http.py
// Replaces legacy: tests/regression/test_binding_lifecycle.py
// Replaces legacy: tests/xmini/test_platform_task_metrics.c
// Replaces legacy: tests/xmini/platform_http_metadata_fixture.c
// Replaces legacy: tests/xmini/test_platform_routes.c::platform_routes
// Replaces legacy: tests/xmini/test_platform_request.c::platform_request
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_http_host.h"

#define WAIT 3000U
#define TRANSFER_SIZE 2050U

static xiaotai_signal_t done, entered, resume, fence;
static atomic_uint calls, replies, preflights, idle_events, hook_unbound, hook_auth, generation = 1;
static atomic_bool allow, identity_valid;
static _Thread_local bool advance_before_stamp, advance_at_stamp;
static _Thread_local uint32_t stale_stamp;
static xiaotai_platform_reply_t results[16];
static char bodies[16][129];
static xiaotai_platform_transfer_result_t transfer_result;
static char urls[24][XIAOTAI_PLATFORM_URL_MAX + XIAOTAI_PLATFORM_PATH_MAX];
static bool reused[24];
static bool hold_first, hold_reply, stall_preflight, resume_inside, expect_no_delay, provisioning, external_ram;
static bool exercise_preflight, no_retry_get, worker_create_fails;
static unsigned read_bytes, wrote_bytes;
static atomic_bool verify_transfer, hold_begin;
static unsigned worker_priority;
static uint8_t transfer_payload[TRANSFER_SIZE];
static atomic_bool trace_transfer_ticks;
static atomic_uint transfer_ticks;
static atomic_bool hold_before_http;
static _Atomic int64_t transfer_clock_offset;
static atomic_uint stack_sample_bytes, task_lists, stack_samples;
static atomic_bool hold_stack_sample;
static const char ai_reply_body[] = "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"fixture-ai-token\"}}";
static char ai_reply_scratch[sizeof(ai_reply_body)];
static uint16_t ai_reply_pressure_status;

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    return __real_xiaotai_time_us() + atomic_load(&transfer_clock_offset);
}

uint32_t __real_platform_signaling_step(void);
uint32_t __wrap_platform_signaling_step(void);
uint32_t __wrap_platform_signaling_step(void)
{
    if (atomic_load(&trace_transfer_ticks)) {
        platform_lock();
        const bool in_transfer = platform.io && platform.io->is_transfer;
        platform_unlock();
        if (in_transfer) atomic_fetch_add(&transfer_ticks, 1U);
    }
    return __real_platform_signaling_step();
}

xiaotai_err_t __real_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *request,
                                        xiaotai_http_response_t *response);
xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *request,
                                        xiaotai_http_response_t *response)
{
    if (atomic_exchange(&hold_before_http, false)) {
        platform_lock();
        assert(platform.io && platform.io->is_transfer && platform.io->state == PLATFORM_RUNNING);
        platform_unlock();
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    return __real_xiaotai_http_perform(client, request, response);
}

/* Tests supply identity and discovered/session input only. The real worker,
 * framework, port HTTP parser and digest run without opening sockets. */
bool xiaotai_net_provisioning_boot(void) { return provisioning; }
bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_mem_has_external(void) { return external_ram; }

static atomic_bool watch_body, fail_body, fail_bearer;
static _Atomic(void *) leased_body;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes);
void __real_xiaotai_mem_free(void *ptr);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes);
void __wrap_xiaotai_mem_free(void *ptr);

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes)
{
    const bool body = atomic_load(&watch_body) && kind == XIAOTAI_MEM_BULK && bytes == 8U;
    if (body && atomic_load(&fail_body)) return NULL;
    void *p = __real_xiaotai_mem_alloc(kind, bytes);
    if (body && p) assert(!atomic_exchange(&leased_body, p));
    return p;
}

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes)
{
    if (atomic_load(&fail_bearer) && kind == XIAOTAI_MEM_BULK && count == 1U &&
        bytes == XIAOTAI_PLATFORM_BEARER_MAX) return NULL;
    return __real_xiaotai_mem_calloc(kind, count, bytes);
}

void __wrap_xiaotai_mem_free(void *ptr)
{
    const void *body = atomic_load(&leased_body);
    if (body && body == ptr) {
        if (!atomic_load(&fail_bearer)) {
            const uint8_t *bytes = ptr;
            for (size_t i = 0; i < 8U; ++i) assert(!bytes[i]);
        }
        atomic_store(&leased_body, NULL);
    }
    __real_xiaotai_mem_free(ptr);
}
uint32_t __wrap_xiaotai_identity_generation(void);
uint32_t __wrap_xiaotai_identity_generation(void)
{
    if (advance_at_stamp) {
        advance_at_stamp = false;
        stale_stamp = atomic_fetch_add(&generation, 1U);
        return stale_stamp;
    }
    return atomic_load(&generation);
}
xiaotai_identity_state_t __wrap_xiaotai_identity_state(void);
xiaotai_identity_state_t __wrap_xiaotai_identity_state(void)
{
    if (advance_before_stamp) {
        advance_before_stamp = false;
        atomic_fetch_add(&generation, 1U);
    }
    return atomic_load(&identity_valid) ? XIAOTAI_IDENTITY_VALID : XIAOTAI_IDENTITY_ABSENT;
}
xiaotai_err_t __wrap_xiaotai_identity_get(xiaotai_identity_t *out);
xiaotai_err_t __wrap_xiaotai_identity_get(xiaotai_identity_t *out)
{
    *out = (xiaotai_identity_t){.device_id = "fixture", .device_key = "fixture-key"};
    return atomic_load(&identity_valid) ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (!strcmp(config->name, "xiaotai_platform")) {
        worker_priority = config->priority;
        assert(config->stack == XIAOTAI_MEM_BULK && config->stack_bytes == (external_ram ? 13312U : 8192U));
        if (worker_create_fails) return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_task_create(config, task);
}

uint32_t __wrap_xiaotai_task_current_stack_free(void);
uint32_t __wrap_xiaotai_task_current_stack_free(void)
{
    assert(xiaotai_task_is_current(&platform.task) && !xiaotai_on_owner());
    if (atomic_exchange(&hold_stack_sample, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    atomic_fetch_add(&stack_samples, 1U);
    return atomic_load(&stack_sample_bytes);
}

xiaotai_err_t __real_xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count);
xiaotai_err_t __wrap_xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count);
xiaotai_err_t __wrap_xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count)
{
    atomic_fetch_add(&task_lists, 1U);
    return __real_xiaotai_task_list(tasks, capacity, count);
}

static void wait_for(atomic_uint *value, unsigned n)
{
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000LL;
    while (atomic_load(value) < n && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(value) >= n);
}

static xiaotai_platform_status_t status(void)
{
    xiaotai_platform_status_t out;
    xiaotai_platform_status(&out);
    return out;
}

static void hook(void *ctx) { xiaotai_signal_give(ctx); }
static void owner_sync(void)
{
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000LL;
    xiaotai_err_t err;
    do {
        err = xiaotai_run(hook, &fence);
        if (err != XIAOTAI_OK) xiaotai_delay_ms(1);
    } while (err != XIAOTAI_OK && xiaotai_time_us() < deadline);
    assert(err == XIAOTAI_OK && xiaotai_signal_take(&fence, WAIT));
}

static const char *header(const xiaotai_http_host_request_t *request, const char *name)
{
    for (size_t i = 0; i < request->header_count; ++i) {
        if (!strcmp(request->headers[i].name, name)) return request->headers[i].value;
    }
    return NULL;
}

static void uploaded(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert(size == 5 && !memcmp(data, "bytes", 5));
}

static void p5_uploaded(void *ctx, const uint8_t *data, size_t size)
{
    assert(!xiaotai_on_owner() && !ctx && size == sizeof(transfer_payload) &&
           !memcmp(data, transfer_payload, size));
}

static void p5_respond(const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    static char encoding[XIAOTAI_PLATFORM_CONTENT_TYPE_MAX + 15U];
    static const xiaotai_http_header_t headers[] = {
        {"Content-Type", "audio/wav"}, {"content-type", "other"},
        {"Content-Encoding", encoding}, {"content-encoding", "gzip"},
    };
    const char *url = request->url;
    const bool upload = strstr(url, "/p5/upload") != NULL;
    if (upload) {
        assert(request->streamed && request->method == XIAOTAI_HTTP_POST && request->body_size == TRANSFER_SIZE);
        const char *type = header(request, "Content-Type");
        assert(type && !strcmp(type, "fixture/audio"));
        reply->uploaded = p5_uploaded;
    } else assert(!request->streamed && request->method == XIAOTAI_HTTP_GET);
    if (strstr(url, "/p5/bearer")) {
        assert(!strcmp(url, "http://fixture.local/p5/bearer?clip=one%2Btwo&take=1"));
        const char *auth = header(request, "Authorization");
        assert(auth && !strcmp(auth, "Bearer fixture-bearer"));
    }
    if (strstr(url, "/p5/unsent")) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RESOLVE;
        reply->error = XIAOTAI_FAIL;
        return;
    }
    if (strstr(url, "/p5/reused")) {
        assert(request->reused);
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->error = XIAOTAI_FAIL;
        return;
    }
    memset(encoding, 'a', sizeof(encoding) - 1U);
    reply->headers = headers;
    reply->header_count = sizeof(headers) / sizeof(headers[0]);
    reply->body = transfer_payload;
    reply->body_size = sizeof(transfer_payload);
    reply->read_chunk_bytes = 257U;
    reply->content_length = (int64_t)reply->body_size;
    if (strstr(url, "/p5/unknown")) reply->content_length = -1;
    if (strstr(url, "/p5/empty")) {
        reply->status = 204;
        reply->content_length = 0;
        reply->body_size = 0;
    }
    if (strstr(url, "/p5/unbound")) {
        reply->status = 410;
        reply->body = (const uint8_t *)"{\"code\":6006}";
        reply->content_length = 13;
        reply->body_size = 13;
    }
    if (strstr(url, "/p5/unauth")) reply->status = 401;
    if (strstr(url, "/p5/slow")) reply->read_chunk_ms = 115U;
}

static void respond(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && xiaotai_platform_exchanges_busy() && request->options.no_delay == expect_no_delay);
    const unsigned index = atomic_load(&calls);
    assert(index < sizeof(urls) / sizeof(urls[0]));
    strcpy(urls[index], request->url);
    reused[index] = request->reused;
    atomic_fetch_add(&calls, 1);
    if (hold_first && !index) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    const char *url = request->url;
    if (strstr(url, "/p5/")) { p5_respond(request, reply); return; }
    if (ai_reply_pressure_status) {
        assert(!strcmp(url, "http://fixture.local/v1/ai/token"));
        assert(request->method == XIAOTAI_HTTP_GET && !request->body && !request->body_size);
        assert(!strcmp(header(request, "Authorization"), "Bearer fixture-token"));
        reply->status = ai_reply_pressure_status;
        reply->body = (const uint8_t *)ai_reply_scratch;
        reply->body_size = strlen(ai_reply_scratch);
        reply->content_length = (int64_t)reply->body_size;
        return;
    }
    const char *const ok = "{\"code\":200}";
    const char *const denied = "{\"code\":6006}";
    const char *const duplicate = "{\"code\":200,\"code\":201}";
    const char *body = ok;
    if (strstr(url, "/unbound")) { reply->status = 410; body = denied; }
    if (strstr(url, "/unauth")) reply->status = 401;
    if (strstr(url, "/status")) { reply->status = 418; body = duplicate; }
    if (strstr(url, "/business-code")) body = "{\"code\":40400}";
    if (strstr(url, "/wire-fail")) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RESOLVE;
        reply->error = XIAOTAI_FAIL;
        return;
    }
    if (strstr(url, "/bearer")) assert(!strcmp(header(request, "Authorization"), "Bearer fixture-bearer"));
    if (strstr(url, "/signed")) {
        assert(!strcmp(header(request, "X-Device-Id"), "fixture"));
        assert(strlen(header(request, "X-Timestamp")) == 10U);
        assert(strlen(header(request, "X-Nonce")) == 16U);
        assert(strlen(header(request, "X-Signature")) == 44U);
        assert(!header(request, "Authorization"));
    }
    if (strstr(url, "/copied")) assert(request->body_size == 7 && !memcmp(request->body, "{\"x\":1}", 7));
    if (strstr(url, "/reuse") && request->reused) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->error = XIAOTAI_FAIL;
        return;
    }
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
    reply->content_length = (int64_t)reply->body_size;
    if (strstr(url, "/large")) {
        static uint8_t large[130];
        memset(large, 'x', sizeof(large));
        reply->body = large;
        reply->body_size = sizeof(large);
        reply->content_length = (int64_t)reply->body_size;
    }
    if (strstr(url, "/nul")) {
        static const uint8_t malformed[] = {'{', '}', 0, 'X'};
        reply->body = malformed;
        reply->body_size = sizeof(malformed);
        reply->content_length = (int64_t)reply->body_size;
        reply->read_chunk_bytes = 2;
    }
    if (strstr(url, "/transfer")) {
        static const xiaotai_http_header_t headers[] = {{"Content-Type", "fixture/stream"},
            {"CONTENT-TYPE", "wrong/value"}, {"Content-Encoding", "identity"}};
        reply->headers = headers;
        reply->header_count = sizeof(headers) / sizeof(headers[0]);
        reply->body = (const uint8_t *)"answer";
        reply->body_size = 6;
        reply->content_length = 6;
        reply->read_chunk_bytes = 2;
        if (request->streamed) {
            assert(request->body_size == 5 && !strcmp(header(request, "Content-Type"), "fixture/upload"));
            reply->uploaded = uploaded;
        }
    }
}

static void handler(const xiaotai_platform_reply_t *reply, void *ctx)
{
    if (ai_reply_pressure_status) assert(ctx == ai_reply_scratch);
    assert(xiaotai_on_owner() && xiaotai_platform_exchanges_busy());
    const unsigned index = atomic_load(&replies);
    assert(index < sizeof(results) / sizeof(results[0]));
    results[index] = *reply;
    if (reply->body) { assert(reply->size < sizeof(bodies[0])); strcpy(bodies[index], reply->body); }
    if (hold_reply && !index) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    atomic_fetch_add(&replies, 1);
    xiaotai_signal_give(&done);
}

static void idle(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && event->args.value == 0);
    atomic_fetch_add(&idle_events, 1);
}

static void on_unbound(uint32_t value) { assert(xiaotai_on_owner() && value == atomic_load(&generation)); atomic_fetch_add(&hook_unbound, 1); }
static void on_auth(void) { assert(xiaotai_on_owner()); atomic_fetch_add(&hook_auth, 1); }

static xiaotai_err_t gate_preflight(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && xiaotai_platform_exchanges_busy());
    const unsigned n = atomic_fetch_add(&preflights, 1) + 1U;
    if (n == 1U && resume_inside) {
        xiaotai_platform_exchange_preflight_resume();
        return XIAOTAI_ERR_BUSY;
    }
    if (stall_preflight && !atomic_load(&allow)) return XIAOTAI_ERR_BUSY;
    return XIAOTAI_OK;
}

static void gate_unavailable(void)
{
    uint32_t untouched = 19;
    assert(xiaotai_platform_gate_close(NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_platform_gate_close(&untouched) == XIAOTAI_ERR_INVALID_STATE && untouched == 19);
    assert(!xiaotai_platform_gate_drained(untouched));
    assert(xiaotai_platform_gate_open(untouched) == XIAOTAI_ERR_INVALID_ARG);
}

static xiaotai_err_t registrations(void)
{
    if (provisioning) {
        assert(atomic_load(&platform.initialized) && !atomic_load(&platform.started));
        gate_unavailable();
    }
    const xiaotai_platform_requirement_t first = {.request_body_bytes = 24, .response_bytes = 63, .concurrent_requests = 2};
    const xiaotai_platform_requirement_t second = {.request_body_bytes = 65, .response_bytes = 128, .concurrent_requests = 6};
    xiaotai_platform_requirement_t failed = second;
    failed.signal_bytes = 9000;
    assert(xiaotai_platform_require(&first) == XIAOTAI_OK && xiaotai_platform_require(&second) == XIAOTAI_OK);
    assert(xiaotai_platform_require(&failed) == XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_platform_require_service("ai") == XIAOTAI_OK);
    assert(xiaotai_platform_require_service("ai") == XIAOTAI_OK);
    if (exercise_preflight) assert(xiaotai_platform_exchange_preflight_register(gate_preflight, NULL) == XIAOTAI_OK);
    platform_reply_hooks(&(platform_reply_hooks_t){.binding_lost = on_unbound, .auth_failed = on_auth});
    return xiaotai_event_subscribe(XIAOTAI_EVENT_PLATFORM_IDLE, idle, NULL);
}

static void set_session(xiaotai_platform_phase_t phase, bool authorized, bool signaling)
{
    const xiaotai_platform_status_t state = {.phase = phase, .authorized = authorized, .signaling = signaling};
    platform_session_publish(&state, authorized ? "fixture-token" : NULL);
}

static void boot(const xiaotai_platform_config_t *config)
{
    static const char *const depends[] = {"platform", NULL};
    static const xiaotai_module_t engine = {.name = "platform", .init = xiaotai_engine_platform_init, .start = xiaotai_engine_platform_start};
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = depends, .init = registrations};
    static const xiaotai_module_t *const mods[] = {&engine, &fixture};
    const xiaotai_framework_config_t framework = {.modules = mods, .module_count = 2};
#if XIAOTAI_PLATFORM_RECONNECT_COMMAND
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
#endif
    assert(xiaotai_platform_configure(config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    assert(xiaotai_platform_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_platform_require(&(xiaotai_platform_requirement_t){0}) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_http_host_set_responder(respond, NULL, false);
    owner_sync();
}

static void discover(void)
{
    const platform_service_t services[] = {{"device", "http://fixture.local"}, {"ai", "http://fixture.local"},
                                          {"bad", NULL}, {"other", "http://other.local"}, {"mqtt", "mqtt://fixture.local"}};
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    xiaotai_platform_set_network(true, mac);
    assert(platform_services_replace(services, sizeof(services) / sizeof(services[0])) == XIAOTAI_OK);
}

static xiaotai_platform_request_t make_request(const char *path)
{
    return (xiaotai_platform_request_t){.service = "ai", .path = path, .auth = XIAOTAI_PLATFORM_AUTH_NONE};
}

static xiaotai_err_t send(const char *path)
{
    const xiaotai_platform_request_t req = make_request(path);
    return xiaotai_platform_request(&req, handler, NULL);
}

static void wait_idle(unsigned count)
{
    wait_for(&idle_events, count);
    owner_sync();
    assert(!xiaotai_platform_exchanges_busy() && !status().exchanges);
}

static void wait_stack_sample(uint32_t bytes)
{
    const int64_t deadline = xiaotai_time_us() + 1000000;
    xiaotai_platform_stats_t stats;
    do {
        xiaotai_platform_stats(&stats);
        if (stats.worker_stack_free == bytes) return;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "platform stack: sampled %u bytes, expected %u (task-list calls: %u)\n",
            (unsigned)stats.worker_stack_free, (unsigned)bytes, atomic_load(&task_lists));
    assert(stats.worker_stack_free == bytes);
}

static void stack_watermark(void)
{
    wait_stack_sample(0);
    atomic_store(&stack_sample_bytes, 2345U);
    atomic_store(&hold_stack_sample, true);
    boot(NULL);
    assert(xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_stats(NULL);
    wait_stack_sample(0); /* A created worker has not yet published its first sample. */
    assert(!atomic_load(&stack_samples));
    xiaotai_signal_give(&resume);
    wait_stack_sample(2345U);

    discover();
    atomic_store(&stack_sample_bytes, 0U);
    atomic_fetch_add(&transfer_clock_offset, 2000000);
    assert(send("/ok") == XIAOTAI_OK);
    wait_idle(1);
    wait_stack_sample(0); /* Real zero from the worker, not a stale nonzero cache. */

    atomic_store(&stack_sample_bytes, 1728U);
    atomic_fetch_add(&transfer_clock_offset, 2000000);
    assert(send("/ok") == XIAOTAI_OK);
    wait_idle(2);
    wait_stack_sample(1728U);
    assert(atomic_load(&stack_samples) >= 3U && !atomic_load(&task_lists));
    xiaotai_platform_reset();
    const unsigned stopped = atomic_load(&stack_samples);
    wait_stack_sample(0);
    assert(atomic_load(&stack_samples) == stopped && !atomic_load(&task_lists));
}

static void admission(void)
{
    boot(&(xiaotai_platform_config_t){.request_slots = 2, .requests_need_signaling = true});
    assert(send("/before") == XIAOTAI_ERR_PLATFORM_OFFLINE);
    discover();
    xiaotai_platform_stats_t stats;
    xiaotai_platform_stats(&stats);
    assert(stats.slot_clamps == 1U && platform.slot_count == 2U && platform.request_limit == 65U && platform.response_limit == 128U);
    assert(!platform.response);
    char url[256];
    assert(xiaotai_platform_endpoint("mqtt", url, sizeof(url)) == XIAOTAI_OK && !strcmp(url, "mqtt://fixture.local"));
    assert(xiaotai_platform_endpoint("bad", url, sizeof(url)) == XIAOTAI_ERR_PLATFORM_BAD_SERVICE);
    assert(xiaotai_platform_endpoint("missing", url, sizeof(url)) == XIAOTAI_ERR_NOT_SUPPORTED);
    const xiaotai_platform_phase_t phases[] = {XIAOTAI_PLATFORM_NO_IDENTITY, XIAOTAI_PLATFORM_UNBOUND, XIAOTAI_PLATFORM_PAUSED};
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        set_session(phases[i], false, false);
        assert(send("/ok") == XIAOTAI_OK);
        wait_idle((unsigned)i + 1U);
    }
    xiaotai_platform_request_t req = make_request("/signed");
    req.auth = XIAOTAI_PLATFORM_AUTH_SIGNED;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(xiaotai_clock_set_unix_ms(1750000000000LL) == XIAOTAI_OK);
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    atomic_store(&identity_valid, true);
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    wait_idle(4);
    set_session(XIAOTAI_PLATFORM_CONNECTING, true, false);
    req.auth = XIAOTAI_PLATFORM_AUTH_SESSION;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    hold_first = false;
    req = make_request("/bearer");
    req.auth = XIAOTAI_PLATFORM_AUTH_BEARER;
    req.bearer = "fixture-bearer";
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    wait_idle(5);
    char path[193];
    memset(path, 'a', sizeof(path));
    path[0] = '/'; path[191] = 0;
    assert(send(path) == XIAOTAI_OK);
    path[191] = 'a'; path[192] = 0;
    assert(send(path) == XIAOTAI_ERR_INVALID_ARG);
    wait_idle(6);
    req = make_request("/ok");
    req.method = (xiaotai_platform_method_t)-1;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_INVALID_ARG);
    req.method = (xiaotai_platform_method_t)(XIAOTAI_PLATFORM_DELETE + 1);
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(atomic_load(&calls) == 6U && !platform.bearer);
}

static void request_body_lease(bool resident, bool full_timeout)
{
    external_ram = resident;
    hold_first = hold_reply = true;
    boot(&(xiaotai_platform_config_t){.request_slots = 2U, .request_full_timeout = full_timeout});
    discover();
    assert(platform.slot_count == 2U && platform.request_limit == 65U);
    char *first = platform.slots[0].body;
    char *second = platform.slots[1].body;
    assert(resident ? first && second == first + 65U : !first && !second);

    char body[] = "{\"x\":1}";
    xiaotai_platform_request_t req = make_request("/copied");
    req.method = XIAOTAI_PLATFORM_POST;
    req.body = body;
    watch_body = true;
    if (!resident) {
        fail_body = true;
        assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_NO_MEM);
        fail_body = false;
        req.auth = XIAOTAI_PLATFORM_AUTH_BEARER;
        req.bearer = "fixture-bearer";
        fail_bearer = true;
        assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_NO_MEM);
        fail_bearer = false;
        req.auth = XIAOTAI_PLATFORM_AUTH_NONE;
        req.bearer = NULL;
        assert(!status().exchanges && !atomic_load(&calls) && !atomic_load(&replies));
        assert(!atomic_load(&leased_body) && !platform.bearer);
    }
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&entered, WAIT));
    assert((atomic_load(&leased_body) != NULL) == !resident);
    memset(body, 'x', sizeof(body));
    xiaotai_signal_give(&resume);
    assert(xiaotai_signal_take(&entered, WAIT));
    assert(status().exchanges == 1U && !atomic_load(&replies) && !atomic_load(&leased_body));
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(results[0].result == XIAOTAI_OK);
    assert(platform.slots[0].body == first && platform.slots[1].body == second);
    printf("request body: slots=%u limit=%u idle_body_bytes=%u\n", (unsigned)platform.slot_count,
           (unsigned)platform.request_limit, resident ? 130U : 0U);
}

static void done_stream(void *ctx, const xiaotai_platform_transfer_result_t *result);

static void request_saturation(bool full_timeout)
{
    external_ram = hold_first = true;
    boot(&(xiaotai_platform_config_t){.request_slots = 4U, .request_full_timeout = full_timeout});
    discover();
    set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
    const xiaotai_platform_request_t request = {
        .service = "device", .path = "/fixture", .method = XIAOTAI_PLATFORM_POST,
        .body = "{}", .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = 1000,
    };
    assert(xiaotai_platform_request(&request, handler, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&entered, WAIT));
    for (unsigned i = 1; i < 4U; ++i)
        assert(xiaotai_platform_request(&request, handler, NULL) == XIAOTAI_OK);

    const xiaotai_err_t result = xiaotai_platform_request(&request, handler, NULL);
    printf("full pool: accepted=%u fifth=%#x\n", (unsigned)status().exchanges, (unsigned)result);
    assert(result == (full_timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_ERR_NO_MEM));
    xiaotai_platform_transfer_t transfer = {.request = make_request("/full-transfer")};
    const xiaotai_platform_stream_t stream = {.done = done_stream};
    uint32_t transfer_id = 1U;
    assert(xiaotai_platform_transfer(&transfer, &stream, &transfer_id) == XIAOTAI_ERR_NO_MEM && !transfer_id);
    transfer.request.flags = XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT;
    transfer_id = 1U;
    assert(xiaotai_platform_transfer(&transfer, &stream, &transfer_id) ==
           (full_timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_ERR_NO_MEM) && !transfer_id);
    assert(status().exchanges == 4U && atomic_load(&calls) == 1U && !atomic_load(&replies));
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&calls) == 4U && atomic_load(&replies) == 4U && !status().exchanges);
}

static void ordering(void)
{
    hold_first = hold_reply = true;
    boot(&(xiaotai_platform_config_t){.request_slots = 4});
    discover();
    assert(send("/first") == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    char path[] = "/copied", body[] = "{\"x\":1}";
    xiaotai_platform_request_t req = make_request(path);
    req.method = XIAOTAI_PLATFORM_POST;
    req.body = body;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    memset(path, 'x', sizeof(path));
    memset(body, 'x', sizeof(body));
    req = make_request("/v1/ai/token"); req.flags = XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    uint32_t gate = 0, second = 7;
    assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK && gate);
    assert(xiaotai_platform_gate_close(&second) == XIAOTAI_ERR_PLATFORM_GATED && second == 7);
    assert(send("/refused") == XIAOTAI_ERR_PLATFORM_GATED);
    assert(!xiaotai_platform_gate_drained(gate) && status().exchanges == 3);

    /* Cancel maintenance before draining, then fill the reopened pool. The
     * native API distinguishes full capacity from an elapsed deadline. */
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
    req = make_request("/v1/call/hangup"); req.flags = XIAOTAI_PLATFORM_URGENT;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(send("/overflow") == XIAOTAI_ERR_NO_MEM);
    assert(status().exchanges == 4 && atomic_load(&calls) == 1 && !atomic_load(&replies));
    const uint32_t cancelled = gate;
    gate = 0;
    assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK && gate != cancelled);
    assert(xiaotai_platform_gate_open(cancelled) == XIAOTAI_ERR_INVALID_ARG);
    assert(!xiaotai_platform_gate_drained(cancelled) && !xiaotai_platform_gate_drained(gate));
    assert(send("/still-gated") == XIAOTAI_ERR_PLATFORM_GATED);

    xiaotai_signal_give(&resume);
    assert(xiaotai_signal_take(&entered, WAIT));
    assert(atomic_load(&calls) == 1U && status().exchanges == 4 && !xiaotai_platform_gate_drained(gate));
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(strstr(urls[1], "/v1/call/hangup") && strstr(urls[2], "/v1/ai/token") && strstr(urls[3], "/copied"));
    assert(atomic_load(&calls) == 4 && atomic_load(&replies) == 4);
    assert(!reused[3]); /* The token request releases the client before ordinary work. */
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000LL;
    while (!xiaotai_platform_gate_drained(gate) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(xiaotai_platform_gate_drained(gate));
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
    assert(xiaotai_platform_gate_close(&second) == XIAOTAI_OK && second != gate);
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_platform_gate_open(second) == XIAOTAI_OK);
}

static void response(void)
{
    external_ram = true;
    boot(&(xiaotai_platform_config_t){.auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE});
    assert(platform.response);
    discover();
    set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
    const char *const paths[] = {"/status", "/large", "/nul", "/unbound", "/unauth", "/business-code", "/wire-fail"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        xiaotai_platform_request_t req = make_request(paths[i]);
        req.auth = i == 4U ? XIAOTAI_PLATFORM_AUTH_SESSION : XIAOTAI_PLATFORM_AUTH_NONE;
        assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
        wait_idle((unsigned)i + 1);
    }
    assert(results[0].result == XIAOTAI_OK && results[0].http_status == 418 && results[0].code == XIAOTAI_PLATFORM_NO_CODE);
    assert(!strcmp(bodies[0], "{\"code\":200,\"code\":201}"));
    assert(results[1].result == XIAOTAI_ERR_INVALID_SIZE && results[1].http_status == 200 && !results[1].body);
    assert(results[2].result != XIAOTAI_OK && results[2].http_status == 200 && !results[2].body);
    assert(results[3].http_status == 410 && results[3].code == 6006 && atomic_load(&hook_unbound) == 1);
    assert(results[4].http_status == 401 && atomic_load(&hook_auth) == 1);
    assert(results[5].result == XIAOTAI_OK && results[5].http_status == 200 && results[5].code == 40400);
    assert(!strcmp(bodies[5], "{\"code\":40400}"));
    assert(results[6].result == XIAOTAI_FAIL && !results[6].http_status &&
           results[6].code == XIAOTAI_PLATFORM_NO_CODE && !results[6].body);
}

static void stale(void)
{
    hold_first = true;
    boot(&(xiaotai_platform_config_t){.request_slots = 4, .requests_need_signaling = true});
    discover();
    set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
    xiaotai_platform_request_t req = make_request("/first");
    req.auth = XIAOTAI_PLATFORM_AUTH_SESSION;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    req.path = "/staled-queued";
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(send("/not-session") == XIAOTAI_OK);
    set_session(XIAOTAI_PLATFORM_CONNECTING, true, false);
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&calls) == 2 && results[0].result == XIAOTAI_ERR_PLATFORM_STALE && results[0].http_status == 200);
    assert(!results[0].body && results[1].result == XIAOTAI_ERR_PLATFORM_STALE && !results[1].http_status);
    assert(results[2].result == XIAOTAI_OK && strstr(urls[1], "/not-session"));
}

static void admission_epoch(void)
{
    boot(&(xiaotai_platform_config_t){.request_slots = 2, .requests_need_signaling = true});
    discover();
    set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
    const uint32_t initial = atomic_load(&generation);
    xiaotai_platform_request_t req = make_request("/stale-during-admission");
    req.method = XIAOTAI_PLATFORM_POST;
    req.auth = XIAOTAI_PLATFORM_AUTH_SESSION;
    req.body = "{\"x\":1}";
    atomic_store(&watch_body, true);
    /* Only the admitting thread changes the generation. The real worker
     * sees the new value by the time the old stamp is published to it. */
    advance_at_stamp = true;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(!advance_at_stamp && stale_stamp == initial && atomic_load(&generation) == initial + 1U);
    wait_idle(1);
    assert(atomic_load(&replies) == 1U && !atomic_load(&calls) && !atomic_load(&leased_body));
    assert(results[0].generation == initial && results[0].result == XIAOTAI_ERR_PLATFORM_STALE &&
           !results[0].http_status && !results[0].body && !results[0].size);
    atomic_store(&watch_body, false);

    assert(xiaotai_clock_set_unix_ms(1750000000000LL) == XIAOTAI_OK);
    atomic_store(&identity_valid, true);
    req = make_request("/signed-after-change");
    req.auth = XIAOTAI_PLATFORM_AUTH_SIGNED;
    advance_before_stamp = true;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(!advance_before_stamp && atomic_load(&generation) == initial + 2U);
    wait_idle(2);
    assert(atomic_load(&replies) == 2U && atomic_load(&calls) == 1U &&
           results[1].generation == initial + 2U && results[1].result == XIAOTAI_OK &&
           strstr(urls[0], "/signed-after-change"));

    char at_limit[66];
    assert(platform.request_limit + 1U == sizeof(at_limit));
    memset(at_limit, 'x', sizeof(at_limit));
    memcpy(at_limit, "{\"x\":\"", 6U);
    memcpy(at_limit + sizeof(at_limit) - 3U, "\"}", 3U);
    req = make_request("/refused-body");
    req.body = "{}";
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_INVALID_ARG);
    req.method = XIAOTAI_PLATFORM_POST;
    req.body = at_limit;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_INVALID_ARG);
    owner_sync();
    assert(!xiaotai_platform_exchanges_busy() && atomic_load(&calls) == 1U && atomic_load(&replies) == 2U);
    printf("admission epoch: stale=%u current=%u wire=1 replies=2; two invalid bodies rejected at admission\n",
           (unsigned)initial, (unsigned)results[1].generation);

    const uint32_t expected = atomic_load(&generation);
    req = make_request("/must-not-send");
    req.method = XIAOTAI_PLATFORM_POST;
    req.body = "{\"x\":1}";
    assert(xiaotai_platform_request_for_generation(&req, 0, handler, NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_platform_request_for_generation(&req, initial, handler, NULL) == XIAOTAI_ERR_PLATFORM_STALE);
    assert(!xiaotai_platform_exchanges_busy() && atomic_load(&calls) == 1U && atomic_load(&replies) == 2U);

    /* If the generation changes at admission itself, the slot must still
     * carry the caller's generation, not the new one. */
    advance_at_stamp = true;
    assert(xiaotai_platform_request_for_generation(&req, expected, handler, NULL) == XIAOTAI_OK);
    assert(!advance_at_stamp && atomic_load(&generation) == expected + 1U);
    wait_idle(3);
    assert(results[2].generation == expected && results[2].result == XIAOTAI_ERR_PLATFORM_STALE &&
           !results[2].http_status && !results[2].body && atomic_load(&calls) == 1U);
    req = make_request("/ok");
    assert(xiaotai_platform_request_for_generation(&req, expected + 1U, handler, NULL) == XIAOTAI_OK);
    wait_idle(4);
    assert(results[3].generation == expected + 1U && results[3].result == XIAOTAI_OK &&
           atomic_load(&calls) == 2U && atomic_load(&replies) == 4U);
}

static void connection(void)
{
    expect_no_delay = true;
    boot(&(xiaotai_platform_config_t){.idle_keep_ms = 1000, .http_no_delay = true, .get_retry_on_reuse = !no_retry_get});
    discover();
    assert(send("/first") == XIAOTAI_OK); wait_idle(1);
    assert(send("/reuse") == XIAOTAI_OK); wait_idle(2);
    const unsigned base = no_retry_get ? 2U : 3U;
    assert(atomic_load(&calls) == base && reused[1] && (!no_retry_get ? !reused[2] : true));
    assert(results[1].result == (no_retry_get ? XIAOTAI_FAIL : XIAOTAI_OK));
    assert(send("/keep-again") == XIAOTAI_OK); wait_idle(3);
    xiaotai_platform_request_t req = make_request("/reuse-post"); req.method = XIAOTAI_PLATFORM_POST;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK); wait_idle(4);
    assert(atomic_load(&calls) == base + 2U && reused[base + 1U] && results[3].result == XIAOTAI_FAIL);
}

static void queued_connection(bool release)
{
    hold_first = true;
    boot(NULL); discover();
    xiaotai_platform_request_t req = make_request("/first");
    if (release) req.flags = XIAOTAI_PLATFORM_RELEASE;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    /* Queue was empty when the first request began; the second one arrives
     * while its I/O is already running. RELEASE is the only difference. */
    assert(send("/next") == XIAOTAI_OK);
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&calls) == 2 && reused[1] == !release);
    assert(send("/later") == XIAOTAI_OK);
    wait_idle(2);
    assert(!reused[2]);
}

static void legacy_http_no_delay(void)
{
    static const struct { const char *service, *path; bool post; } routes[] = {
        {"ai", "/v1/ai/token", false},
        {"call", "/v1/call/room", true},
        {"call", "/v1/call/group/device/assignment", false},
        {"call", "/v1/call/group/device/create", true},
        {"call", "/v1/call/group/device/join", true},
        {"call", "/v1/call/group/device/leave", true},
        {"call", "/v1/call/group/device/connect-token", true},
        {"call", "/v1/call/group/device/presence", true},
    };
    expect_no_delay = true;
    boot(&(xiaotai_platform_config_t){.http_no_delay = true});
    discover();
    const platform_service_t services[] = {
        {"ai", "http://fixture.local"}, {"call", "http://fixture.local"},
    };
    assert(platform_services_replace(services, sizeof(services) / sizeof(services[0])) == XIAOTAI_OK);
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const xiaotai_platform_request_t req = {
            .service = routes[i].service, .path = routes[i].path,
            .method = routes[i].post ? XIAOTAI_PLATFORM_POST : XIAOTAI_PLATFORM_GET,
            .auth = XIAOTAI_PLATFORM_AUTH_NONE, .body = routes[i].post ? "{}" : NULL,
        };
        assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
        wait_idle(i + 1U);
        assert(atomic_load(&calls) == i + 1U && atomic_load(&replies) == i + 1U);
        assert(results[i].result == XIAOTAI_OK && results[i].http_status == 200);
        assert(!strcmp(urls[i] + sizeof("http://fixture.local") - 1U, routes[i].path));
    }
}

static void allow_now(void *ctx)
{
    (void)ctx;
    atomic_store(&allow, true);
    xiaotai_platform_exchange_preflight_resume();
}

static void preflight_case(bool resume_first, bool timeout)
{
    exercise_preflight = true;
    stall_preflight = !resume_first;
    resume_inside = resume_first;
    boot(NULL);
    discover();
    platform_profile_exchange(true);
    assert(status().exchanges == 1 && xiaotai_platform_exchanges_busy());
    xiaotai_platform_request_t req = make_request("/first");
    req.timeout_ms = timeout ? 90 : 1000;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    wait_for(&preflights, 1);
    if (!resume_first) {
        assert(!atomic_load(&calls) && status().exchanges == 2);
        if (!timeout) assert(xiaotai_run(allow_now, NULL) == XIAOTAI_OK);
    }
    wait_for(&replies, 1);
    assert(atomic_load(&idle_events) == 0 && status().exchanges >= 1);
    assert(atomic_load(&calls) == (timeout ? 0U : 1U));
    assert(atomic_load(&preflights) == (timeout ? 1U : 2U));
    assert(results[0].result == (timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
    platform_profile_exchange(false);
    wait_idle(1);
}

static void full_owner(void *ctx)
{
    (void)ctx;
    unsigned count = 0;
    while (xiaotai_run(hook, &fence) == XIAOTAI_OK) assert(++count < 256);
    xiaotai_signal_give(&entered);
    assert(xiaotai_signal_take(&resume, WAIT));
}

static void priority_and_queue(void)
{
    exercise_preflight = true;
    boot(NULL);
    assert(worker_priority == (XIAOTAI_FRAMEWORK_OWNER_PRIORITY > 1 ? XIAOTAI_FRAMEWORK_OWNER_PRIORITY - 1U : 1U));
    discover();
    assert(xiaotai_run(full_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    assert(send("/first") == XIAOTAI_OK && xiaotai_platform_exchanges_busy());
    xiaotai_delay_ms(50);
    assert(!atomic_load(&calls) && !atomic_load(&replies) && !atomic_load(&preflights));
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&calls) == 1 && atomic_load(&preflights) == 1 && atomic_load(&replies) == 1);
}

static uint32_t activity;
static void activity_change(void *ctx)
{
    (void)ctx;
    if (activity) {
        xiaotai_activity_end(activity);
        activity = 0;
    } else {
        const xiaotai_activity_t request = {.name = "stream"};
        assert(xiaotai_activity_begin(&request, &activity) == XIAOTAI_OK);
    }
}

static void background(void)
{
    hold_first = true;
    boot(&(xiaotai_platform_config_t){.defer_background_while_active = true, .worker_priority = 20});
    assert(worker_priority == XIAOTAI_FRAMEWORK_OWNER_PRIORITY);
    discover();
    assert(!xiaotai_platform_background_deferred());
    assert(send("/first") == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_request_t req = make_request("/background");
    req.flags = XIAOTAI_PLATFORM_BACKGROUND;
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(activity_change, NULL) == XIAOTAI_OK);
    owner_sync();
    assert(xiaotai_activity_active() && xiaotai_platform_background_deferred());
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_ERR_BUSY);
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&calls) == 1 && atomic_load(&replies) == 2 && results[1].result == XIAOTAI_ERR_BUSY);
    assert(xiaotai_run(activity_change, NULL) == XIAOTAI_OK);
    owner_sync();
    assert(!xiaotai_platform_background_deferred());
    assert(xiaotai_platform_request(&req, handler, NULL) == XIAOTAI_OK);
    wait_idle(2);
    assert(atomic_load(&calls) == 2 && results[2].result == XIAOTAI_OK);
}

static void wait_post_failure(bool reply)
{
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000LL;
    for (;;) {
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        if ((reply ? stats.reply_post_retries : stats.event_post_retries) != 0) break;
        assert(xiaotai_time_us() < deadline);
        xiaotai_delay_ms(1);
    }
}

static void pressure(bool reply)
{
    boot(NULL); discover();
    assert(xiaotai_run(full_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    if (reply) assert(send("/first") == XIAOTAI_OK);
    else {
        platform_profile_exchange(true);
        platform_profile_exchange(false);
    }
    wait_post_failure(reply);
    if (reply) {
        assert(!atomic_load(&replies) && status().exchanges == 1);
        atomic_fetch_add(&generation, 1);
    } else {
        /* A new accepted exchange means the old zero transition, latched
         * while the queue was full, must never reopen the listener. */
        platform_profile_exchange(true);
    }
    xiaotai_signal_give(&resume);
    if (reply) {
        wait_idle(1);
        assert(atomic_load(&replies) == 1 && results[0].result == XIAOTAI_ERR_PLATFORM_STALE && !results[0].body);
    } else {
        const int64_t deadline = xiaotai_time_us() + WAIT * 1000LL;
        while (xiaotai_run(hook, &done) == XIAOTAI_ERR_NO_MEM) {
            assert(xiaotai_time_us() < deadline);
            xiaotai_delay_ms(1);
        }
        assert(xiaotai_signal_take(&done, WAIT));
        assert(!atomic_load(&idle_events) && status().exchanges == 1);
        platform_profile_exchange(false);
        wait_idle(1);
        assert(atomic_load(&idle_events) == 1);
    }
}

static void ai_token_pressure(uint16_t status_code)
{
    const char *const expected = status_code == 200U ? "{\"code\":200}" : ai_reply_body;
    const size_t size = strlen(expected);
    ai_reply_pressure_status = status_code;
    memcpy(ai_reply_scratch, expected, size + 1U);
    boot(NULL);
    discover();
    set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
    assert(xiaotai_run(full_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_request_t req = make_request("/v1/ai/token");
    req.auth = XIAOTAI_PLATFORM_AUTH_SESSION;
    req.timeout_ms = 15000U;
    req.flags = XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE;
    assert(xiaotai_platform_request(&req, handler, ai_reply_scratch) == XIAOTAI_OK);
    wait_post_failure(true);
    assert(atomic_load(&calls) == 1 && !atomic_load(&replies) && status().exchanges == 1);
    /* The HTTP client is done and the owner cannot yet dispatch. Overwrite
     * the peer fixture's original bytes, not the worker's retained reply. */
    memset(ai_reply_scratch, 'x', sizeof(ai_reply_scratch));
    atomic_fetch_add(&transfer_clock_offset, INT64_C(1500000));
    assert(!atomic_load(&replies) && status().exchanges == 1);
    xiaotai_signal_give(&resume);
    wait_idle(1);
    assert(atomic_load(&replies) == 1 && atomic_load(&calls) == 1);
    assert(results[0].result == XIAOTAI_OK && results[0].http_status == status_code && results[0].code == 200);
    assert(results[0].size == size && !strcmp(bodies[0], expected));
    assert(!atomic_load(&hook_auth) && !atomic_load(&hook_unbound));
    printf("AI HTTP %u: retained %zu bytes delivered once after 1500 ms of owner backpressure\n",
           (unsigned)status_code, size);
}

static xiaotai_err_t read_stream(void *ctx, uint8_t *data, size_t capacity, size_t *size)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && capacity && read_bytes < 5);
    *size = read_bytes < 2 ? 1U : 3U;
    assert(*size <= capacity);
    memcpy(data, "bytes" + read_bytes, *size);
    read_bytes += (unsigned)*size;
    return XIAOTAI_OK;
}

static xiaotai_err_t begin_stream(void *ctx, const xiaotai_platform_response_head_t *head)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && head->http_status == 200 && head->content_length == 6);
    assert(!strcmp(head->content_type, "fixture/stream") && head->content_type_repeated);
    assert(!strcmp(head->content_encoding, "identity") && !head->content_encoding_repeated && !head->truncated);
    atomic_store(&verify_transfer, true);
    if (atomic_load(&hold_begin)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t write_stream(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && atomic_load(&verify_transfer) && size <= 6 - wrote_bytes);
    assert(!memcmp(data, "answer" + wrote_bytes, size));
    wrote_bytes += (unsigned)size;
    return XIAOTAI_OK;
}

static void done_stream(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    (void)ctx;
    assert(xiaotai_on_owner() && xiaotai_platform_exchanges_busy());
    transfer_result = *result;
    atomic_fetch_add(&replies, 1);
}

static void transfers(void)
{
    boot(NULL); discover();
    const xiaotai_platform_stream_t stream = {.read = read_stream, .begin = begin_stream, .write = write_stream, .done = done_stream};
    xiaotai_platform_transfer_t req = {.request = make_request("/transfer"), .content_type = "fixture/upload", .body_size = 5};
    req.request.method = XIAOTAI_PLATFORM_POST;
    uint32_t id = 0;
    assert(xiaotai_platform_transfer(&req, &stream, &id) == XIAOTAI_OK && id);
    wait_idle(1);
    assert(transfer_result.result == XIAOTAI_OK && transfer_result.sent && transfer_result.received == 6 && wrote_bytes == 6 && read_bytes == 5);
    req.body_size = 0; req.request.method = XIAOTAI_PLATFORM_GET;
    wrote_bytes = 0;
    atomic_store(&hold_begin, true);
    assert(xiaotai_platform_transfer(&req, &stream, &id) == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_transfer_cancel(id);
    xiaotai_signal_give(&resume);
    wait_idle(2);
    assert(atomic_load(&replies) == 2 && transfer_result.result == XIAOTAI_ERR_PLATFORM_CANCELLED && transfer_result.sent && !wrote_bytes);
}

typedef struct {
    const uint8_t *expected;
    size_t expected_size, written, read;
    unsigned begins, writes, reads;
    atomic_uint done;
    int http_status;
    int64_t length;
    bool hold_write, short_body;
    xiaotai_err_t read_error, begin_error, write_error;
    xiaotai_platform_transfer_result_t result;
} p5_transfer_t;

static p5_transfer_t p5_fixture(void)
{
    return (p5_transfer_t){.expected = transfer_payload, .expected_size = sizeof(transfer_payload),
                           .http_status = 200, .length = TRANSFER_SIZE};
}

static xiaotai_err_t p5_read(void *ctx, uint8_t *out, size_t capacity, size_t *size)
{
    p5_transfer_t *p = ctx;
    assert(!xiaotai_on_owner() && !atomic_load(&p->done) && !p->begins &&
           p->read < TRANSFER_SIZE && capacity <= 1024U && capacity != 0);
    ++p->reads;
    if (p->read_error != XIAOTAI_OK) return p->read_error;
    if (p->short_body) { *size = 0; return XIAOTAI_OK; }
    *size = capacity < 370U ? capacity : 370U;
    memcpy(out, transfer_payload + p->read, *size);
    p->read += *size;
    return XIAOTAI_OK;
}

static xiaotai_err_t p5_begin(void *ctx, const xiaotai_platform_response_head_t *head)
{
    p5_transfer_t *p = ctx;
    assert(!xiaotai_on_owner() && !atomic_load(&p->done) && !p->begins++ && !p->writes);
    assert(head->http_status == p->http_status && head->content_length == p->length);
    assert(!strcmp(head->content_type, "audio/wav") && head->content_type_repeated);
    assert(head->content_encoding_repeated && head->truncated &&
           strlen(head->content_encoding) == XIAOTAI_PLATFORM_CONTENT_TYPE_MAX - 1U);
    for (const char *c = head->content_encoding; *c; ++c) assert(*c == 'a');
    return p->begin_error;
}

static xiaotai_err_t p5_write(void *ctx, const uint8_t *data, size_t size)
{
    p5_transfer_t *p = ctx;
    assert(!xiaotai_on_owner() && !atomic_load(&p->done) && p->begins == 1U);
    assert(size && size <= 257U && size <= p->expected_size - p->written);
    assert(!memcmp(data, p->expected + p->written, size));
    p->written += size;
    if (atomic_load(&trace_transfer_ticks)) atomic_fetch_add(&transfer_clock_offset, 120000);
    if (++p->writes == 1U && p->hold_write) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&resume, WAIT));
    }
    return p->write_error;
}

static void p5_done(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    p5_transfer_t *p = ctx;
    assert(xiaotai_on_owner() && xiaotai_platform_exchanges_busy());
    p->result = *result;
    assert(atomic_fetch_add(&p->done, 1U) == 0);
}

static xiaotai_platform_stream_t p5_stream(p5_transfer_t *p)
{
    return (xiaotai_platform_stream_t){.read = p5_read, .begin = p5_begin, .write = p5_write,
                                      .done = p5_done, .ctx = p};
}

static uint32_t p5_start(const xiaotai_platform_transfer_t *request, p5_transfer_t *p)
{
    xiaotai_platform_stream_t stream = p5_stream(p);
    uint32_t id;
    assert(xiaotai_platform_transfer(request, &stream, &id) == XIAOTAI_OK && id);
    memset(&stream, 0, sizeof(stream)); /* The caller's callbacks were copied. */
    return id;
}

static void p5_check(p5_transfer_t *p, xiaotai_err_t error, int http, bool sent)
{
    assert(atomic_load(&p->done) == 1U && p->result.generation != 0);
    assert(p->result.result == error && p->result.http_status == http && p->result.sent == sent);
    assert(p->result.received == p->written);
}

static void p5_upload_case(void)
{
    hold_first = true;
    boot(&(xiaotai_platform_config_t){.idle_keep_ms = 1000U, .get_retry_on_reuse = true});
    discover();
    assert(send("/first") == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    char path[] = "/p5/upload", type[] = "fixture/audio";
    xiaotai_platform_transfer_t req = {.request = make_request(path), .content_type = type, .body_size = TRANSFER_SIZE};
    req.request.method = XIAOTAI_PLATFORM_POST;
    p5_transfer_t p = p5_fixture();
    const uint32_t id = p5_start(&req, &p);
    path[4] = 'X'; type[0] = 'X';
    xiaotai_signal_give(&resume);
    wait_idle(1);
    p5_check(&p, XIAOTAI_OK, 200, true);
    assert(p.read == TRANSFER_SIZE && p.reads > 2U && p.writes > 2U &&
           p.written == TRANSFER_SIZE && atomic_load(&calls) == 2U);
    xiaotai_platform_transfer_cancel(id); /* Finished has no effect. */

    req.request.path = "/p5/upload"; req.content_type = "fixture/audio";
    p = p5_fixture(); p.read_error = XIAOTAI_ERR_BUSY;
    (void)p5_start(&req, &p);
    wait_idle(2);
    p5_check(&p, XIAOTAI_ERR_BUSY, 0, true);
    assert(p.reads == 1U && !p.begins && !p.writes && atomic_load(&calls) == 3U);

    p = p5_fixture(); p.short_body = true;
    (void)p5_start(&req, &p);
    wait_idle(3);
    p5_check(&p, XIAOTAI_FAIL, 0, true);
    assert(p.reads == 1U && !p.begins && !p.writes && atomic_load(&calls) == 4U);

    req.request.path = "p5/upload"; /* Missing leading slash. */
    uint32_t rejected = 99;
    const xiaotai_platform_stream_t stream = p5_stream(&p);
    assert(xiaotai_platform_transfer(&req, &stream, &rejected) == XIAOTAI_ERR_INVALID_ARG && !rejected);
    req.request.path = "/p5/upload";
    req.request.timeout_ms = XIAOTAI_PLATFORM_TRANSFER_TIMEOUT_MAX_MS + 1U;
    assert(xiaotai_platform_transfer(&req, &stream, &rejected) == XIAOTAI_ERR_INVALID_ARG && !rejected);
    assert(atomic_load(&calls) == 4U && atomic_load(&p.done) == 1U);
}

static void p5_download_case(void)
{
    boot(&(xiaotai_platform_config_t){.get_retry_on_reuse = true, .idle_keep_ms = 1000U});
    discover();
    set_session(XIAOTAI_PLATFORM_NO_IDENTITY, false, false);
    xiaotai_platform_transfer_t req = {.request = make_request("/p5/bearer?clip=one%2Btwo&take=1")};
    req.request.auth = XIAOTAI_PLATFORM_AUTH_BEARER;
    req.request.bearer = "fixture-bearer";
    p5_transfer_t p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(1);
    p5_check(&p, XIAOTAI_OK, 200, true);
    assert(p.written == TRANSFER_SIZE && p.begins == 1U && p.writes > 2U && !p.reads && !platform.bearer);

    req = (xiaotai_platform_transfer_t){.request = make_request("/p5/reused")};
    p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(2);
    p5_check(&p, XIAOTAI_FAIL, 0, true);
    assert(!p.begins && !p.writes && atomic_load(&calls) == 2U); /* Streaming GET is not replayed. */

    req.request.path = "/p5/known"; req.response_max = 600U;
    p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(3);
    p5_check(&p, XIAOTAI_ERR_INVALID_SIZE, 200, true);
    assert(p.begins == 1U && !p.writes);

    req.request.path = "/p5/unknown";
    p = p5_fixture(); p.length = -1;
    (void)p5_start(&req, &p);
    wait_idle(4);
    p5_check(&p, XIAOTAI_ERR_INVALID_SIZE, 200, true);
    assert(p.begins == 1U && p.writes == 2U && p.written == 514U);

    req.request.path = "/p5/empty"; req.response_max = 0;
    p = p5_fixture(); p.http_status = 204; p.length = 0;
    (void)p5_start(&req, &p);
    wait_idle(5);
    p5_check(&p, XIAOTAI_OK, 204, true);
    assert(p.begins == 1U && !p.writes);

    req.request.path = "/p5/known";
    p = p5_fixture(); p.begin_error = XIAOTAI_ERR_NOT_SUPPORTED;
    (void)p5_start(&req, &p);
    wait_idle(6);
    p5_check(&p, XIAOTAI_ERR_NOT_SUPPORTED, 200, true);
    assert(p.begins == 1U && !p.writes);

    p = p5_fixture(); p.write_error = XIAOTAI_ERR_NO_MEM;
    (void)p5_start(&req, &p);
    wait_idle(7);
    p5_check(&p, XIAOTAI_ERR_NO_MEM, 200, true);
    assert(p.begins == 1U && p.writes == 1U && p.written == 257U);
    assert(p.result.receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_NONE);

    req.request.path = "/p5/unsent";
    p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(8);
    p5_check(&p, XIAOTAI_FAIL, 0, false);
    assert(!p.begins && !p.writes);
    assert(p.result.receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_NONE);

    req.request.path = "/p5/slow"; req.stall_timeout_ms = 20U;
    p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(9);
    p5_check(&p, XIAOTAI_ERR_TIMEOUT, 200, true);
    assert(!p.writes && atomic_load(&calls) == 9U);
    assert(p.result.receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY);
}

static void p5_cancel_case(void)
{
    hold_first = true;
    boot(NULL); discover();
    assert(send("/first") == XIAOTAI_OK && xiaotai_signal_take(&entered, WAIT));
    const xiaotai_platform_transfer_t req = {.request = make_request("/p5/known")};
    p5_transfer_t p = p5_fixture();
    const uint32_t queued = p5_start(&req, &p);
    xiaotai_platform_transfer_cancel(queued);
    xiaotai_signal_give(&resume);
    wait_idle(1);
    p5_check(&p, XIAOTAI_ERR_PLATFORM_CANCELLED, 0, false);
    assert(!p.begins && !p.writes && atomic_load(&calls) == 1U);

    p = p5_fixture(); p.hold_write = true;
    const uint32_t running = p5_start(&req, &p);
    assert(running != queued && xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_transfer_cancel(queued); /* Wrong generation of slot. */
    xiaotai_platform_transfer_cancel(running);
    xiaotai_platform_transfer_cancel(running);
    assert(!atomic_load(&p.done) && xiaotai_platform_exchanges_busy());
    xiaotai_signal_give(&resume);
    wait_idle(2);
    p5_check(&p, XIAOTAI_ERR_PLATFORM_CANCELLED, 200, true);
    assert(p.writes == 1U && p.written == 257U && atomic_load(&calls) == 2U);

    p = p5_fixture(); p.hold_write = true;
    (void)p5_start(&req, &p);
    assert(xiaotai_signal_take(&entered, WAIT));
    atomic_fetch_add(&generation, 1U);
    xiaotai_signal_give(&resume);
    wait_idle(3);
    p5_check(&p, XIAOTAI_ERR_PLATFORM_STALE, 200, true);
    assert(p.writes == 1U && p.written == 257U && p.result.generation != atomic_load(&generation));
}

static void p5_start_race(void)
{
    boot(NULL); discover();
    const xiaotai_platform_transfer_t req = {.request = make_request("/p5/known")};
    p5_transfer_t p = p5_fixture();
    atomic_store(&hold_before_http, true);
    const uint32_t id = p5_start(&req, &p);
    assert(xiaotai_signal_take(&entered, WAIT));
    xiaotai_platform_transfer_cancel(id);
    assert(!atomic_load(&p.done) && !atomic_load(&calls));
    xiaotai_signal_give(&resume);
    wait_idle(1);
    /* The cancellation won before the port owned the client. It must not
     * resolve, connect or send, even though per-client cancel saw IDLE. */
    assert(!atomic_load(&calls));
    p5_check(&p, XIAOTAI_ERR_PLATFORM_CANCELLED, 0, false);
    assert(!p.begins && !p.writes);

    p = p5_fixture();
    (void)p5_start(&req, &p);
    wait_idle(2);
    p5_check(&p, XIAOTAI_OK, 200, true);
    assert(atomic_load(&calls) == 1U && p.written == TRANSFER_SIZE);
}

static void p5_lifecycle_case(bool heartbeat)
{
    boot(&(xiaotai_platform_config_t){.auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE});
    discover();
    p5_transfer_t p = p5_fixture();
    xiaotai_platform_transfer_t req = {.request = make_request(heartbeat ? "/p5/slow" : "/p5/unbound")};
    if (heartbeat) atomic_store(&trace_transfer_ticks, true);
    else {
        p.expected = (const uint8_t *)"{\"code\":6006}";
        p.expected_size = p.length = 13;
        p.http_status = 410;
    }
    (void)p5_start(&req, &p);
    wait_idle(1);
    p5_check(&p, XIAOTAI_OK, p.http_status, true);
    assert(p.written == p.expected_size);
    if (heartbeat) assert(atomic_load(&transfer_ticks) >= 3U); /* Real timer step inside the HTTP exchange. */
    else {
        assert(!atomic_load(&hook_unbound));
        req.request.path = "/unbound";
        assert(xiaotai_platform_request(&req.request, handler, NULL) == XIAOTAI_OK);
        wait_idle(2);
        assert(atomic_load(&hook_unbound) == 1U);

        set_session(XIAOTAI_PLATFORM_ONLINE, true, true);
        req.request.path = "/p5/unauth";
        req.request.auth = XIAOTAI_PLATFORM_AUTH_SESSION;
        p = p5_fixture(); p.http_status = 401;
        (void)p5_start(&req, &p);
        wait_idle(3);
        p5_check(&p, XIAOTAI_OK, 401, true);
        assert(atomic_load(&hook_auth) == 1U && atomic_load(&hook_unbound) == 1U);
    }
}

typedef struct {
    pthread_barrier_t *barrier;
    bool opening;
    uint32_t ticket;
    xiaotai_err_t result;
} gate_call_t;

static void *gate_call(void *ctx)
{
    gate_call_t *call = ctx;
    const int barrier = pthread_barrier_wait(call->barrier);
    assert(barrier == 0 || barrier == PTHREAD_BARRIER_SERIAL_THREAD);
    call->result = call->opening ? xiaotai_platform_gate_open(call->ticket) : xiaotai_platform_gate_close(&call->ticket);
    return NULL;
}

static uint32_t concurrent_gate_calls(uint32_t ticket)
{
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    gate_call_t ops[2];
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        ops[i] = (gate_call_t){.barrier = &barrier, .opening = ticket != 0, .ticket = ticket};
        assert(pthread_create(&threads[i], NULL, gate_call, &ops[i]) == 0);
    }
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(pthread_barrier_destroy(&barrier) == 0);

    const unsigned winner = ops[0].result == XIAOTAI_OK ? 0 : 1;
    assert(ops[winner].result == XIAOTAI_OK && ops[winner].ticket != 0);
    const gate_call_t *loser = &ops[1 - winner];
    assert(loser->result == (ticket ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_ERR_PLATFORM_GATED));
    assert(loser->ticket == ticket);
    return ops[winner].ticket;
}

static void provisioning_gate(void)
{
    gate_unavailable();
    provisioning = true;
    uint32_t previous = 0;
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        boot(NULL);
        assert(!platform.objects && !platform.slots && !atomic_load(&platform.running));
        assert(send("/before") == XIAOTAI_ERR_PLATFORM_OFFLINE && !status().gated);
        uint32_t active = 0, duplicate = UINT32_MAX;
        assert(xiaotai_platform_gate_close(&active) == XIAOTAI_OK && active && active != previous);
        assert(xiaotai_platform_gate_close(&duplicate) == XIAOTAI_ERR_PLATFORM_GATED && duplicate == UINT32_MAX);
        assert(!xiaotai_platform_gate_drained(0) && !xiaotai_platform_gate_drained(previous));
        assert(xiaotai_platform_gate_open(0) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_platform_gate_open(previous) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_platform_gate_drained(active) && status().gated);
        assert(send("/held") == XIAOTAI_ERR_PLATFORM_OFFLINE && !xiaotai_platform_exchanges_busy());
        if (cycle == 0) {
            /* Host reset discards a held ticket without allowing it to become valid in the next boot. */
            previous = active;
            xiaotai_platform_reset();
            xiaotai_framework_reset();
#if XIAOTAI_PLATFORM_RECONNECT_COMMAND
            xiaotai_eventlog_reset();
#endif
            gate_unavailable();
        } else {
            assert(xiaotai_platform_gate_open(active) == XIAOTAI_OK);
            assert(!xiaotai_platform_gate_drained(active) && !status().gated);
            assert(send("/after") == XIAOTAI_ERR_PLATFORM_OFFLINE && !xiaotai_platform_exchanges_busy());
            const uint32_t concurrent = concurrent_gate_calls(0);
            assert(concurrent != active && xiaotai_platform_gate_drained(concurrent));
            assert(xiaotai_platform_gate_open(active) == XIAOTAI_ERR_INVALID_ARG);
            assert(concurrent_gate_calls(concurrent) == concurrent);
            assert(!status().gated && !xiaotai_platform_gate_drained(concurrent));
        }
    }
}

int main(int argc, char **argv)
{
    const char *mode = argc == 2 ? argv[1] : "admission";
    for (size_t i = 0; i < sizeof(transfer_payload); ++i) transfer_payload[i] = (uint8_t)(i * 31U);
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    if (!strcmp(mode, "admission")) admission();
    else if (!strcmp(mode, "body_demand") || !strcmp(mode, "body_resident") || !strcmp(mode, "body_demand_legacy"))
        request_body_lease(!strcmp(mode, "body_resident"), !strcmp(mode, "body_demand_legacy"));
    else if (!strcmp(mode, "saturation_legacy") || !strcmp(mode, "saturation_default"))
        request_saturation(!strcmp(mode, "saturation_legacy"));
    else if (!strcmp(mode, "order")) ordering();
    else if (!strcmp(mode, "reply")) response();
    else if (!strcmp(mode, "stale")) stale();
    else if (!strcmp(mode, "admission_epoch")) admission_epoch();
    else if (!strcmp(mode, "connection") || !strcmp(mode, "no_retry")) { no_retry_get = !strcmp(mode, "no_retry"); connection(); }
    else if (!strcmp(mode, "queued_reuse")) queued_connection(false);
    else if (!strcmp(mode, "queued_release")) queued_connection(true);
    else if (!strcmp(mode, "legacy_http_no_delay")) legacy_http_no_delay();
    else if (!strcmp(mode, "transfer")) transfers();
    else if (!strcmp(mode, "transfer_upload")) p5_upload_case();
    else if (!strcmp(mode, "transfer_download")) p5_download_case();
    else if (!strcmp(mode, "transfer_cancel")) p5_cancel_case();
    else if (!strcmp(mode, "transfer_start_race")) p5_start_race();
    else if (!strcmp(mode, "transfer_lifecycle") || !strcmp(mode, "transfer_heartbeat"))
        p5_lifecycle_case(!strcmp(mode, "transfer_heartbeat"));
    else if (!strcmp(mode, "preflight")) preflight_case(false, false);
    else if (!strcmp(mode, "resume_first")) preflight_case(true, false);
    else if (!strcmp(mode, "timeout")) preflight_case(false, true);
    else if (!strcmp(mode, "owner_queue")) priority_and_queue();
    else if (!strcmp(mode, "background")) background();
    else if (!strcmp(mode, "reply_pressure")) pressure(true);
    else if (!strcmp(mode, "ai_reply_pressure")) ai_token_pressure(201);
    else if (!strcmp(mode, "ai_reply_pressure_200")) ai_token_pressure(200);
    else if (!strcmp(mode, "idle_pressure")) pressure(false);
    else if (!strcmp(mode, "stack_watermark")) stack_watermark();
    else if (!strcmp(mode, "provisioning")) provisioning_gate();
    else if (!strcmp(mode, "worker_unavailable")) {
        worker_create_fails = true;
        boot(NULL);
        assert(platform.objects && !platform.provisioning && !platform.slots && !atomic_load(&platform.running));
        assert(atomic_load(&platform.initialized) && atomic_load(&platform.started));
        assert(xiaotai_platform_start_failed() && status().last_error == XIAOTAI_ERR_NO_MEM);
        wait_stack_sample(0);
        assert(!atomic_load(&stack_samples) && !atomic_load(&task_lists));
        uint32_t ticket = 0, duplicate = UINT32_MAX;
        assert(xiaotai_platform_gate_close_binding_idle(&ticket) == XIAOTAI_OK && ticket);
        assert(xiaotai_platform_gate_close(&duplicate) == XIAOTAI_ERR_PLATFORM_GATED && duplicate == UINT32_MAX);
        assert(xiaotai_platform_gate_drained(ticket) && status().gated);
        assert(send("/held") == XIAOTAI_ERR_PLATFORM_OFFLINE && !xiaotai_platform_exchanges_busy());
        assert(xiaotai_platform_gate_open(ticket) == XIAOTAI_OK);
        assert(!xiaotai_platform_gate_drained(ticket) && !status().gated);
        assert(send("/offline") == XIAOTAI_ERR_PLATFORM_OFFLINE && !xiaotai_platform_exchanges_busy());
    } else assert(false);
    xiaotai_platform_reset();
    xiaotai_framework_reset();
#if XIAOTAI_PLATFORM_RECONNECT_COMMAND
    xiaotai_eventlog_reset();
#endif
    xiaotai_signal_deinit(&fence);
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&entered);
    xiaotai_signal_deinit(&done);
    printf("platform_request %s: ok\n", mode);
    return 0;
}
