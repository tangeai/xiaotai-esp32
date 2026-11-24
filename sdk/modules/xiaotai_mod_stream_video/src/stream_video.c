// SPDX-License-Identifier: MIT
#include "internal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai/video_link.h"
#include "xiaotai/video_profiles.h"

static const xiaotai_video_profile_t s_h264 = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
static const xiaotai_video_profile_t s_jpeg = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
static const xiaotai_video_downlink_t s_compact = XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT;
static xiaotai_stream_video_config_t s_config;
static xiaotai_critical_t s_snapshot_lock;
static bool s_lock_initialized;
static atomic_bool s_initialized;
static xiaotai_stream_video_status_t s_status;
static xiaotai_stream_video_stats_t s_stats;
static uint32_t s_present_failures;
static uint32_t s_timer_failures;
static uint32_t s_activity;
static uint32_t s_retry;
static bool s_receiving_session;
static bool s_source_installed;
static bool s_remote_seen;
static const xiaotai_video_profile_t *s_uplink;
static const xiaotai_video_downlink_t *s_downlink;
static xiaotai_link_media_t s_link_video;
static stream_video_profile_t s_profile;
static const xiaotai_platform_scene_part_t s_scene = {
    .write = stream_video_profile_write, .ctx = &s_profile,
};
static const xiaotai_contribution_t s_contributions[] = {
    {XIAOTAI_LINK_SLOT_VIDEO, XIAOTAI_STREAM_PROFILE_SCENE, &s_link_video},
    {XIAOTAI_PLATFORM_SLOT_SCENE, XIAOTAI_STREAM_PROFILE_SCENE, &s_scene},
    {0},
};

static void lock(void) { xiaotai_critical_enter(&s_snapshot_lock); }
static void unlock(void) { xiaotai_critical_exit(&s_snapshot_lock); }

static void count(uint32_t *value)
{
    lock();
    ++*value;
    unlock();
}

xiaotai_err_t xiaotai_stream_video_configure(const xiaotai_stream_video_config_t *config)
{
    if (atomic_load(&s_initialized)) return XIAOTAI_ERR_INVALID_STATE;
    if (config && ((config->no_downlink && config->no_uplink) ||
                   (config->viewport && config->viewport->display != config->display) ||
                   !stream_video_profile_valid(config))) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    s_config = config ? *config : (xiaotai_stream_video_config_t){0};
    return XIAOTAI_OK;
}

void xiaotai_stream_video_status(xiaotai_stream_video_status_t *status)
{
    if (!status) return;
    if (!atomic_load(&s_initialized)) {
        *status = (xiaotai_stream_video_status_t){0};
        return;
    }
    lock();
    *status = s_status;
    unlock();
}

void xiaotai_stream_video_stats(xiaotai_stream_video_stats_t *stats)
{
    if (!stats) return;
    if (!atomic_load(&s_initialized)) {
        *stats = (xiaotai_stream_video_stats_t){0};
        return;
    }
    lock();
    *stats = s_stats;
    unlock();
}

static void report_remote(uint32_t generation, bool ready)
{
    const xiaotai_args_t args = {.value = ready};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, &args, &generation, sizeof(generation));
}

static void clear_source(uint32_t generation)
{
    if (!s_source_installed) return;
#ifdef XIAOTAI_STREAM_VIDEO_WITH_UI
    stream_video_unpresent(s_config.display, generation);
#else
    (void)generation;
#endif
    s_source_installed = false;
}

static void receive_peer(void *ctx)
{
    const uint32_t generation = (uint32_t)(uintptr_t)ctx;
    s_retry = 0;
    if (generation != s_status.generation || !s_receiving_session) return;

    const xiaotai_err_t error = xiaotai_link_receive(generation, XIAOTAI_LINK_VIDEO,
                                                     xiaotai_video_transport_receive, NULL);
    lock();
    ++s_stats.receive_requests;
    if (error == XIAOTAI_OK) s_status.receiving = true;
    else ++s_stats.receive_failures;
    unlock();
    if (error != XIAOTAI_OK && xiaotai_timer_start(1000, receive_peer, ctx, &s_retry) != XIAOTAI_OK) {
        count(&s_timer_failures);
    }
}

