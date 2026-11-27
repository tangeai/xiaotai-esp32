// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h5_local_stop.py
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
// Replaces legacy: tests/regression/test_environment_control.py
/* Cross-component ST1/P2 on a single-slot host product. P3 publishes
 * discovery/session inputs; HTTP, RTC and audio hardware are scripted. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/link.h"
#include "xiaotai/media.h"
#include "xiaotai/media_uplink.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/preferences.h"
#include "xiaotai_http_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_transport_host.h"

#define WAIT_MS 2000U

static xiaotai_signal_t owner_entered, owner_release, replies;
static xiaotai_signal_t barrier_entered, barrier_release, environment_finished;
static xiaotai_signal_t rtc_worker_paused, rtc_worker_release;
static atomic_uint requests, delivered, physical_closed, http_needs_closed;
static bool test_finish_held, test_unsolicited_stop, test_monitor_stop;
static bool test_requested_stop, test_next_request, test_pending_stop;
static bool test_stop_before_request, test_stale_stop, test_admission_before_error;
static bool test_inline_ack, test_stuck_ack;
static atomic_bool pause_rtc_worker, pause_rtc_evaluate, pause_rtc_fault;
static int stop_detail;
static unsigned store_attempts;
static xiaotai_environment_status_t final_environment;
static int environment_result;
static unsigned media_requirements, media_opened, media_closed;
static uint32_t media_generation;
static uintptr_t accept_during_close;
static uint32_t admitted_during_close, refused_during_close, open_during_close;
static bool pause_admission_on_stop;
static const xiaotai_network_t wifi = {.desc.kind = XIAOTAI_NETWORK_WIFI};

const xiaotai_network_t *xiaotai_hal_network(size_t i) { return i ? NULL : &wifi; }

/* The POSIX port keeps task handles resident after exit. These two workers
 * clear their engine handle on reset; keep only the retained host records
 * reachable, leaving leak detection active for all fixtures and resources. */
static xiaotai_task_t retained_handles[2];
int64_t __real_xt_now_ms(void);
void environment_barrier_wait_idle(void);

static bool pause_here(atomic_bool *armed)
{
    return atomic_load(armed) && xiaotai_task_is_current(&retained_handles[1]) &&
           atomic_exchange(armed, false);
}

static void paused(void)
{
    xiaotai_signal_give(&rtc_worker_paused);
    assert(xiaotai_signal_take(&rtc_worker_release, WAIT_MS));
}

int64_t __wrap_xt_now_ms(void)
{
    if (pause_here(&pause_rtc_worker)) paused();
    return __real_xt_now_ms();
}

xiaotai_err_t __real_xiaotai_platform_endpoint(const char *service, char *out, size_t capacity);
xiaotai_err_t __wrap_xiaotai_platform_endpoint(const char *service, char *out, size_t capacity)
{
    if (pause_here(&pause_rtc_evaluate)) paused();
    return __real_xiaotai_platform_endpoint(service, out, capacity);
}

void __real_xiaotai_critical_exit(xiaotai_critical_t *lock);
void __wrap_xiaotai_critical_exit(xiaotai_critical_t *lock)
{
    const bool after_fault = lock == &xt.lock && xt.environment_error != XIAOTAI_OK && pause_here(&pause_rtc_fault);
    __real_xiaotai_critical_exit(lock);
    if (after_fault) paused();
}
xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    const xiaotai_err_t result = __real_xiaotai_task_create(config, task);
    if (result == XIAOTAI_OK && !strcmp(config->name, "xiaotai_platform")) retained_handles[0] = *task;
    if (result == XIAOTAI_OK && !strcmp(config->name, "xt_transport")) retained_handles[1] = *task;
    return result;
}

/* Profile reporting is outside this scenario. Neither preflight nor an
 * ordinary stream start may use the optional signaling hold policy. */
xiaotai_err_t __wrap_xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t write, void *ctx)
{
    assert(!strcmp(scene, "stream") && write && !ctx);
    return XIAOTAI_OK;
}

size_t __wrap_xiaotai_platform_scene_parts(const char *activity, const char *scene,
                                         char *out, size_t capacity, size_t *length)
{
    (void)activity; (void)scene; (void)out; (void)capacity; (void)length;
    assert(false);
    return 0;
}

xiaotai_err_t __wrap_xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    (void)suspended; (void)ctx; (void)hold;
    assert(false);
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

void __wrap_xiaotai_platform_signaling_release(uint32_t hold) { (void)hold; assert(false); }

/* Audio hardware: record only the generation-specific open/close. Nothing
 * here can reserve a slot or post an admission event. */
xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *need)
{
    assert(need && need->downlink_burst_ms);
    ++media_requirements;
    return XIAOTAI_OK;
}

bool xiaotai_media_can_send(const xiaotai_media_format_t *format)
{
    assert(format->encoding == XIAOTAI_CODEC_G711A && format->rate_hz == 8000);
    return true;
}

bool xiaotai_media_can_receive(const xiaotai_media_format_t *format)
{
    assert(format->encoding == XIAOTAI_CODEC_OPUS);
    return false;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session)
{
    assert(xiaotai_on_owner() && !media_generation && session->generation && session->sink == xiaotai_link_audio_sink());
    media_generation = session->generation;
    ++media_opened;
    return XIAOTAI_OK;
}

void xiaotai_media_close(uint32_t generation)
{
    assert(xiaotai_on_owner() && generation == media_generation);
    if (accept_during_close) {
        xiaotai_transport_status_t before, after;
        xiaotai_transport_status(&before);
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
        const uintptr_t peer = accept_during_close;
        accept_during_close = 0;
        xiaotai_transport_host_accepted(peer);
        xiaotai_transport_status(&after);
        admitted_during_close = after.inbound_admitted - before.inbound_admitted;
        refused_during_close = after.inbound_refused - before.inbound_refused;
        open_during_close = after.links_open;
        printf("during media_close: admitted_delta=%u refused_delta=%u links_open=%u\n",
               admitted_during_close, refused_during_close, open_during_close);
    }
    ++media_closed;
    media_generation = 0;
}

