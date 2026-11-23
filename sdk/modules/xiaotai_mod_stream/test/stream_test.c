// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/regression/test_cores3_opus_receive.py
// Replaces legacy: tests/regression/test_p4_h5_local_stop.py
// Replaces legacy: tests/regression/test_device_profile_response.py
// Replaces legacy: tests/regression/test_wt99_h5_local_stop.py
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai_kv_host.h"

#define CHECK(ok) do { if (!(ok)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #ok); abort(); } } while (0)
#define OK(call) CHECK((call) == XIAOTAI_OK)

typedef struct {
    xiaotai_platform_status_t platform;
    xiaotai_platform_profile_fn_t profile;
    void *profile_ctx;
    xiaotai_platform_exchange_preflight_fn_t preflight;
    void *preflight_ctx;
    xiaotai_err_t preflight_result;
    unsigned resumes;
    xiaotai_transport_state_t transport_state;
    xiaotai_transport_requirement_t transport_need;
    xiaotai_link_listener_t listener;
    xiaotai_link_listen_t mode;
    xiaotai_media_requirement_t needs[2];
    xiaotai_media_session_t media;
    xiaotai_stream_ended_t ended;
    unsigned n_needs, registrations, mode_sets, starts, ends, states;
    unsigned opens, closes, receives, disconnects, permits, parts_calls, part_calls;
    uint32_t disconnected, closed_media, received, active, attached_gen, resources;
    int last_state;
    xiaotai_err_t media_result, receive_result, listen_failure;
    bool external, capture, opus, pending, retiring;
    unsigned pending_cancelled, enabled_ext;
    char trace[512];
} fixture_t;

static fixture_t f;
static xiaotai_signal_t barrier;
static xiaotai_signal_t owner_blocked, owner_release;

static xiaotai_stream_status_t status(void)
{
    xiaotai_stream_status_t s;
    xiaotai_stream_status(&s);
    return s;
}

static xiaotai_stream_stats_t stats(void)
{
    xiaotai_stream_stats_t s;
    xiaotai_stream_stats(&s);
    return s;
}

static void note(char action)
{
    const size_t at = strlen(f.trace);
    CHECK(at + 1U < sizeof(f.trace));
    f.trace[at] = action;
    f.trace[at + 1U] = '\0';
}

bool __wrap_xiaotai_mem_has_external(void) { return f.external; }

xiaotai_err_t __real_xiaotai_activity_begin(const xiaotai_activity_t *activity, uint32_t *id);

xiaotai_err_t __wrap_xiaotai_activity_begin(const xiaotai_activity_t *activity, uint32_t *id)
{
    if (!strcmp(activity->name, XIAOTAI_STREAM_ACTIVITY)) f.resources = activity->resources;
    return __real_xiaotai_activity_begin(activity, id);
}

void xiaotai_platform_status(xiaotai_platform_status_t *status) { *status = f.platform; }

bool xiaotai_platform_exchanges_busy(void) { return f.platform.exchanges != 0; }

xiaotai_err_t xiaotai_platform_exchange_preflight_register(xiaotai_platform_exchange_preflight_fn_t fn, void *ctx)
{
    CHECK(fn && !f.preflight && !f.registrations);
    f.preflight = fn;
    f.preflight_ctx = ctx;
    return XIAOTAI_OK;
}

void xiaotai_platform_exchange_preflight_resume(void) { ++f.resumes; }

xiaotai_err_t xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t write, void *ctx)
{
    CHECK(!strcmp(scene, "stream") && !f.profile);
    f.profile = write;
    f.profile_ctx = ctx;
    return XIAOTAI_OK;
}

size_t xiaotai_platform_scene_parts(const char *activity, const char *scene, char *out, size_t capacity, size_t *length)
{
    ++f.parts_calls;
    const void *parts[4];
    const size_t n = xiaotai_activity_contributions(activity, XIAOTAI_PLATFORM_SLOT_SCENE, scene, parts, 4);
    size_t placed = 0;
    for (size_t i = 0; i < n && i < 4; ++i) {
        const xiaotai_platform_scene_part_t *p = parts[i];
        if (*length >= capacity) break;
        char text[256];
        size_t room = capacity - *length - 1U;
        if (room > sizeof(text)) room = sizeof(text);
        const int count = p->write(text, room, p->ctx);
        if (count <= 0 || (size_t)count > room) continue;
        out[(*length)++] = ',';
        memcpy(out + *length, text, (size_t)count);
        *length += (size_t)count;
        ++placed;
    }
    return placed;
}