static void close_session(void)
{
    const uint32_t generation = s_status.generation;
    if (!generation) return;
    if (s_retry) xiaotai_timer_cancel(s_retry);
    s_retry = 0;
    if (s_receiving_session) xiaotai_link_receive_stop(generation, XIAOTAI_LINK_VIDEO);
    s_receiving_session = false;
    clear_source(generation);
    xiaotai_video_close(generation);
    const bool was_ready = s_status.remote_ready;
    lock();
    s_status.generation = 0;
    s_status.receiving = false;
    s_status.camera_enabled = false;
    s_status.remote_ready = false;
    s_status.remote_unsupported = false;
    s_status.uplink = XIAOTAI_VIDEO_OFF;
    s_status.uplink_error = XIAOTAI_OK;
    unlock();
    s_remote_seen = false;
    if (was_ready) report_remote(generation, false);
}

static void open_session(uint32_t generation)
{
    if (generation == s_status.generation) return;
    close_session();
    if (!generation) return;
    xiaotai_video_session_t session = {
        .generation = generation,
        .uplink = s_uplink,
        .downlink = s_status.downlink_offered ? s_downlink : NULL,
        .link = xiaotai_video_transport_link(),
        .camera = s_config.camera,
        .display = s_config.display,
        .placement = s_config.placement,
    };
    if (!session.uplink && !session.downlink) return;
    xiaotai_err_t error = xiaotai_video_open(&session);
    if (error != XIAOTAI_OK) count(&s_stats.session_failures);
    if (error == XIAOTAI_ERR_NOT_SUPPORTED && session.downlink && !s_config.viewport) {
        /* Another component was expected to declare this viewport. The
         * immutable contribution remains the build-time link declaration. */
        lock();
        s_status.downlink_offered = false;
        s_status.downlink_codec = XIAOTAI_VIDEO_CODEC_NONE;
        unlock();
        session.downlink = NULL;
        if (!session.uplink) return;
        error = xiaotai_video_open(&session);
        if (error != XIAOTAI_OK) count(&s_stats.session_failures);
    }
    if (error != XIAOTAI_OK) return; /* The host's audio session survives. */
    lock();
    s_status.generation = generation;
    s_status.camera_enabled = session.uplink != NULL;
    ++s_stats.sessions;
    unlock();
    s_receiving_session = session.downlink != NULL;
    if (s_receiving_session) receive_peer((void *)(uintptr_t)generation);
}

static void stream_begin(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    s_activity = id;
    open_session(generation);
}

static void stream_session(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    if (id == s_activity) open_session(generation);
}

static void stream_end(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    (void)generation;
    if (id != s_activity) return;
    close_session();
    s_activity = 0;
}

static void camera_switch(uint32_t generation, bool enabled)
{
    if (!generation || generation != s_status.generation) {
        count(&s_stats.stale);
        return;
    }
    if (!s_uplink || s_status.camera_enabled == enabled) return;
    if (xiaotai_video_set_camera_enabled(generation, enabled) != XIAOTAI_OK) {
        count(&s_stats.stale);
        return;
    }
    lock();
    s_status.camera_enabled = enabled;
    ++s_stats.camera_switches;
    unlock();
}

static void camera_on_job(void *ctx) { camera_switch((uint32_t)(uintptr_t)ctx, true); }
static void camera_off_job(void *ctx) { camera_switch((uint32_t)(uintptr_t)ctx, false); }

xiaotai_err_t xiaotai_stream_video_set_camera(uint32_t generation, bool enabled)
{
    if (!generation) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_stream_video_status_t status;
    xiaotai_stream_video_status(&status);
    if (!status.uplink_offered) return XIAOTAI_ERR_NOT_SUPPORTED;
    return xiaotai_run(enabled ? camera_on_job : camera_off_job, (void *)(uintptr_t)generation);
}

static void camera_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (args->value == 0 || args->value == 1) camera_switch(s_status.generation, args->value == 1);
}