bool xiaotai_media_push(uint32_t generation, const xiaotai_media_packet_t *packet)
{
    (void)generation;
    (void)packet;
    assert(false);
    return false;
}

/* This fixture exercises external-RAM product topology without real audio. */
xiaotai_err_t xiaotai_media_uplink_attach(const xiaotai_media_uplink_executor_t *executor)
{
    (void)executor;
    assert(false);
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

void xiaotai_media_uplink_detach(const xiaotai_media_uplink_executor_t *executor)
{
    (void)executor;
    assert(false);
}

xiaotai_media_uplink_work_t xiaotai_media_uplink_step(const xiaotai_media_uplink_executor_t *executor)
{
    (void)executor;
    assert(false);
    return XIAOTAI_MEDIA_UPLINK_IDLE;
}

static const char *const platform_deps[] = {"net", "identity", NULL};
static const char *const transport_deps[] = {"platform", NULL};
static const char *const stream_deps[] = {"transport", NULL};
static const xiaotai_module_t net_module = {.name = "net", .init = xiaotai_engine_net_init,
    .start = xiaotai_engine_net_start};
static const xiaotai_module_t identity_module = {.name = "identity", .init = xiaotai_engine_identity_init};
static const xiaotai_module_t platform_module = {.name = "platform", .depends = platform_deps,
    .init = xiaotai_engine_platform_init, .start = xiaotai_engine_platform_start};
static const xiaotai_module_t transport_module = {.name = "transport", .depends = transport_deps,
    .init = xiaotai_engine_transport_init, .start = xiaotai_engine_transport_start};
static const xiaotai_module_t stream_module = {.name = "stream", .depends = stream_deps,
    .init = xiaotai_module_stream_init, .start = xiaotai_module_stream_start};
static bool fail_environment_gate;

xiaotai_err_t __real_xiaotai_preferences_maintenance_flush(uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_preferences_maintenance_flush(uint32_t timeout_ms)
{
    assert(test_finish_held && !xiaotai_on_owner());
    xiaotai_signal_give(&barrier_entered);
    assert(xiaotai_signal_take(&barrier_release, WAIT_MS));
    return __real_xiaotai_preferences_maintenance_flush(timeout_ms);
}

xiaotai_err_t __wrap_xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done, void *ctx)
{
    assert(test_finish_held && xiaotai_on_owner() && !strcmp(base, "http://matrix.invalid") && done && ctx);
    ++store_attempts;
    /* Record the illegal store attempt without writing an environment. */
    return XIAOTAI_ERR_NO_MEM;
}

static void environment_finished_fn(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && event->size == sizeof(final_environment));
    environment_result = event->args.value;
    memcpy(&final_environment, event->data, event->size);
    xiaotai_signal_give(&environment_finished);
}

static xiaotai_err_t environment_init(void)
{
    xiaotai_err_t err = xiaotai_module_environment_init();
    if (err == XIAOTAI_OK)
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_ENVIRONMENT_RESULT, environment_finished_fn, NULL);
    return err;
}

static const char *const environment_deps[] = {"platform", "stream", NULL};
static const xiaotai_module_t environment_module = {.name = "environment", .depends = environment_deps,
    .init = environment_init, .start = xiaotai_module_environment_start};

static xiaotai_err_t later_gate(const char *reason, void *ctx)
{
    (void)ctx;
    if (fail_environment_gate && !strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT)) return XIAOTAI_ERR_BUSY;
    return XIAOTAI_OK;
}

static void later_release(const char *reason, void *ctx)
{
    (void)reason;
    (void)ctx;
    assert(!fail_environment_gate);
}

static xiaotai_err_t later_init(void)
{
    static const xiaotai_quiesce_gate_t gate = {.acquire = later_gate, .release = later_release};
    return xiaotai_quiesce_gate_register("later", &gate, NULL);
}

static const char *const later_deps[] = {"stream", NULL};
static const xiaotai_module_t later_module = {.name = "later", .depends = later_deps, .init = later_init};

static void notify(void *ctx) { xiaotai_signal_give(ctx); }

static void owner_sync(void)
{
    xiaotai_signal_t done;
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    assert(xiaotai_run(notify, &done) == XIAOTAI_OK && xiaotai_signal_take(&done, WAIT_MS));
    xiaotai_signal_deinit(&done);
}

static void settle(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
        owner_sync();
        assert(xiaotai_transport_host_settle(WAIT_MS));
    }
    owner_sync();
}

static void block(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&owner_entered);
    assert(xiaotai_signal_take(&owner_release, WAIT_MS));
}

static void block_owner(void)
{
    assert(xiaotai_run(block, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_entered, WAIT_MS));
}

static xiaotai_stream_status_t stream_status(void)
{
    xiaotai_stream_status_t result;
    xiaotai_stream_status(&result);
    return result;
}

static xiaotai_transport_status_t transport_status(void)
{
    xiaotai_transport_status_t result;
    xiaotai_transport_status(&result);
    return result;
}

static void answer_http(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    assert(!xiaotai_on_owner() && xiaotai_platform_exchanges_busy());
    const unsigned needed = atomic_load(&http_needs_closed);
    assert(!needed || (atomic_load(&physical_closed) >= needed && !transport_status().links_releasing));
    assert(!strcmp(request->url, "http://service.invalid/probe") && request->method == XIAOTAI_HTTP_GET);
    static const uint8_t body[] = "{\"code\":0}";
    reply->status = 200;
    reply->body = body;
    reply->body_size = sizeof(body) - 1U;
    atomic_fetch_add(&requests, 1);
}

static void reply_http(const xiaotai_platform_reply_t *reply, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner() && reply->result == XIAOTAI_OK && reply->http_status == 200 && reply->code == 0);
    atomic_fetch_add(&delivered, 1);
    xiaotai_signal_give(&replies);
}

static void request_http(void)
{
    const xiaotai_platform_request_t request = {.service = "fixture", .method = XIAOTAI_PLATFORM_GET,
        .path = "/probe", .auth = XIAOTAI_PLATFORM_AUTH_NONE, .timeout_ms = 5000};
    assert(xiaotai_platform_request(&request, reply_http, NULL) == XIAOTAI_OK);
}