xiaotai_transport_state_t xiaotai_transport_state(void) { return f.transport_state; }

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *need)
{
    f.transport_need = *need;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listen(const xiaotai_link_listener_t *listener, uint32_t *id)
{
    CHECK(f.registrations++ == 0 && f.mode == XIAOTAI_LINK_LISTEN_OFF);
    f.listener = *listener;
    *id = 1;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listener_open_gate(uint32_t id, bool acquire)
{
    (void)id;
    (void)acquire;
    CHECK(false); /* This module fixture keeps the default policy. */
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t xiaotai_link_listener_pause_open(uint32_t id, bool paused)
{
    (void)id;
    (void)paused;
    CHECK(false); /* The default profile does not pause at local stop. */
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t xiaotai_link_listen_set(uint32_t id, xiaotai_link_listen_t mode)
{
    CHECK(id == 1 && mode != f.mode);
    ++f.mode_sets;
    if (f.listen_failure != XIAOTAI_OK) {
        const xiaotai_err_t error = f.listen_failure;
        f.listen_failure = XIAOTAI_OK;
        return error;
    }
    f.mode = mode;
    if (mode == XIAOTAI_LINK_LISTEN_OFF && f.pending) {
        f.pending = false;
        f.retiring = true;
        ++f.pending_cancelled;
    }
    return XIAOTAI_OK;
}

bool xiaotai_link_listener_retiring(uint32_t id)
{
    CHECK(id == 1);
    return f.retiring;
}

xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t receive, void *ctx)
{
    CHECK(kind == XIAOTAI_LINK_AUDIO && !receive && !ctx);
    ++f.receives;
    f.received = generation;
    note('r');
    return f.receive_result;
}

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool permit)
{
    (void)generation; (void)kind; (void)permit;
    ++f.permits;
    return XIAOTAI_OK;
}

void xiaotai_link_disconnect(uint32_t generation)
{
    ++f.disconnects;
    f.disconnected = generation;
    note('d');
}

static const xiaotai_media_sink_t sink;
const xiaotai_media_sink_t *xiaotai_link_audio_sink(void) { return &sink; }

bool xiaotai_media_can_send(const xiaotai_media_format_t *format)
{
    CHECK(format->encoding == XIAOTAI_CODEC_G711A && format->rate_hz == 8000);
    return f.capture;
}

bool xiaotai_media_can_receive(const xiaotai_media_format_t *format)
{
    CHECK(format->encoding == XIAOTAI_CODEC_OPUS && format->rate_hz == 16000);
    return f.opus;
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *need)
{
    CHECK(f.n_needs < 2);
    f.needs[f.n_needs++] = *need;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session)
{
    ++f.opens;
    f.media = *session;
    note('m');
    return f.media_result;
}

void xiaotai_media_close(uint32_t generation)
{
    ++f.closes;
    f.closed_media = generation;
    note('c');
}

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_STARTED)) {
        ++f.starts;
        f.active = (uint32_t)event->args.value;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_ENDED)) {
        ++f.ends;
        CHECK(event->size == sizeof(f.ended));
        f.ended = *(const xiaotai_stream_ended_t *)event->data;
        CHECK((int)f.ended.end == event->args.value);
    } else {
        ++f.states;
        f.last_state = event->args.value;
    }
}

static void begin_attachment(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    CHECK(status().phase == XIAOTAI_STREAM_STARTING && status().generation == generation);
    CHECK(xiaotai_activity_generation(id) == generation);
    f.attached_gen = generation;
    note('b');
}

static void session_attachment(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id;
    (void)ctx;
    CHECK(generation == 0);
    note('0');
}

static void end_attachment(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id;
    (void)generation;
    (void)ctx;
    note('e');
}

static const xiaotai_link_media_t video = {
    .send = true, .send_stream = 4, .peer_streams = XIAOTAI_LINK_PEER_STREAM_IGNORE,
    .send_format = {XIAOTAI_LINK_CODEC_H264, 0, 0}, .receive = true, .receive_stream = 5,
    .receive_request = true, .receive_formats = {{XIAOTAI_LINK_CODEC_JPEG, 0, 0}},
};
static const xiaotai_link_media_t no_video;
static const xiaotai_link_media_t invalid_receive = {.receive = true};

static int part_text(char *out, size_t capacity, void *ctx)
{
    (void)ctx;
    ++f.part_calls;
    if (f.enabled_ext == 5) return 0;
    if (f.enabled_ext == 6) return -1;
    const char text[] = "\"up_video_mt\":[\"h264\"],\"down_video_mt\":[\"mjpeg\"],\"camera_rotation\":270";
    if (capacity < sizeof(text) - 1U) return -1;
    memcpy(out, text, sizeof(text) - 1U);
    return sizeof(text) - 1U;
}