static void video_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_video_event_t *data = event->size == sizeof(xiaotai_video_event_t) ? event->data : NULL;
    if (!data || !data->generation || data->generation != s_status.generation) {
        count(&s_stats.stale);
        return;
    }
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_UPLINK) == 0) {
        lock();
        s_status.uplink = data->phase;
        s_status.uplink_error = data->phase == XIAOTAI_VIDEO_ERROR ? data->error : XIAOTAI_OK;
        if (data->phase == XIAOTAI_VIDEO_ERROR) ++s_stats.uplink_errors;
        unlock();
        const xiaotai_args_t args = {.value = data->phase};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_STREAM_VIDEO_UPLINK, &args, data, sizeof(*data));
        return;
    }
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_REMOTE) == 0) {
        if (event->args.value != 0 && event->args.value != 1) return;
        const bool ready = event->args.value == 1;
        if (ready == s_status.remote_ready) return;
        if (ready) {
#ifdef XIAOTAI_STREAM_VIDEO_WITH_UI
            s_source_installed = stream_video_present(s_config.display, data->generation) == XIAOTAI_OK;
            if (!s_source_installed) count(&s_present_failures);
#endif
        } else {
            clear_source(data->generation);
        }
        lock();
        if (ready && !s_remote_seen) ++s_stats.remote_ready;
        s_status.remote_ready = ready;
        unlock();
        if (ready) s_remote_seen = true;
        report_remote(data->generation, ready);
        return;
    }
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED) == 0) {
        if (!s_status.remote_unsupported) return;
        lock();
        s_status.remote_unsupported = false;
        unlock();
        const xiaotai_args_t args = {.value = (int32_t)data->generation};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, &args, NULL, 0);
        return;
    }
    if (s_status.remote_unsupported) return;
    lock();
    s_status.remote_unsupported = true;
    ++s_stats.remote_unsupported;
    unlock();
    const xiaotai_args_t args = {.value = (int32_t)data->generation};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, &args, NULL, 0);
}

static void diagnostics(const xiaotai_reply_t *reply)
{
    const xiaotai_stream_video_status_t status = s_status;
    const xiaotai_stream_video_stats_t stats = s_stats;
    xiaotai_at_printf(reply, "+STREAM-VIDEO: generation=%" PRIu32 " uplink_offered=%u downlink_offered=%u"
                      " uplink=%u uplink_error=%d camera=%u receiving=%u remote=%u unsupported=%u",
                      status.generation, (unsigned)status.uplink_offered, (unsigned)status.downlink_offered,
                      (unsigned)status.uplink, status.uplink_error, (unsigned)status.camera_enabled,
                      (unsigned)status.receiving, (unsigned)status.remote_ready, (unsigned)status.remote_unsupported);
    xiaotai_at_printf(reply, "+STREAM-VIDEO-STATS: sessions=%" PRIu32 " session_failures=%" PRIu32
                      " uplink_errors=%" PRIu32 " receive_requests=%" PRIu32 " receive_failures=%" PRIu32
                      " remote_ready=%" PRIu32 " remote_unsupported=%" PRIu32 " camera_switches=%" PRIu32
                      " stale=%" PRIu32 " present_failures=%" PRIu32 " timer_failures=%" PRIu32,
                      stats.sessions, stats.session_failures, stats.uplink_errors, stats.receive_requests,
                      stats.receive_failures, stats.remote_ready, stats.remote_unsupported, stats.camera_switches,
                      stats.stale, s_present_failures, s_timer_failures);
}

static bool wire_supported(xiaotai_video_codec_t codec, uint32_t mask)
{
    return (codec == XIAOTAI_VIDEO_CODEC_H264 || codec == XIAOTAI_VIDEO_CODEC_JPEG) &&
           (mask & XIAOTAI_VIDEO_CODEC_BIT(codec));
}

static bool needs_send_buffer(const xiaotai_video_profile_t *profile)
{
    if (!profile) return false;
    /* Equivalent or smaller JPEG stream framing uses the default buffer,
     * even if the product declared a copy of the built-in profile. */
    return profile->codec != XIAOTAI_VIDEO_CODEC_JPEG || profile->max_frame_bytes > s_jpeg.max_frame_bytes;
}