static void wait_http(unsigned n)
{
    assert(xiaotai_signal_take(&replies, WAIT_MS));
    settle();
    assert(atomic_load(&requests) == n && atomic_load(&delivered) == n && !xiaotai_platform_exchanges_busy());
}

static void wait_preflight_deferred(void)
{
    const int64_t end = xiaotai_time_us() + WAIT_MS * 1000;
    do {
        platform_lock();
        const bool waiting = platform.preflight_deferred;
        platform_unlock();
        if (waiting) return;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < end);
    assert(false);
}

static void disconnected(uintptr_t conn, unsigned wanted)
{
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == wanted);
    xiaotai_transport_host_call_t records[64];
    const size_t count = xiaotai_transport_host_calls(records, 64);
    for (size_t i = count; i > 0; --i) {
        if (records[i - 1].kind == XIAOTAI_TRANSPORT_HOST_DISCONNECT) {
            assert(records[i - 1].value == (int64_t)conn);
            return;
        }
    }
    assert(false);
}

static void close_from_peer(uintptr_t conn)
{
    atomic_fetch_add(&physical_closed, 1);
    xiaotai_transport_host_closed(conn);
}

static void prepare(unsigned slots, bool preserve_inbound, bool admit_before_profile)
{
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_kv_set_str("tirtc_cfg", "device_id", "preflight-fixture-device", WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str("tirtc_cfg", "secret", "preflight-fixture-key", WAIT_MS) == XIAOTAI_OK);
    const xiaotai_net_credentials_t wifi_credentials = {.ssid = "fixture-wifi"};
    assert(xiaotai_kv_set("preflight_net", "wifi", &wifi_credentials, sizeof(wifi_credentials), WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_net_configure(&(xiaotai_net_config_t){.layout = XIAOTAI_NET_STORE_PLAIN,
        .space = "preflight_net"}) == XIAOTAI_OK);
    assert(!xiaotai_mem_has_external());
    assert(xiaotai_transport_configure(&(xiaotai_transport_config_t){.links = slots,
        .media_uplink_worker = XIAOTAI_TRANSPORT_OFF,
        .fail_environment_on_unsolicited_stop = test_monitor_stop,
        .unsolicited_stop = test_finish_held ? XIAOTAI_TRANSPORT_RECOVER_FAULT : XIAOTAI_TRANSPORT_RECOVER_RETRY})
        == XIAOTAI_OK);
    /* Color products' external-RAM AUTO does not install the HTTP idle OPEN guard. */
    if (slots == 2 || preserve_inbound)
        assert(xiaotai_stream_configure(&(xiaotai_stream_config_t){
            .platform_idle = XIAOTAI_STREAM_PLATFORM_IDLE_IGNORE,
            .admit_before_profile = admit_before_profile,
            .pause_admission_on_local_stop = pause_admission_on_stop,
            .preserve_inbound_on_environment = preserve_inbound}) == XIAOTAI_OK);
    xiaotai_transport_host_auto_started(true);
    xiaotai_transport_host_auto_stopped(true);
    xiaotai_http_host_set_responder(answer_http, NULL, false);
    assert(xiaotai_signal_init(&owner_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&owner_release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&replies) == XIAOTAI_OK);
    static const xiaotai_module_t *const modules[] = {
        &net_module, &identity_module, &platform_module, &transport_module, &stream_module, &later_module,
        &environment_module,
    };
    static const xiaotai_priority_t priorities[] = {{"stream", 1}, {NULL, 0}};
    const xiaotai_framework_config_t config = {.modules = modules,
        .module_count = test_finish_held ? 7 : preserve_inbound ? 6 : 5,
        .priorities = priorities, .available_needs = UINT32_MAX};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    settle();
    assert(xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID && media_requirements == 2);
    const platform_service_t services[] = {
        {XIAOTAI_PLATFORM_SERVICE_TIRTC, "http://rtc.invalid"}, {"fixture", "http://service.invalid"},
    };
    assert(platform_services_replace(services, 2) == XIAOTAI_OK);
    platform_session_publish(&(xiaotai_platform_status_t){.phase = XIAOTAI_PLATFORM_ONLINE,
        .authorized = true, .signaling = true, .profile_ready = true}, "preflight-session-fixture");

    xiaotai_net_host_record_t calls[8];
    const size_t n = xiaotai_net_host_records(calls, 8);
    uint32_t token = 0;
    for (size_t i = 0; i < n; ++i) if (calls[i].call == XIAOTAI_NET_HOST_JOIN) token = calls[i].token;
    assert(token);
    const xiaotai_net_driver_event_t joined = {.kind = XIAOTAI_NET_DRIVER_JOINED,
        .token = token, .ssid = "fixture-wifi"};
    const xiaotai_net_driver_event_t address = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = 1};
    assert(xiaotai_net_host_emit(&joined) == XIAOTAI_OK && xiaotai_net_host_emit(&address) == XIAOTAI_OK);
    settle();
    assert(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    assert(transport_status().links_max == slots && stream_status().phase == XIAOTAI_STREAM_LISTENING);
}

static bool replacement_race(const char *mode, bool stop_first, bool concurrent, bool error_first)
{
    const uintptr_t a = 401, b = 402;
    xiaotai_transport_host_accepted(a);
    settle();
    const uint32_t old = stream_status().generation;
    assert(old && media_generation == old && media_opened == 1 && !media_closed);

    block_owner();
    if (stop_first) assert(xiaotai_stream_stop(old) == XIAOTAI_OK);
    if (concurrent) {
        xiaotai_transport_host_accepted(b);
        assert(xiaotai_transport_host_settle(WAIT_MS));
        assert(transport_status().links_open == 2);
    }
    if (error_first) {
        xiaotai_transport_host_error(a, XIAOTAI_LINK_END_NETWORK, -1);
        assert(xiaotai_transport_host_settle(WAIT_MS));
        disconnected(a, 1);
    }
    close_from_peer(a);
    if (!concurrent) xiaotai_transport_host_accepted(b);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(transport_status().inbound_admitted == 2 && transport_status().links_open == 1);
    xiaotai_link_stats_t retired;
    assert(xiaotai_link_stats(old, &retired) && retired.state == XIAOTAI_LINK_STATE_NONE);
    assert(!transport_status().links_releasing);
    assert(stream_status().generation == old && media_generation == old);
    if (!stop_first) assert(xiaotai_stream_stop(old) == XIAOTAI_OK);
    xiaotai_signal_give(&owner_release);
    settle();

    const uint32_t fresh = stream_status().generation;
    const size_t disconnects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    const size_t old_disconnects = error_first ? 1U : 0U;
    printf("%s: old=%u replacement=%u media_opened=%u media_closed=%u disconnects=%zu links_open=%u\n",
           mode, old, fresh, media_opened, media_closed, disconnects, transport_status().links_open);
    if (disconnects > old_disconnects) disconnected(b, (unsigned)old_disconnects + 1U);
    const bool kept = fresh && fresh != old && media_generation == fresh &&
        media_opened == 2 && media_closed == 1 && disconnects == old_disconnects;
    if (kept) {
        assert(stream_status().last_end == (error_first ? XIAOTAI_STREAM_END_LINK :
            stop_first ? XIAOTAI_STREAM_END_LOCAL : XIAOTAI_STREAM_END_PEER));
        assert(xiaotai_stream_stop(old) == XIAOTAI_OK);
        settle();
        assert(stream_status().generation == fresh && media_closed == 1 &&
               xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == old_disconnects);
    }
    if (concurrent) assert(!fresh && disconnects == 1 && !kept);
    close_from_peer(b);
    settle();
    assert(!stream_status().generation && !transport_status().links_releasing);
    assert(media_closed == (kept ? 2U : 1U));
    return kept;
}

static void genuine_busy(void)
{
    const uintptr_t a = 401, b = 402;
    xiaotai_transport_host_accepted(a);
    settle();
    const uint32_t old = stream_status().generation;
    assert(old && media_generation == old && media_opened == 1 && !media_closed);
    block_owner();
    xiaotai_transport_host_accepted(b);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    xiaotai_link_stats_t live;
    assert(transport_status().inbound_admitted == 2 && transport_status().links_open == 2);
    assert(xiaotai_link_stats(old, &live) && live.state == XIAOTAI_LINK_STATE_OPEN);
    xiaotai_signal_give(&owner_release);
    settle();
    disconnected(b, 1);
    assert(stream_status().generation == old && stream_status().last_end == XIAOTAI_STREAM_END_BUSY);
    assert(media_generation == old && media_opened == 1 && !media_closed && transport_status().links_releasing == 1);
    close_from_peer(b);
    settle();
    assert(!transport_status().links_releasing);
    assert(xiaotai_stream_stop(old) == XIAOTAI_OK);
    settle();
    disconnected(a, 2);
    assert(!stream_status().generation && stream_status().last_end == XIAOTAI_STREAM_END_LOCAL);
    assert(media_closed == 1 && transport_status().links_releasing == 1);
    close_from_peer(a);
    settle();
    assert(!transport_status().links_releasing && stream_status().phase == XIAOTAI_STREAM_LISTENING);
    puts("busy: opens=2, older physically OPEN, newer alone refused; old local stop releases exactly once");
}

static void peer_error_snapshot(void)
{
    const uintptr_t a = 401, b = 402;
    xiaotai_transport_host_accepted(a);
    settle();
    const uint32_t old = stream_status().generation;
    assert(old && media_generation == old);
    block_owner();
    xiaotai_transport_host_error(a, XIAOTAI_LINK_END_PEER, -1);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    xiaotai_link_stats_t stale;
    assert(xiaotai_link_stats(old, &stale) && stale.state == XIAOTAI_LINK_STATE_NONE);
    assert(!transport_status().links_open && transport_status().links_releasing == 1);
    assert(stream_status().generation == old && !media_closed);
    disconnected(a, 1); /* Error is not the stack's physical close acknowledgement. */
    xiaotai_transport_host_accepted(b);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(transport_status().inbound_admitted == 2 && transport_status().links_open == 1);
    assert(xiaotai_link_stats(old, &stale) && stale.state == XIAOTAI_LINK_STATE_NONE);
    xiaotai_signal_give(&owner_release);
    settle();
    disconnected(b, 2);
    assert(!stream_status().generation && stream_status().last_end == XIAOTAI_STREAM_END_PEER);
    assert(media_opened == 1 && media_closed == 1 && transport_status().links_releasing == 2);
    close_from_peer(a);
    close_from_peer(b);
    settle();
    assert(!transport_status().links_releasing);
    puts("peer-error: exact stats NONE, PEER reason and second slot admission do not prove physical close");
}

static void hold_listener(void *ctx)
{
    assert(xiaotai_stream_hold("fixture", ctx) == XIAOTAI_OK);
}

static void release_listener(void *ctx)
{
    xiaotai_stream_release(*(uint32_t *)ctx);
}

static void listener_off(void)
{
    const uintptr_t a = 401, b = 402;
    uint32_t hold = 0;
    xiaotai_transport_host_accepted(a);
    settle();
    assert(stream_status().generation && media_opened == 1);
    block_owner();
    assert(xiaotai_run(hold_listener, &hold) == XIAOTAI_OK);
    close_from_peer(a);
    xiaotai_transport_host_accepted(b);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(transport_status().inbound_admitted == 2 && transport_status().links_open == 1);
    xiaotai_signal_give(&owner_release);
    settle();
    disconnected(b, 1);
    assert(hold && !stream_status().generation && stream_status().phase == XIAOTAI_STREAM_CLOSED);
    assert(media_opened == 1 && media_closed == 1 && transport_status().links_releasing == 1);
    close_from_peer(b);
    settle();
    assert(!transport_status().links_releasing);
    assert(xiaotai_run(release_listener, &hold) == XIAOTAI_OK);
    settle();
    assert(stream_status().phase == XIAOTAI_STREAM_LISTENING);
    puts("listener-off: tentative replacement revoked, no new media; physical close retires record");
}

struct other_activity {
    const char *name;
    uint32_t generation, id;
    unsigned revoked;
};

static void other_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id;
    (void)reason;
    ++((struct other_activity *)ctx)->revoked;
}