static const xiaotai_platform_scene_part_t part = {.write = part_text};

static xiaotai_err_t observer_init(void)
{
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_STATE, on_event, NULL));
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_STARTED, on_event, NULL));
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_ENDED, on_event, NULL));
    static xiaotai_contribution_t c[4];
    memset(c, 0, sizeof(c));
    if (f.enabled_ext) {
        c[0] = (xiaotai_contribution_t){.slot = XIAOTAI_PLATFORM_SLOT_SCENE, .data = &part};
        c[1] = (xiaotai_contribution_t){.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = "stream",
            .data = f.enabled_ext == 3 ? &no_video : f.enabled_ext == 4 ? &invalid_receive : &video};
        if (f.enabled_ext == 2 || f.enabled_ext == 3) {
            c[2] = (xiaotai_contribution_t){.slot = XIAOTAI_LINK_SLOT_VIDEO, .data = &video};
        }
    }
    const xiaotai_attachment_t a = {
        .begin = begin_attachment, .session = session_attachment, .end = end_attachment,
        .contributions = c,
    };
    return xiaotai_activity_attach("stream", &a);
}

static void noop(void) {}

static void sync_job(void *ctx) { (void)ctx; xiaotai_signal_give(&barrier); }

static void drain(void)
{
    for (int i = 0; i < 4; ++i) {
        OK(xiaotai_run(sync_job, NULL));
        CHECK(xiaotai_signal_take(&barrier, 3000));
    }
}

static void owner(void (*job)(void *ctx), void *ctx)
{
    OK(xiaotai_run(job, ctx));
    drain();
}

static void setup(const xiaotai_stream_config_t *config, bool external, bool capture, bool opus,
                  bool ready, unsigned ext)
{
    xiaotai_stream_reset();
    xiaotai_kv_host_clear();
    memset(&f, 0, sizeof(f));
    f.external = external;
    f.capture = capture;
    f.opus = opus;
    f.enabled_ext = ext;
    f.platform.generation = 50;
    if (ready) {
        f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
        f.platform.profile_ready = true;
        f.transport_state = XIAOTAI_TRANSPORT_ONLINE;
    }
    OK(xiaotai_stream_configure(config));
    OK(xiaotai_eventlog_init(NULL));
    static const xiaotai_module_t stream = {
        .name = "stream", .init = xiaotai_module_stream_init, .start = xiaotai_module_stream_start,
    };
    static const xiaotai_module_t observer = {.name = "observer", .init = observer_init};
    static const xiaotai_module_t *const modules[] = {&stream, &observer};
    static const xiaotai_priority_t priorities[] = {{"ai", 1}, {"stream", 2}, {"call", 3}, {NULL, 0}};
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = 2, .priorities = priorities, .available_needs = UINT32_MAX,
        .restart = noop,
    };
    OK(xiaotai_framework_start(&framework));
    drain();
    CHECK(f.registrations == 1 && !f.listener.handlers.command);
    CHECK(f.transport_need.links == 1 && f.transport_need.command_pool_bytes == 512);
    CHECK(f.listener.profile.command_max_bytes == 256 && f.listener.profile.command_pool_bytes == 512);
    CHECK(f.n_needs == 2);
    CHECK(f.needs[0].uplink.encoding == XIAOTAI_CODEC_G711A && f.needs[0].downlink.rate_hz == 8000);
    CHECK(f.needs[1].uplink.rate_hz == 0 && f.needs[1].downlink.encoding == XIAOTAI_CODEC_OPUS);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(f.needs[i].packet_ms == 20 && f.needs[i].max_packet_bytes == 640);
        CHECK(f.needs[i].downlink_burst_ms == 200 && !f.needs[i].preroll_ms);
    }
    CHECK(xiaotai_stream_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
}

static void teardown(void)
{
    CHECK(f.permits == 0);
    xiaotai_framework_reset();
    xiaotai_stream_reset();
}

static void event(const char *name, int value)
{
    const xiaotai_args_t args = {.value = value};
    OK(xiaotai_event_emit(name, &args, NULL, 0));
    drain();
}

static void arrive(void *ctx)
{
    f.listener.handlers.opened(f.listener.handlers.ctx, (uint32_t)(uintptr_t)ctx);
}

static void open_link(uint32_t gen) { owner(arrive, (void *)(uintptr_t)gen); }

static void set_enabled(void *ctx)
{
    OK(xiaotai_setting_set_int(XIAOTAI_SETTING_STREAM_ENABLED, (int32_t)(uintptr_t)ctx));
}