xiaotai_err_t xiaotai_module_stream_video_init(void)
{
    if (atomic_load(&s_initialized)) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_lock_initialized) {
        xiaotai_critical_init(&s_snapshot_lock);
        s_lock_initialized = true;
    }
    xiaotai_video_capabilities_t caps = {0};
    xiaotai_video_capabilities(&caps);
    const xiaotai_video_profile_t *up = s_config.uplink ? s_config.uplink :
        (wire_supported(XIAOTAI_VIDEO_CODEC_H264, caps.uplink_codecs) ? &s_h264 : &s_jpeg);
    const xiaotai_video_downlink_t *down = s_config.downlink ? s_config.downlink : &s_compact;
    uint16_t width = 0, height = 0;
    s_uplink = !s_config.no_uplink && wire_supported(up->codec, caps.uplink_codecs) &&
        xiaotai_video_resolve(up, s_config.camera, &width, &height) == XIAOTAI_OK ? up : NULL;
    s_downlink = !s_config.no_downlink && wire_supported(down->codec, caps.downlink_codecs) ? down : NULL;
    s_status = (xiaotai_stream_video_status_t){
        .uplink_offered = s_uplink != NULL, .downlink_offered = s_downlink != NULL,
        .uplink_codec = s_uplink ? s_uplink->codec : XIAOTAI_VIDEO_CODEC_NONE,
        .downlink_codec = s_downlink ? s_downlink->codec : XIAOTAI_VIDEO_CODEC_NONE,
        .uplink_width = s_uplink ? width : 0, .uplink_height = s_uplink ? height : 0,
    };
    if (!s_uplink && !s_downlink) {
        xiaotai_err_t error = xiaotai_diag_register("stream_video", diagnostics);
        if (error == XIAOTAI_OK) atomic_store(&s_initialized, true);
        return error;
    }
    xiaotai_err_t error = XIAOTAI_OK;
    if (s_config.viewport) error = xiaotai_video_require_viewport(s_config.viewport);
    if (error == XIAOTAI_OK && s_uplink) error = xiaotai_video_require_uplink(s_uplink);
    if (error == XIAOTAI_OK && s_downlink) error = xiaotai_video_require_downlink(s_downlink);
    if (error == XIAOTAI_OK && needs_send_buffer(s_uplink)) {
        if (s_uplink->max_frame_bytes > UINT32_MAX / 2U) return XIAOTAI_ERR_INVALID_SIZE;
        const xiaotai_transport_requirement_t require = {.send_buffer_bytes = s_uplink->max_frame_bytes * 2U};
        error = xiaotai_transport_require(&require);
    }
    if (error != XIAOTAI_OK) return error;

    s_link_video = (xiaotai_link_media_t){
        .send = s_status.uplink_offered, .peer_streams = XIAOTAI_LINK_PEER_STREAM_REFUSE,
        .send_format = {.codec = s_status.uplink_codec},
        .receive = s_status.downlink_offered, .receive_request = true,
        .receive_formats = {{.codec = s_status.downlink_codec}},
    };
    s_profile = (stream_video_profile_t){
        .uplink = s_status.uplink_codec, .downlink = s_status.downlink_codec,
        .rotation = !s_uplink || s_uplink->upright ? 0 : caps.camera_rotation, .config = &s_config,
    };
    const xiaotai_attachment_t extension = {
        .begin = stream_begin, .end = stream_end, .session = stream_session, .contributions = s_contributions,
    };
    error = xiaotai_activity_attach(XIAOTAI_STREAM_ACTIVITY, &extension);
    if (error == XIAOTAI_OK) error = xiaotai_intent_handle(XIAOTAI_INTENT_STREAM_VIDEO_CAMERA, camera_intent, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_diag_register("stream_video", diagnostics);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UPLINK, video_event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_REMOTE, video_event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, video_event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, video_event, NULL);
    if (error == XIAOTAI_OK) atomic_store(&s_initialized, true);
    return error;
}

void xiaotai_stream_video_reset(void)
{
    atomic_store(&s_initialized, false);
    if (s_lock_initialized) lock();
    s_status = (xiaotai_stream_video_status_t){0};
    s_stats = (xiaotai_stream_video_stats_t){0};
    s_present_failures = 0;
    s_timer_failures = 0;
    if (s_lock_initialized) unlock();
    s_activity = s_retry = 0;
    s_source_installed = s_receiving_session = s_remote_seen = false;
    s_uplink = NULL;
    s_downlink = NULL;
    s_config = (xiaotai_stream_video_config_t){0};
}