static void start_other(void *ctx)
{
    struct other_activity *other = ctx;
    assert(xiaotai_activity_begin(&(const xiaotai_activity_t){.name = other->name, .resources = XIAOTAI_RESOURCE_LINK,
        .generation = other->generation, .revoked = other_revoked, .ctx = other}, &other->id) == XIAOTAI_OK);
}

static void end_other(void *ctx)
{
    const struct other_activity *other = ctx;
    const char *name = NULL;
    assert(other->id && xiaotai_activity_current(&name) == other->id && !strcmp(name, other->name));
    assert(!other->revoked);
    xiaotai_activity_end(other->id);
}

static void local_stop_offline(void)
{
    assert(xiaotai_stream_stop(0) == XIAOTAI_ERR_INVALID_ARG);
    const uintptr_t first = 601, rejected = 602;
    xiaotai_transport_host_accepted(first);
    settle();
    const uint32_t gen = stream_status().generation;
    assert(gen && stream_status().phase == XIAOTAI_STREAM_ACTIVE && media_generation == gen);

    block_owner();
    platform_session_publish(&(xiaotai_platform_status_t){.phase = XIAOTAI_PLATFORM_CONNECTING,
        .authorized = true, .profile_ready = true}, "preflight-session-fixture");
    assert(xiaotai_stream_stop(gen) == XIAOTAI_OK);
    close_from_peer(first);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(!transport_status().links_open && stream_status().generation == gen && !media_closed);
    assert(xiaotai_stream_stop(gen) == XIAOTAI_OK);
    xiaotai_signal_give(&owner_release);
    settle();
    assert(!media_generation && media_closed == 1 && !stream_status().generation);
    assert(stream_status().last_end == XIAOTAI_STREAM_END_LOCAL);
    assert(stream_status().phase == XIAOTAI_STREAM_CLOSED &&
           (stream_status().closed & XIAOTAI_STREAM_CLOSED_OFFLINE));
    assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    xiaotai_transport_host_accepted(rejected);
    disconnected(rejected, 1);
    close_from_peer(rejected);
    settle();
    assert(media_opened == 1 && media_closed == 1 && !transport_status().links_releasing);

    platform_session_publish(&(xiaotai_platform_status_t){.phase = XIAOTAI_PLATFORM_ONLINE,
        .authorized = true, .signaling = true, .profile_ready = true}, "preflight-session-fixture");
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
    settle();
    assert(stream_status().phase == XIAOTAI_STREAM_LISTENING);

    static const char *const others[] = {"call", "ai", "room"};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        struct other_activity other = {.name = others[i], .generation = gen};
        xiaotai_stream_stats_t before, after;
        assert(xiaotai_run(start_other, &other) == XIAOTAI_OK);
        settle();
        xiaotai_stream_stats(&before);
        assert(xiaotai_stream_stop(gen) == XIAOTAI_OK);
        settle();
        xiaotai_stream_stats(&after);
        assert(after.stale_stops == before.stale_stops + 1);
        assert(media_closed == 1 && xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1);
        assert(stream_status().phase == XIAOTAI_STREAM_LISTENING);
        assert(xiaotai_run(end_other, &other) == XIAOTAI_OK);
        settle();
    }
    puts("local-stop-offline: exact old generation closes once, offline gate stays closed, and call/ai/room survive stale stop");
}