static uint32_t quiesce_id;
static void quiesce(void *ctx)
{
    if (ctx) OK(xiaotai_quiesce_begin("test", &quiesce_id));
    else xiaotai_quiesce_end(quiesce_id);
}

static void test_admission(void)
{
    CHECK(xiaotai_stream_configure(&(xiaotai_stream_config_t){.up_audio_stream = 16}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_stream_configure(&(xiaotai_stream_config_t){.down_audio_stream = 15}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_stream_configure(&(xiaotai_stream_config_t){.platform_idle = 3}) == XIAOTAI_ERR_INVALID_ARG);
    setup(NULL, true, true, true, false, 0);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && !f.mode_sets && !f.states);
    CHECK(status().closed == (XIAOTAI_STREAM_CLOSED_OFFLINE | XIAOTAI_STREAM_CLOSED_PROFILE));
    f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(status().closed & XIAOTAI_STREAM_CLOSED_OFFLINE);
    f.transport_state = XIAOTAI_TRANSPORT_ONLINE;
    event(XIAOTAI_EVENT_TRANSPORT_STATE, 0);
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_PROFILE);
    f.platform.profile_ready = true;
    f.listen_failure = XIAOTAI_ERR_NO_MEM;
    event(XIAOTAI_EVENT_PLATFORM_PROFILE, 1);
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_MEMORY && stats().listen_failures == 1);
    event(XIAOTAI_EVENT_PLATFORM_PROFILE, 1);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN && f.states == 1 && status().phase == XIAOTAI_STREAM_LISTENING);
    event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(f.states == 1 && f.mode_sets == 2);
    f.pending = true;
    owner(set_enabled, NULL);
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_DISABLED && f.pending_cancelled == 1 && f.states == 2);
    owner(set_enabled, (void *)1);
    owner(quiesce, (void *)1);
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_QUIESCED && f.last_state == XIAOTAI_STREAM_CLOSED);
    owner(quiesce, NULL);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN);
    f.platform.phase = XIAOTAI_PLATFORM_LOGIN;
    open_link(99);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && status().closed == XIAOTAI_STREAM_CLOSED_OFFLINE);
    CHECK(f.disconnected == 99 && !f.opens && f.ended.end == XIAOTAI_STREAM_END_CLOSED && !f.ended.started);
    f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    f.platform.profile_ready = false;
    event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_PROFILE);
    teardown();

    for (int policy = 0; policy <= 2; ++policy) {
        for (int external = 0; external <= 1; ++external) {
            const xiaotai_stream_config_t config = {.platform_idle = policy, .admit_before_profile = true};
            setup(&config, external != 0, true, false, true, 0);
            f.platform.profile_ready = false;
            event(XIAOTAI_EVENT_PLATFORM_PROFILE, 0);
            CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN);
            f.platform.exchanges = 1;
            const bool waits = policy == 1 || (policy == 0 && !external);
            CHECK((f.preflight != NULL) == waits && (f.listener.admit_open != NULL) == waits);
            if (waits) CHECK(!f.listener.admit_open(f.listener.handlers.ctx));
            /* Platform has no busy-edge event: the delayed opened() must
             * still see reality and must never open activity/media. */
            if (waits) {
                open_link(7);
                CHECK(f.ended.end == XIAOTAI_STREAM_END_CLOSED && !f.opens && !f.starts);
            } else {
                event(XIAOTAI_EVENT_PLATFORM_PROFILE, 0);
            }
            CHECK((f.mode == XIAOTAI_LINK_LISTEN_OFF) == waits);
            if (waits) CHECK(status().closed == XIAOTAI_STREAM_CLOSED_PLATFORM);
            f.platform.exchanges = 0;
            event(XIAOTAI_EVENT_PLATFORM_IDLE, 0);
            CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN);
            open_link(8);
            f.platform.exchanges = 1;
            event(XIAOTAI_EVENT_PLATFORM_PROFILE, 0);
            CHECK(status().generation == 8 && !f.closes);
            CHECK((f.mode == XIAOTAI_LINK_LISTEN_OFF) == waits);
            OK(xiaotai_stream_stop(8));
            drain();
            teardown();
        }
    }
}

static void run_preflight(void *unused)
{
    (void)unused;
    CHECK(xiaotai_on_owner() && f.preflight);
    f.preflight_result = f.preflight(f.preflight_ctx);
}

static void test_preflight(void)
{
    setup(NULL, false, true, false, true, 0);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN && f.listener.admit_open(f.listener.handlers.ctx));
    /* A request becomes visible before any queued owner message. */
    f.platform.exchanges = 1;
    CHECK(!f.listener.admit_open(f.listener.handlers.ctx));
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN);
    f.pending = true;
    f.listen_failure = XIAOTAI_ERR_NO_MEM;
    owner(run_preflight, NULL);
    CHECK(f.preflight_result == XIAOTAI_ERR_NO_MEM && f.pending && !f.resumes);
    owner(run_preflight, NULL);
    CHECK(f.preflight_result == XIAOTAI_ERR_BUSY && f.pending_cancelled == 1 && f.retiring);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && !f.opens);
    event(XIAOTAI_EVENT_TRANSPORT_LINKS, 1);
    CHECK(!f.resumes);
    f.retiring = false;
    event(XIAOTAI_EVENT_TRANSPORT_LINKS, 0);
    CHECK(f.resumes == 1);
    event(XIAOTAI_EVENT_TRANSPORT_LINKS, 0);
    CHECK(f.resumes == 1);
    owner(run_preflight, NULL);
    CHECK(f.preflight_result == XIAOTAI_OK && !f.opens);
    owner(set_enabled, NULL);
    f.platform.exchanges = 0;
    event(XIAOTAI_EVENT_PLATFORM_IDLE, 0);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF);
    owner(set_enabled, (void *)1);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN);
    open_link(101);
    f.platform.exchanges = 1;
    owner(run_preflight, NULL);
    CHECK(f.preflight_result == XIAOTAI_OK && f.mode == XIAOTAI_LINK_LISTEN_OFF);
    CHECK(status().generation == 101 && !f.closes && !f.disconnects);
    OK(xiaotai_stream_stop(101));
    drain();
    teardown();
}

static void subscribe(void *ctx)
{
    uint32_t gen = (uint32_t)(uintptr_t)ctx;
    f.listener.handlers.peer_subscribed(NULL, gen, XIAOTAI_LINK_AUDIO, 10, true);
    f.listener.handlers.peer_subscribed(NULL, gen, XIAOTAI_LINK_VIDEO, 11, true);
}

static void lost_link(void *ctx)
{
    const uint32_t *args = ctx;
    f.listener.handlers.closed(NULL, args[0], args[1], 0);
}

static void fail_media(void *ctx)
{
    f.media.event((uint32_t)(uintptr_t)ctx, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, f.media.ctx);
}

static uint32_t other_activity;
static void begin_activity(void *ctx)
{
    const xiaotai_activity_t activity = {.name = ctx, .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    OK(xiaotai_activity_begin(&activity, &other_activity));
}

static void end_activity(void *ctx) { (void)ctx; xiaotai_activity_end(other_activity); }

static void block_owner(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&owner_blocked);
    CHECK(xiaotai_signal_take(&owner_release, 3000));
}

static void empty_job(void *ctx) { (void)ctx; }

static void verify_stop_backpressure(uint32_t generation)
{
    OK(xiaotai_signal_init(&owner_blocked));
    OK(xiaotai_signal_init(&owner_release));
    OK(xiaotai_run(block_owner, NULL));
    CHECK(xiaotai_signal_take(&owner_blocked, 3000));
    xiaotai_err_t err = XIAOTAI_OK;
    size_t jobs = 0;
    while (jobs < 1024U && (err = xiaotai_run(empty_job, NULL)) == XIAOTAI_OK) ++jobs;
    CHECK(jobs && err == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_stream_stop(generation) == XIAOTAI_ERR_NO_MEM);
    CHECK(status().generation == generation);
    xiaotai_signal_give(&owner_release);
    const int64_t deadline = xiaotai_time_us() + 3000000;
    while ((err = xiaotai_run(sync_job, NULL)) == XIAOTAI_ERR_NO_MEM && xiaotai_time_us() < deadline) {
        xiaotai_delay_ms(1);
    }
    CHECK(err == XIAOTAI_OK && xiaotai_signal_take(&barrier, 3000));
    xiaotai_signal_deinit(&owner_blocked);
    xiaotai_signal_deinit(&owner_release);
}