static bool local_stop_gate(void)
{
    const uintptr_t old = 601, replacement = 602;
    xiaotai_transport_host_accepted(old);
    settle();
    const uint32_t gen = stream_status().generation;
    assert(gen && media_generation == gen && transport_status().links_open == 1);
    accept_during_close = replacement;
    assert(xiaotai_stream_stop(gen) == XIAOTAI_OK);
    settle();
    const uint32_t current = stream_status().generation;
    printf("after local stop: old=%u current=%u media_opened=%u media_closed=%u\n",
           gen, current, media_opened, media_closed);
    assert(!accept_during_close && current != gen && media_closed == 1);
    const bool blocked = admitted_during_close == 0 && refused_during_close == 1 &&
                         open_during_close == 1 && !current && media_opened == 1;
    if (!blocked) assert(admitted_during_close == 1 && !refused_during_close && open_during_close == 2 &&
                         current && media_opened == 2);
    close_from_peer(old);
    close_from_peer(replacement);
    settle();
    assert(!stream_status().generation && !transport_status().links_releasing);
    const uintptr_t next = 603;
    const unsigned seen = media_opened;
    xiaotai_transport_host_accepted(next);
    settle();
    assert(stream_status().phase == XIAOTAI_STREAM_ACTIVE && stream_status().generation && media_opened == seen + 1);
    close_from_peer(next);
    settle();
    assert(!stream_status().generation && !transport_status().links_releasing);
    return blocked;
}

struct env_gate {
    bool accepted_first;
    uint32_t ticket;
};

static void begin_environment(void *ctx)
{
    struct env_gate *gate = ctx;
    const uintptr_t inbound = 501;
    if (gate->accepted_first) {
        xiaotai_transport_host_accepted(inbound);
        assert(transport_status().inbound_admitted == 1 && !stream_status().generation);
    }
    /* Hold the owner here: neither the queued OPENED nor a queued QUIESCE
     * event may provide the answer to the synchronous arbitration. */
    const xiaotai_err_t err = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &gate->ticket);
    if (gate->accepted_first) {
        assert(err == XIAOTAI_ERR_BUSY && !gate->ticket && !xiaotai_quiesced());
        assert(transport_status().inbound_admitted == 1 && transport_status().links_open == 1);
    } else {
        assert(err == XIAOTAI_OK && gate->ticket && xiaotai_quiesced());
        xiaotai_transport_host_accepted(inbound);
        assert(transport_status().inbound_refused == 1 && !transport_status().inbound_admitted);
    }
}

static void end_environment(void *ctx)
{
    const struct env_gate *gate = ctx;
    xiaotai_quiesce_end(gate->ticket);
}