static void test_session(void)
{
    setup(NULL, true, true, true, true, 1);
    const xiaotai_link_media_t *audio = &f.listener.profile.audio;
    CHECK(audio->send && audio->send_stream == 10 && audio->receive_stream == 14);
    CHECK(audio->receive_formats[0].codec == XIAOTAI_LINK_CODEC_G711A);
    CHECK(audio->receive_formats[1].codec == XIAOTAI_LINK_CODEC_OPUS);
    CHECK(xiaotai_stream_stop(0) == XIAOTAI_ERR_INVALID_ARG);
    owner(begin_activity, "ai");
    open_link(41);
    CHECK(!strcmp(f.trace, "bmr") && stats().preempted_others == 1);
    CHECK(f.resources == (XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT |
                          XIAOTAI_RESOURCE_LINK | XIAOTAI_RESOURCE_CAMERA));
    CHECK(f.opens == 1 && f.received == 41 && f.attached_gen == 41 && f.active == 41);
    CHECK(status().phase == XIAOTAI_STREAM_ACTIVE && f.media.duplex && f.media.sink == &sink);
    CHECK(f.media.uplink.rate_hz == 8000 && f.media.downlink_burst_ms == 200 && f.media.latency.max_ms == 100);
    owner(subscribe, (void *)40);
    CHECK(!status().peer_audio && !status().peer_video);
    owner(subscribe, (void *)41);
    CHECK(status().peer_audio && status().peer_video);
    verify_stop_backpressure(41);
    owner(set_enabled, NULL);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && status().generation == 41 && !f.ends);
    owner(set_enabled, (void *)1);
    OK(xiaotai_stream_stop(40));
    drain();
    CHECK(stats().stale_stops == 1 && f.ends == 0);
    OK(xiaotai_stream_stop(41));
    drain();
    CHECK(!strcmp(f.trace, "bmr0ecd") && f.ended.started && f.ended.end == XIAOTAI_STREAM_END_LOCAL);
    CHECK(f.disconnected == 41 && f.closed_media == 41 && f.mode == XIAOTAI_LINK_LISTEN_OPEN);
    CHECK(status().generation == 0 && !status().peer_audio && !status().peer_video);
    OK(xiaotai_stream_stop(41));
    drain();
    CHECK(f.ends == 1 && f.closes == 1 && f.disconnects == 1 && stats().stale_stops == 2);
    uint32_t args[] = {41, XIAOTAI_LINK_END_PEER};
    open_link(42);
    owner(lost_link, args);
    CHECK(status().generation == 42 && f.ends == 1);
    args[0] = 42;
    owner(lost_link, args);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_PEER && f.ended.error == XIAOTAI_OK && f.disconnected == 41);

    f.media_result = XIAOTAI_ERR_NOT_SUPPORTED;
    open_link(43);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_MEDIA && !f.ended.started && f.closes == 2);
    f.media_result = XIAOTAI_OK;
    f.receive_result = XIAOTAI_ERR_INVALID_STATE;
    open_link(44);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_LINK && !f.ended.started && f.closed_media == 44);
    f.receive_result = XIAOTAI_OK;
    open_link(45);
    owner(fail_media, (void *)44);
    CHECK(status().generation == 45);
    owner(fail_media, (void *)45);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_MEDIA && f.ended.started);

    open_link(46);
    open_link(47);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_BUSY && f.ended.generation == 47 && status().generation == 46);
    OK(xiaotai_intent_post(XIAOTAI_INTENT_STREAM_STOP, NULL));
    drain();
    CHECK(f.ended.end == XIAOTAI_STREAM_END_LOCAL && f.ended.generation == 46);
    open_link(48);
    f.trace[0] = '\0';
    owner(begin_activity, "call");
    CHECK(!strcmp(f.trace, "ecd") && f.ended.end == XIAOTAI_STREAM_END_REVOKED);
    open_link(49);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_BUSY && !f.ended.started && f.mode == XIAOTAI_LINK_LISTEN_OPEN);
    owner(end_activity, NULL);
    open_link(50);
    owner(quiesce, (void *)1);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_REVOKED && f.mode == XIAOTAI_LINK_LISTEN_OFF);
    owner(quiesce, NULL);

    static const int ordinary[] = {
        XIAOTAI_PLATFORM_OFFLINE_NETWORK, XIAOTAI_PLATFORM_OFFLINE_SIGNALING, XIAOTAI_PLATFORM_OFFLINE_REFRESH,
    };
    open_link(51);
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); ++i) {
        f.platform.phase = XIAOTAI_PLATFORM_CONNECTING;
        event(XIAOTAI_EVENT_PLATFORM_OFFLINE, ordinary[i]);
        CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && status().generation == 51);
        f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
        event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
        CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN && status().generation == 51);
    }
    f.platform.phase = XIAOTAI_PLATFORM_LOGIN;
    event(XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_BINDING);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_BINDING);
    f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    open_link(52);
    f.platform.phase = XIAOTAI_PLATFORM_UNBOUND;
    event(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, 0);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_BINDING && f.ended.generation == 52);
    f.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    event(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    open_link(53);
    ++f.platform.generation;
    event(XIAOTAI_EVENT_PLATFORM_PROFILE, 0);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_BINDING && stats().ended_binding == 3);
    open_link(54);
    args[0] = 54; args[1] = XIAOTAI_LINK_END_TIMEOUT;
    owner(lost_link, args);
    CHECK(f.ended.end == XIAOTAI_STREAM_END_LINK && f.ended.error == XIAOTAI_ERR_TIMEOUT);

    open_link(55);
    f.platform.phase = XIAOTAI_PLATFORM_CONNECTING;
    event(XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_NETWORK);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && status().generation == 55);
    OK(xiaotai_stream_stop(55));
    drain();
    CHECK(f.ended.generation == 55 && f.ended.end == XIAOTAI_STREAM_END_LOCAL);
    CHECK(f.closed_media == 55 && f.disconnected == 55 && !status().generation && f.mode == XIAOTAI_LINK_LISTEN_OFF);
    teardown();

    setup(NULL, true, false, false, true, 0);
    CHECK(!f.listener.profile.audio.send && f.listener.profile.audio.receive);
    CHECK(f.listener.profile.audio.receive_formats[1].codec == XIAOTAI_LINK_CODEC_NONE);
    open_link(1);
    CHECK(f.media.uplink.rate_hz == 0 && f.media.downlink.rate_hz == 8000);
    CHECK(f.resources == (XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK));
    OK(xiaotai_stream_stop(1));
    drain();
    teardown();
}