static void environment_gate(bool accepted_first)
{
    struct env_gate gate = {.accepted_first = accepted_first};
    assert(xiaotai_run(begin_environment, &gate) == XIAOTAI_OK);
    settle();
    if (accepted_first) {
        assert(stream_status().generation && media_opened == 1 && !media_closed);
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    } else {
        assert(!stream_status().generation && !media_opened);
        disconnected(501, 1);
    }
    close_from_peer(501);
    settle();
    if (!accepted_first) {
        assert(xiaotai_run(end_environment, &gate) == XIAOTAI_OK);
        settle();
        assert(stream_status().phase == XIAOTAI_STREAM_LISTENING);
        xiaotai_transport_host_accepted(502);
        settle();
        assert(stream_status().generation && media_opened == 1);
        close_from_peer(502);
        settle();
    }
    assert(!stream_status().generation && media_closed == 1);
    puts("environment gate: admission and maintenance have a single winner");
}

static void fail_later_environment_gate(void *ctx)
{
    (void)ctx;
    uint32_t ticket = 0;
    fail_environment_gate = true;
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &ticket) == XIAOTAI_ERR_BUSY);
    assert(!ticket && !xiaotai_quiesced());
    xiaotai_transport_host_accepted(503); /* Release is synchronous, no event required. */
    assert(transport_status().inbound_admitted == 1 && transport_status().links_open == 1);
}

static void environment_gate_rollback(void)
{
    assert(xiaotai_run(fail_later_environment_gate, NULL) == XIAOTAI_OK);
    settle();
    assert(stream_status().generation && media_opened == 1);
    close_from_peer(503);
    settle();
    assert(media_closed == 1);
}

struct environment_release_case {
    bool platform_online;
    bool admit_before_profile;
};

static void release_environment_in_owner(void *ctx)
{
    const struct environment_release_case *test = ctx;
    uint32_t ticket = 0;
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &ticket) == XIAOTAI_OK);
    assert(ticket && xiaotai_quiesced());

    /* A platform state change is visible before its owner notice. Run both
     * gate operations in one turn, as on an early environment unwind. */
    const xiaotai_platform_status_t platform = {
        .phase = test->platform_online ? XIAOTAI_PLATFORM_ONLINE : XIAOTAI_PLATFORM_UNBOUND,
        .authorized = test->platform_online, .signaling = test->platform_online,
    };
    platform_session_publish(&platform, "preflight-session-fixture");
    assert(transport_status().state == XIAOTAI_TRANSPORT_ONLINE);
    xiaotai_quiesce_end(ticket);
    assert(!xiaotai_quiesced());

    xiaotai_transport_host_accepted(504);
    const xiaotai_transport_status_t received = transport_status();
    const bool expected = test->platform_online && test->admit_before_profile;
    printf("environment release: online=%u admit_before_profile=%u admitted=%u refused=%u links_open=%u\n",
           test->platform_online, test->admit_before_profile, received.inbound_admitted,
           received.inbound_refused, received.links_open);
    fflush(stdout);
    assert(received.inbound_admitted == (unsigned)expected);
    assert(received.inbound_refused == (unsigned)!expected && received.links_open == (unsigned)expected);
    assert(!stream_status().generation && !media_opened);
}

static void environment_release(bool platform_online, bool admit_before_profile)
{
    struct environment_release_case test = {platform_online, admit_before_profile};
    const bool expected = platform_online && admit_before_profile;
    assert(xiaotai_run(release_environment_in_owner, &test) == XIAOTAI_OK);
    settle();
    if (expected) {
        assert(stream_status().generation && media_opened == 1);
    } else {
        assert(!stream_status().generation && !media_opened);
        assert(stream_status().phase == XIAOTAI_STREAM_CLOSED);
        assert(stream_status().closed & (platform_online ? XIAOTAI_STREAM_CLOSED_PROFILE : XIAOTAI_STREAM_CLOSED_OFFLINE));
        disconnected(504, 1);
    }
    close_from_peer(504);
    settle();
    assert(!stream_status().generation && media_closed == (unsigned)expected);
}

static void start_switch(void *ctx)
{
    (void)ctx;
    assert(xiaotai_environment_set("http://matrix.invalid") == XIAOTAI_OK);
}

static void check_environment_lease(void *ctx)
{
    (void)ctx;
    xiaotai_environment_status_t env;
    xiaotai_environment_status(&env);
    assert(xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
    assert(env.phase == XIAOTAI_ENVIRONMENT_WAITING && !env.write_attempted);
}

static void cancel_switch(void *ctx)
{
    (void)ctx;
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

static void pause_worker_before_pass(void)
{
    atomic_store(&pause_rtc_worker, true);
    xiaotai_transport_host_clock_advance(0);
    assert(xiaotai_signal_take(&rtc_worker_paused, WAIT_MS));
}

static bool environment_finish_held(void)
{
    assert(xiaotai_signal_init(&barrier_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&barrier_release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&environment_finished) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&rtc_worker_paused) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&rtc_worker_release) == XIAOTAI_OK);
    assert(xiaotai_run(start_switch, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&barrier_entered, WAIT_MS));
    assert(xiaotai_run(check_environment_lease, NULL) == XIAOTAI_OK);
    owner_sync();

    if (test_requested_stop) {
        xiaotai_transport_host_auto_stopped(test_inline_ack);
        if (test_stuck_ack) xiaotai_transport_host_fail_stop(XIAOTAI_ERR_TIMEOUT, 44, 1);
        assert(xiaotai_transport_restart() == XIAOTAI_OK);
        settle();
        assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
        if (test_stuck_ack) assert(transport_status().state == XIAOTAI_TRANSPORT_FAULT);
    }
    if (test_pending_stop) pause_worker_before_pass();
    if (test_stop_before_request) {
        pause_worker_before_pass();
        xiaotai_transport_host_auto_stopped(false);
        atomic_store(&pause_rtc_evaluate, true);
        assert(xiaotai_transport_restart() == XIAOTAI_OK);
        xiaotai_signal_give(&rtc_worker_release);
        assert(xiaotai_signal_take(&rtc_worker_paused, WAIT_MS));
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP));
        xiaotai_transport_host_stopped(0);
        xiaotai_signal_give(&rtc_worker_release);
        settle();
        printf("raw STOP before local downcall: unsolicited=%u downcalls=%u\n",
               transport_status().unsolicited_stops, xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP));
    }
    if (test_stale_stop) {
        pause_worker_before_pass();
        xiaotai_transport_host_stopped(0);
        assert(xiaotai_run(cancel_switch, NULL) == XIAOTAI_OK);
        owner_sync();
        xiaotai_signal_give(&barrier_release);
        assert(xiaotai_signal_take(&environment_finished, WAIT_MS));
        assert(environment_result == XIAOTAI_ENVIRONMENT_CANCELLED && !store_attempts && !xiaotai_quiesced());
        assert(xiaotai_run(start_switch, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&barrier_entered, WAIT_MS));
        assert(!transport_status().unsolicited_stops && xiaotai_quiesced());
        xiaotai_signal_give(&rtc_worker_release);
        settle();
        assert(transport_status().unsolicited_stops == 1);
        puts("raw STOP in A consumed only after A's cancel and B's acquisition");
    }
    if (test_unsolicited_stop && !test_pending_stop && !test_stop_before_request && !test_admission_before_error) {
        assert(!transport_status().unsolicited_stops);
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP));
        xiaotai_transport_host_stopped(stop_detail);
        settle();
        assert(transport_status().unsolicited_stops == 1);
        assert(transport_status().state == XIAOTAI_TRANSPORT_FAULT);
        assert(!stream_status().generation && !media_opened);
        assert(xiaotai_run(check_environment_lease, NULL) == XIAOTAI_OK);
        owner_sync();
        puts("barrier still in flight: quiesce held, H5 inactive, no store");
    }
    if (test_requested_stop && !test_pending_stop) {
        if (!test_inline_ack) xiaotai_transport_host_stopped(0);
        settle();
        assert(!transport_status().unsolicited_stops &&
               xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1 &&
               xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
    }

    if (test_pending_stop) xiaotai_transport_host_stopped(stop_detail);
    if (test_admission_before_error) {
        atomic_store(&pause_rtc_fault, true);
        xiaotai_transport_host_stopped(0);
        assert(xiaotai_signal_take(&rtc_worker_paused, WAIT_MS));
    }
    xiaotai_signal_give(&barrier_release);
    bool waited_for_classification = true;
    if (test_pending_stop) {
        environment_barrier_wait_idle();
        owner_sync();
        xiaotai_environment_status_t env;
        xiaotai_environment_status(&env);
        waited_for_classification = xiaotai_quiesced() && !env.write_attempted;
        printf("raw STOP before classification: waiting=%u wrote=%u\n",
               waited_for_classification, env.write_attempted);
        xiaotai_signal_give(&rtc_worker_release);
        settle();
        if (test_requested_stop && waited_for_classification) {
            assert(xiaotai_signal_take(&barrier_entered, WAIT_MS));
            xiaotai_signal_give(&barrier_release);
        }
    }
    assert(xiaotai_signal_take(&environment_finished, WAIT_MS));
    bool sealed = true;
    if (test_admission_before_error) {
        owner_sync();
        const xiaotai_transport_status_t before = transport_status();
        assert(before.state == XIAOTAI_TRANSPORT_ONLINE && !xiaotai_quiesced());
        xiaotai_transport_host_accepted(541);
        const xiaotai_transport_status_t after = transport_status();
        sealed = !before.connect_ready && before.inbound_admitted == after.inbound_admitted &&
                 before.inbound_refused + 1 == after.inbound_refused;
        printf("error visible before teardown: connect_ready=%u admission_delta=%u refused_delta=%u\n",
               before.connect_ready, after.inbound_admitted - before.inbound_admitted,
               after.inbound_refused - before.inbound_refused);
        xiaotai_signal_give(&rtc_worker_release);
    }
    settle();
    assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
    assert(final_environment.phase == XIAOTAI_ENVIRONMENT_IDLE);
    printf("async_stop=%u transport_fault=%u final_error=0x%x wrote=%u store_calls=%u quiesced=%u restart=%u\n",
           transport_status().unsolicited_stops, transport_status().state == XIAOTAI_TRANSPORT_FAULT,
           (unsigned)final_environment.error, final_environment.write_attempted, store_attempts,
           xiaotai_quiesced(), xiaotai_restart_pending());
    bool passed = (!test_unsolicited_stop || !test_monitor_stop)
        ? final_environment.error == XIAOTAI_ERR_NO_MEM && final_environment.write_attempted && store_attempts == 1
        : final_environment.error == (stop_detail ? stop_detail : XIAOTAI_ERR_INVALID_STATE) &&
          !final_environment.write_attempted && !store_attempts;
    if (test_next_request) {
        assert(xiaotai_run(start_switch, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&barrier_entered, WAIT_MS));
        xiaotai_signal_give(&barrier_release);
        assert(xiaotai_signal_take(&environment_finished, WAIT_MS));
        settle();
        printf("next ticket: final_error=0x%x wrote=%u store_calls=%u\n",
               (unsigned)final_environment.error, final_environment.write_attempted, store_attempts);
        passed = passed && final_environment.error == XIAOTAI_ERR_NO_MEM && final_environment.write_attempted &&
                 store_attempts == 1 && !xiaotai_quiesced() && !xiaotai_restart_pending();
    }
    xiaotai_signal_deinit(&rtc_worker_release);
    xiaotai_signal_deinit(&rtc_worker_paused);
    xiaotai_signal_deinit(&environment_finished);
    xiaotai_signal_deinit(&barrier_release);
    xiaotai_signal_deinit(&barrier_entered);
    if (test_stop_before_request)
        passed = passed && transport_status().unsolicited_stops == 1 &&
                 !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_STOP);
    return passed && waited_for_classification && sealed && environment_result == XIAOTAI_ENVIRONMENT_FAILED;
}