static void test_profile_case(void *ctx)
{
    (void)ctx;
    const bool has_record = f.enabled_ext && f.enabled_ext != 3 && f.enabled_ext != 4;
    const bool spliced = f.enabled_ext == 1 || f.enabled_ext == 2 || f.enabled_ext == 7;
    const bool custom = f.enabled_ext == 7;
    const xiaotai_link_media_t *v = &f.listener.profile.video;
    CHECK(status().video_declared == has_record);
    CHECK(v->send == has_record && v->receive == has_record && v->receive_request == has_record);
    if (has_record) {
        CHECK(v->peer_streams == video.peer_streams && v->send_format.codec == video.send_format.codec);
        CHECK(v->receive_formats[0].codec == video.receive_formats[0].codec);
        CHECK(v->send_stream == (custom ? 5 : 11) && v->receive_stream == (custom ? 8 : 15));
    }
    const unsigned ignored[] = {0, 0, 1, 2, 1, 0, 0, 0};
    CHECK(stats().extensions_ignored == ignored[f.enabled_ext]);
    char text[512];
    const int length = f.profile(text, sizeof(text) - 1U, f.profile_ctx);
    CHECK(length > 0 && length < (int)sizeof(text));
    text[length] = 0;
    xiaotai_json_token_t tokens[64];
    size_t count;
    xiaotai_json_doc_t doc;
    OK(xiaotai_json_parse(text, length, tokens, 64, &count, &doc));
    const int audio[] = {custom ? 4 : 10, custom ? 7 : 14, 8000, 1};
    const char *names[] = {"up_audio_streamid", "down_audio_streamid", "audio_rate", "audio_channels",
                          "up_audio_mt", "down_audio_mt", "up_video_mt", "down_video_mt", "no_video"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        CHECK(strstr(text, names[i]) && !strstr(strstr(text, names[i]) + strlen(names[i]), names[i]));
        if (i < 4) {
            int64_t value;
            OK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, names[i]), &value));
            CHECK(value == audio[i]);
        } else if (i < 6) {
            CHECK(xiaotai_json_equals(&doc, xiaotai_json_at(&doc, xiaotai_json_get(&doc, 0, names[i]), 0), "alaw"));
        }
    }
    bool no_video_value;
    OK(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "no_video"), &no_video_value));
    CHECK(no_video_value == !spliced);
    if (spliced) {
        int64_t up, down, rotation;
        OK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "up_video_streamid"), &up));
        OK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "down_video_streamid"), &down));
        OK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "camera_rotation"), &rotation));
        CHECK(up == v->send_stream && down == v->receive_stream && rotation == 270);
        CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "up_video_mt[0]"), "h264"));
        CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "down_video_mt[0]"), "mjpeg"));
    } else {
        CHECK(xiaotai_json_get(&doc, 0, "up_video_streamid") < 0 && xiaotai_json_get(&doc, 0, "down_video_streamid") < 0);
        CHECK(xiaotai_json_find(&doc, 0, "up_video_mt[0]") < 0 && xiaotai_json_find(&doc, 0, "down_video_mt[0]") < 0);
    }
    CHECK(f.part_calls == (has_record ? 1U : 0U));
    CHECK(stats().profile_overflows == (has_record && !spliced ? 1U : 0U));
    CHECK(f.profile(text, (size_t)length, f.profile_ctx) == length);
    CHECK(f.profile(text, 20, f.profile_ctx) < 0);
    if (spliced) {
        const int reduced = f.profile(text, 200, f.profile_ctx);
        CHECK(reduced > 0);
        text[reduced] = '\0';
        CHECK(strstr(text, "\"no_video\":true") && !strstr(text, "up_video_streamid"));
    }
}