int main(int argc, char **argv)
{
    bool passed = true;
    if (argc == 2) {
        test_finish_held = !strncmp(argv[1], "environment-finish-held", strlen("environment-finish-held"));
        test_monitor_stop = test_finish_held && strcmp(argv[1], "environment-finish-held-default");
        test_inline_ack = !strcmp(argv[1], "environment-finish-held-inline-ack");
        test_stuck_ack = !strcmp(argv[1], "environment-finish-held-stuck-ack");
        test_requested_stop = test_inline_ack || test_stuck_ack ||
                              !strcmp(argv[1], "environment-finish-held-stop-ack") ||
                              !strcmp(argv[1], "environment-finish-held-pending-ack");
        test_pending_stop = !strcmp(argv[1], "environment-finish-held-pending") ||
                            !strcmp(argv[1], "environment-finish-held-pending-ack");
        test_stop_before_request = !strcmp(argv[1], "environment-finish-held-stop-before-request");
        test_stale_stop = !strcmp(argv[1], "environment-finish-held-stale-stop");
        test_admission_before_error = !strcmp(argv[1], "environment-finish-held-admission-before-error");
        test_unsolicited_stop = test_finish_held && !test_requested_stop && !test_stale_stop &&
                               strcmp(argv[1], "environment-finish-held-control");
        test_next_request = test_finish_held && !strcmp(argv[1], "environment-finish-held-next");
        if (!strcmp(argv[1], "environment-finish-held-explicit-error")) stop_detail = XIAOTAI_ERR_TIMEOUT;
        pause_admission_on_stop = !strcmp(argv[1], "local-stop-gate") || !strcmp(argv[1], "local-stop-offline") ||
                                  !strcmp(argv[1], "stop-first") || !strcmp(argv[1], "transport-first");
        const bool release_native = !strcmp(argv[1], "environment-release-offline-native") ||
                                    !strcmp(argv[1], "environment-release-profile-native");
        const bool release_s3 = !strcmp(argv[1], "environment-release-offline-s3") ||
                                !strcmp(argv[1], "environment-release-profile-s3");
        prepare(release_s3 ? 1 : 2, !strncmp(argv[1], "environment-", strlen("environment-")), release_native);
        if (test_finish_held) passed = environment_finish_held();
        else if (!strcmp(argv[1], "local-stop-offline")) local_stop_offline();
        else if (!strcmp(argv[1], "local-stop-gate")) passed = local_stop_gate();
        else if (!strcmp(argv[1], "local-stop-default")) passed = !local_stop_gate();
        else if (!strcmp(argv[1], "busy")) genuine_busy();
        else if (!strcmp(argv[1], "environment-inbound-first")) environment_gate(true);
        else if (!strcmp(argv[1], "environment-close-first")) environment_gate(false);
        else if (!strcmp(argv[1], "environment-rollback")) environment_gate_rollback();
        else if (release_native || release_s3)
            environment_release(!strncmp(argv[1], "environment-release-profile-", strlen("environment-release-profile-")),
                                release_native);
        else if (!strcmp(argv[1], "peer-error")) peer_error_snapshot();
        else if (!strcmp(argv[1], "listener-off")) listener_off();
        else if (!strcmp(argv[1], "busy-before-ack")) passed = !replacement_race(argv[1], false, true, false);
        else if (!strcmp(argv[1], "error-then-ack")) passed = replacement_race(argv[1], false, false, true);
        else if (!strcmp(argv[1], "stop-first")) passed = replacement_race(argv[1], true, false, false);
        else {
            assert(!strcmp(argv[1], "transport-first"));
            passed = replacement_race(argv[1], false, false, false);
        }
        goto shutdown;
    }
    assert(argc == 1);
    prepare(1, false, false);

    /* HTTP wins while the listener mode is still OPEN on the blocked owner. */
    block_owner();
    request_http();
    assert(xiaotai_platform_exchanges_busy());
    xiaotai_transport_host_accepted(101);
    xiaotai_transport_status_t status = transport_status();
    assert(!status.inbound_admitted && status.inbound_refused == 1 && !status.links_open);
    disconnected(101, 1);
    assert(!atomic_load(&requests));
    close_from_peer(101);
    xiaotai_signal_give(&owner_release);
    wait_http(1);
    assert(stream_status().phase == XIAOTAI_STREAM_LISTENING);

    /* RTC wins the initial slot, but opened() is already queued on the owner
     * when HTTP is accepted. Only stack confirmation releases preflight. */
    block_owner();
    xiaotai_transport_host_accepted(102);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    status = transport_status();
    assert(status.inbound_admitted == 1 && status.links_open == 1 && !stream_status().generation);
    atomic_store(&http_needs_closed, 2);
    request_http();
    assert(xiaotai_platform_exchanges_busy());
    xiaotai_signal_give(&owner_release);
    wait_preflight_deferred();
    disconnected(102, 2);
    status = transport_status();
    assert(!status.links_open && status.links_releasing == 1 && !media_opened);
    assert(xiaotai_platform_exchanges_busy() && atomic_load(&requests) == 1);
    close_from_peer(102);
    wait_http(2);
    assert(stream_status().phase == XIAOTAI_STREAM_LISTENING && !transport_status().links_releasing);

    /* An already handed-off stream is not a temporary candidate. */
    xiaotai_transport_host_accepted(103);
    settle();
    const uint32_t active = stream_status().generation;
    assert(active && media_opened == 1 && media_generation == active);
    assert(stream_status().phase == XIAOTAI_STREAM_ACTIVE);
    request_http();
    wait_http(3);
    assert(stream_status().generation == active && stream_status().phase == XIAOTAI_STREAM_ACTIVE);
    assert(!media_closed && xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2);
    close_from_peer(103);
    settle();
    assert(!stream_status().generation && media_closed == 1 && !media_generation);

shutdown:
    xiaotai_transport_reset();
    xiaotai_platform_reset();
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_identity_reset();
    xiaotai_stream_reset();
    for (size_t i = 0; i < 2; ++i) xiaotai_task_notify(&retained_handles[i]);
    xiaotai_signal_deinit(&replies);
    xiaotai_signal_deinit(&owner_release);
    xiaotai_signal_deinit(&owner_entered);
    return passed ? 0 : 1;
}