static void test_profile(void)
{
    for (unsigned i = 0; i <= 7; ++i) {
        const xiaotai_stream_config_t config = {
            .up_audio_stream = 4, .down_audio_stream = 7, .up_video_stream = 5, .down_video_stream = 8,
        };
        setup(i == 7 ? &config : NULL, true, true, false, true, i);
        owner(test_profile_case, NULL);
        teardown();
    }
}

static void test_receive_policy(void)
{
    const xiaotai_stream_config_t pcma_only = {.receive_pcma_only = true};
    for (unsigned i = 0; i < 4U; ++i) {
        const bool restricted = (i & 1U) != 0, decoder = (i & 2U) != 0;
        setup(restricted ? &pcma_only : NULL, true, true, decoder, true, 0);
        const xiaotai_link_media_t *audio = &f.listener.profile.audio;
        CHECK(audio->receive && audio->receive_request && audio->receive_stream == 14U);
        CHECK(audio->receive_formats[0].codec == XIAOTAI_LINK_CODEC_G711A &&
              audio->receive_formats[0].rate_hz == 8000U && audio->receive_formats[0].channels == 1U);
        CHECK(audio->receive_formats[1].codec == (decoder && !restricted ? XIAOTAI_LINK_CODEC_OPUS : XIAOTAI_LINK_CODEC_NONE));
        CHECK(audio->send && audio->send_stream == 10U && audio->send_format.codec == XIAOTAI_LINK_CODEC_G711A &&
              audio->send_format.rate_hz == 8000U && audio->send_format.channels == 1U);
        owner(test_profile_case, NULL);
        teardown();
    }
}

static uint32_t tickets[XIAOTAI_STREAM_HOLDS_MAX];

static void take_holds(void *ctx)
{
    (void)ctx;
    f.pending = true;
    for (size_t i = 0; i < XIAOTAI_STREAM_HOLDS_MAX; ++i) {
        OK(xiaotai_stream_hold("privacy", &tickets[i]));
        CHECK(tickets[i] != 0);
        if (i) CHECK(tickets[i] != tickets[i - 1U]);
    }
    uint32_t extra = 0;
    CHECK(xiaotai_stream_hold("other", &extra) == XIAOTAI_ERR_NO_MEM && !extra);
}

static void release_hold(void *ctx) { xiaotai_stream_release((uint32_t)(uintptr_t)ctx); }

static void test_hold(void)
{
    setup(NULL, true, true, false, true, 0);
    open_link(55);
    owner(take_holds, NULL);
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && f.pending_cancelled == 1);
    CHECK(f.ends == 0 && status().phase == XIAOTAI_STREAM_ACTIVE && status().holds == XIAOTAI_STREAM_HOLDS_MAX);
    OK(xiaotai_stream_stop(55));
    drain();
    CHECK(status().closed == XIAOTAI_STREAM_CLOSED_HELD);
    for (size_t i = 0; i < XIAOTAI_STREAM_HOLDS_MAX; ++i) {
        CHECK(f.mode == XIAOTAI_LINK_LISTEN_OFF && status().holds == XIAOTAI_STREAM_HOLDS_MAX - i);
        owner(release_hold, (void *)(uintptr_t)tickets[i]);
        owner(release_hold, (void *)(uintptr_t)tickets[i]);
    }
    CHECK(f.mode == XIAOTAI_LINK_LISTEN_OPEN && !status().holds);
    teardown();
}

int main(int argc, char **argv)
{
    OK(xiaotai_signal_init(&barrier));
    if (argc == 1) test_admission();
    else if (!strcmp(argv[1], "session")) test_session();
    else if (!strcmp(argv[1], "profile")) test_profile();
    else if (!strcmp(argv[1], "receive_policy")) test_receive_policy();
    else if (!strcmp(argv[1], "hold")) test_hold();
    else if (!strcmp(argv[1], "preflight")) test_preflight();
    else CHECK(false);
    xiaotai_signal_deinit(&barrier);
    return 0;
}
