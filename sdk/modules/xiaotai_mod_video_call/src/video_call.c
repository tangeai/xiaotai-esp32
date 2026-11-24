// SPDX-License-Identifier: MIT
#include <string.h>
#include "video_call_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/platform.h"
#include "xiaotai/transport.h"
#include "xiaotai/video_link.h"
#include "xiaotai/video_profiles.h"

#define VIDEO_CALL_ACTIVITY "call"
#define VIDEO_CALL_SOURCE "video_call"
#define VIDEO_CALL_STREAM 11U
#define SUBSCRIBE_RETRY_MS 1000U

static const xiaotai_video_profile_t compact = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
static const xiaotai_video_profile_t sensor = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
static const xiaotai_video_call_scene_t default_scene;

typedef struct {
    video_call_scene_part_t part;
    xiaotai_platform_scene_part_t writer;
    xiaotai_video_downlink_t downlink;
    xiaotai_link_media_t media;
} scene_t;

static struct {
    xiaotai_video_call_config_t config;
    xiaotai_video_call_status_t status;
    scene_t scenes[XIAOTAI_VIDEO_CALL_SCENES_MAX];
    xiaotai_contribution_t contributions[2U * XIAOTAI_VIDEO_CALL_SCENES_MAX + 1U];
    size_t count;
    const scene_t *current;
    uint32_t activity;
    uint32_t retry;
    bool init;
    bool permitted;
    bool shown;
} s_video;

static bool video_supported(uint32_t codecs, xiaotai_video_codec_t codec)
{
    return (codecs & XIAOTAI_VIDEO_CODEC_BIT(codec)) != 0U;
}

static const xiaotai_video_call_scene_t *scene_config(const char *name)
{
    for (size_t i = 0; i < XIAOTAI_VIDEO_CALL_SCENES_MAX && s_video.config.scenes[i].scene; ++i) {
        if (strcmp(s_video.config.scenes[i].scene, name) == 0) return &s_video.config.scenes[i];
    }
    return &default_scene;
}

static xiaotai_video_downlink_t downlink_for(const video_call_host_t *host,
                                            const xiaotai_video_call_scene_t *scene)
{
    if (host->peer_codec == XIAOTAI_VIDEO_CODEC_H264) {
        const xiaotai_video_downlink_t desc = XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT;
        return desc;
    }
    const xiaotai_video_downlink_t desc = XIAOTAI_VIDEO_DOWNLINK_JPEG(
        scene->picture_width ? scene->picture_width : 640U, scene->picture_height ? scene->picture_height : 480U);
    return desc;
}

static void publish(const char *event, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    const xiaotai_err_t err = xiaotai_event_emit(event, &args, &s_video.status, sizeof(s_video.status));
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(VIDEO_CALL_SOURCE, "event", s_video.status.link, err);
}

static void publish_state(void)
{
    publish(XIAOTAI_EVENT_VIDEO_CALL_STATE, s_video.status.active ? 1 : 0);
}

static void report_error(const char *label, uint32_t link, xiaotai_err_t err)
{
    s_video.status.error = err;
    xiaotai_eventlog_error(VIDEO_CALL_SOURCE, label, link, err);
}

/* The engine copies the session description. The RTC receive slot and
 * presentation source belong to this attachment until the call lets it go. */
static void close_session(void)
{
    const uint32_t link = s_video.status.link;
    if (!link) return;
    xiaotai_timer_cancel(s_video.retry);
    s_video.retry = 0;
    if (s_video.shown) video_call_ui->clear(s_video.config.display, link);
    xiaotai_video_close(link);
    if (s_video.status.receiving) xiaotai_link_receive_stop(link, XIAOTAI_LINK_VIDEO);
    if (s_video.permitted) (void)xiaotai_link_permit(link, XIAOTAI_LINK_VIDEO, false);
    const bool remote = s_video.status.remote;
    s_video.shown = false;
    s_video.permitted = false;
    s_video.current = NULL;
    s_video.status.link = 0;
    s_video.status.active = false;
    s_video.status.camera_enabled = false;
    s_video.status.sending = false;
    s_video.status.receiving = false;
    s_video.status.remote = false;
    s_video.status.unsupported = false;
    if (remote) publish(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, 0);
    publish_state();
}

static void subscribe(bool retrying);

static void on_retry(void *ctx)
{
    (void)ctx;
    s_video.retry = 0;
    if (s_video.status.active) subscribe(true);
}

static void subscribe(bool retrying)
{
    const uint32_t link = s_video.status.link;
    if (retrying) ++s_video.status.subscribe_retries;
    xiaotai_err_t err = XIAOTAI_OK;
    if (!s_video.status.receiving) {
        err = xiaotai_link_receive(link, XIAOTAI_LINK_VIDEO, xiaotai_video_transport_receive, NULL);
        s_video.status.receiving = err == XIAOTAI_OK;
    }
    if (err == XIAOTAI_OK && s_video.current->part.host->subscribed) {
        err = s_video.current->part.host->subscribed(link);
    }
    if (err != XIAOTAI_OK) {
        report_error("subscribe", link, err);
        const xiaotai_err_t timer = xiaotai_timer_start(SUBSCRIBE_RETRY_MS, on_retry, NULL, &s_video.retry);
        if (timer != XIAOTAI_OK) report_error("retry_timer", link, timer);
    }
    if (retrying) publish_state();
}

static void open_session(const scene_t *scene, uint32_t link)
{
    const video_call_scene_part_t *part = &scene->part;
    const xiaotai_video_call_layout_t *layout = &part->scene->layout;
    const xiaotai_video_placement_t placement = {
        .fit = layout->fit, .rotation = layout->rotation, .match_orientation = layout->match_orientation,
    };
    s_video.current = scene;
    s_video.status.link = link;
    s_video.status.error = XIAOTAI_OK;
    if (part->uplink) {
        xiaotai_err_t err = xiaotai_link_permit(link, XIAOTAI_LINK_VIDEO, true);
        s_video.permitted = err == XIAOTAI_OK;
        if (!s_video.permitted) report_error("permit", link, err);
    }
    if (part->downlink) subscribe(false);
    const xiaotai_video_session_t session = {
        .generation = link,
        .uplink = s_video.permitted ? part->uplink : NULL,
        .downlink = part->downlink,
        .link = xiaotai_video_transport_link(),
        .camera = s_video.config.camera,
        .display = s_video.config.display,
        .placement = &placement,
    };
    const xiaotai_err_t err = session.uplink || session.downlink ? xiaotai_video_open(&session) : s_video.status.error;
    if (err != XIAOTAI_OK) {
        report_error("open", link, err);
        if (s_video.status.receiving) xiaotai_link_receive_stop(link, XIAOTAI_LINK_VIDEO);
        if (s_video.permitted) (void)xiaotai_link_permit(link, XIAOTAI_LINK_VIDEO, false);
        xiaotai_timer_cancel(s_video.retry);
        s_video.retry = 0;
        s_video.permitted = false;
        s_video.current = NULL;
        s_video.status.link = 0;
        s_video.status.receiving = false;
        publish_state();
        return;
    }
    if (part->downlink) {
        const xiaotai_err_t shown = video_call_ui->show(s_video.config.display, link);
        s_video.shown = shown == XIAOTAI_OK;
        if (!s_video.shown) xiaotai_eventlog_error(VIDEO_CALL_SOURCE, "show", link, shown);
    }
    ++s_video.status.sessions;
    s_video.status.active = true;
    s_video.status.camera_enabled = session.uplink != NULL;
    publish_state();
}

static void on_session(uint32_t id, uint32_t link, void *ctx)
{
    (void)ctx;
    if (!id || id != s_video.activity || s_video.status.link == link) return;
    close_session();
    if (!link) return;
    for (size_t i = 0; i < s_video.count; ++i) {
        const scene_t *scene = &s_video.scenes[i];
        if (scene->part.host->match(link)) {
            open_session(scene, link);
            break;
        }
    }
}

static void on_begin(uint32_t id, uint32_t link, void *ctx)
{
    s_video.activity = id;
    on_session(id, link, ctx);
}

static void on_end(uint32_t id, uint32_t link, void *ctx)
{
    (void)link;
    (void)ctx;
    if (id != s_video.activity) return;
    close_session();
    s_video.activity = 0;
}

static void on_video_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_video_event_t *data = event->data;
    if (!s_video.status.active || !data || event->size != sizeof(*data) ||
        data->generation != s_video.status.link) return;
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_REMOTE) == 0) {
        const bool remote = s_video.shown && event->args.value != 0;
        if (s_video.status.remote == remote) return;
        s_video.status.remote = remote;
        publish(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, remote ? 1 : 0);
    } else if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_UNSUPPORTED) == 0) {
        if (s_video.status.unsupported) return;
        s_video.status.unsupported = true;
        publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1);
    } else if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED) == 0) {
        if (!s_video.status.unsupported) return;
        s_video.status.unsupported = false;
        publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0);
    } else {
        s_video.status.sending = s_video.status.camera_enabled && data->phase == XIAOTAI_VIDEO_RUNNING;
        if (data->phase == XIAOTAI_VIDEO_ERROR) report_error("uplink", data->generation, data->error);
    }
    publish_state();
}

void xiaotai_video_call_status(xiaotai_video_call_status_t *status)
{
    if (status) *status = s_video.status;
}

xiaotai_err_t xiaotai_video_call_configure(const xiaotai_video_call_config_t *config)
{
    if (s_video.init) return XIAOTAI_ERR_INVALID_STATE;
    if (!config) {
        memset(&s_video.config, 0, sizeof(s_video.config));
        return XIAOTAI_OK;
    }
    for (size_t i = 0; i < XIAOTAI_VIDEO_CALL_SCENES_MAX && config->scenes[i].scene; ++i) {
        const xiaotai_video_call_scene_t *scene = &config->scenes[i];
        if (!scene->scene[0] || scene->camera_rotation > 270U || scene->camera_rotation % 90U != 0U ||
            (unsigned)scene->layout.rotation > XIAOTAI_VIDEO_ROTATE_270 ||
            (unsigned)scene->layout.fit > XIAOTAI_VIDEO_FIT_COVER || !video_call_profile_config_valid(scene)) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(scene->scene, config->scenes[j].scene) == 0) return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    s_video.config = *config;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_call_set_camera(uint32_t link, bool enabled)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (link && link != s_video.status.link) {
        ++s_video.status.stale_calls;
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (!s_video.status.active || !s_video.permitted) return XIAOTAI_ERR_INVALID_STATE;
    if (enabled == s_video.status.camera_enabled) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_video_set_camera_enabled(s_video.status.link, enabled);
    if (err == XIAOTAI_OK) {
        s_video.status.camera_enabled = enabled;
        if (!enabled) s_video.status.sending = false;
    } else {
        report_error("camera", s_video.status.link, err);
    }
    publish_state();
    return err;
}

static void on_camera(const xiaotai_args_t *args, void *ctx)
{
    const int *operation = ctx;
    (void)xiaotai_video_call_set_camera(args ? (uint32_t)args->value : 0U,
                                         *operation < 0 ? !s_video.status.camera_enabled : *operation != 0);
}

/* The codec bit describes the engine, not this camera/profile pair. A scene
 * whose uplink cannot open still offers its independently supported downlink. */
static bool can_send(const xiaotai_video_profile_t *profile, const char *label)
{
    uint16_t width, height;
    const xiaotai_err_t err = xiaotai_video_resolve(profile, s_video.config.camera, &width, &height);
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(VIDEO_CALL_SOURCE, label, 0, err);
    return err == XIAOTAI_OK;
}

static xiaotai_err_t declare_scene(const video_call_host_t *host, const xiaotai_video_profile_t *uplink, bool receive)
{
    if (s_video.count == XIAOTAI_VIDEO_CALL_SCENES_MAX) return XIAOTAI_ERR_NO_MEM;
    const size_t index = s_video.count;
    scene_t *scene = &s_video.scenes[index];
    const xiaotai_video_call_scene_t *config = scene_config(host->scene);
    if (receive) {
        scene->downlink = downlink_for(host, config);
        const xiaotai_err_t err = xiaotai_video_require_downlink(&scene->downlink);
        if (err != XIAOTAI_OK) return err;
    }
    scene->part = (video_call_scene_part_t){
        .host = host, .scene = config, .camera = s_video.config.camera,
        .uplink = uplink,
        .downlink = receive ? &scene->downlink : NULL,
    };
    const bool send = uplink != NULL;
    scene->media = (xiaotai_link_media_t){
        .send = send, .send_stream = send ? VIDEO_CALL_STREAM : 0,
        .send_format = {.codec = send ? XIAOTAI_LINK_CODEC_H264 : XIAOTAI_LINK_CODEC_NONE},
        .peer_streams = XIAOTAI_LINK_PEER_STREAM_REFUSE,
        .receive = receive, .receive_stream = receive ? VIDEO_CALL_STREAM : 0, .receive_request = receive,
        .accept_peer_stream = receive && config->accept_peer_video_stream,
        .receive_formats = {{.codec = receive ? host->peer_codec : XIAOTAI_LINK_CODEC_NONE}},
    };
    scene->writer = (xiaotai_platform_scene_part_t){.write = video_call_profile_write, .ctx = &scene->part};
    s_video.contributions[2U * index] = (xiaotai_contribution_t){
        .slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = host->scene, .data = &scene->media,
    };
    s_video.contributions[2U * index + 1U] = (xiaotai_contribution_t){
        .slot = XIAOTAI_PLATFORM_SLOT_SCENE, .scene = host->scene, .data = &scene->writer,
    };
    ++s_video.count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_module_video_call_init(void)
{
    if (s_video.init) return XIAOTAI_ERR_INVALID_STATE;
    s_video.init = true;
    xiaotai_video_capabilities_t caps = {0};
    xiaotai_video_capabilities(&caps);
    const bool h264 = video_supported(caps.uplink_codecs, XIAOTAI_VIDEO_CODEC_H264);
    const xiaotai_video_profile_t *compact_uplink = h264 && can_send(&compact, "resolve_compact") ? &compact : NULL;
    const xiaotai_video_profile_t *sensor_uplink = h264 && can_send(&sensor, "resolve_sensor") ? &sensor : NULL;
    xiaotai_video_viewport_t viewport;
    const bool display = video_call_ui && caps.downlink_codecs &&
                         xiaotai_video_viewport_for_display(s_video.config.display, &viewport) == XIAOTAI_OK;
    bool uplinks = false, downlinks = false;
    xiaotai_err_t err = XIAOTAI_OK;
    if (compact_uplink) err = xiaotai_video_require_uplink(compact_uplink);
    if (sensor_uplink && err == XIAOTAI_OK) err = xiaotai_video_require_uplink(sensor_uplink);
    for (size_t i = 0; video_call_hosts[i] && err == XIAOTAI_OK; ++i) {
        const video_call_host_t *host = video_call_hosts[i];
        const xiaotai_video_profile_t *uplink = host->peer_codec == XIAOTAI_VIDEO_CODEC_JPEG ? sensor_uplink : compact_uplink;
        const bool receive = display && video_supported(caps.downlink_codecs, host->peer_codec);
        if (uplink || receive) err = declare_scene(host, uplink, receive);
        uplinks |= uplink != NULL;
        downlinks |= receive;
    }
    if (downlinks && err == XIAOTAI_OK) err = xiaotai_video_require_viewport(&viewport);
    if (uplinks && err == XIAOTAI_OK) {
        const xiaotai_transport_requirement_t req = {.send_buffer_bytes = 2U * 1024U * 1024U};
        err = xiaotai_transport_require(&req);
    }
    if (err != XIAOTAI_OK) return err;
    if (!s_video.count) return video_call_at_init();
    const xiaotai_attachment_t attachment = {
        .begin = on_begin, .session = on_session, .end = on_end, .contributions = s_video.contributions,
    };
    err = xiaotai_activity_attach(VIDEO_CALL_ACTIVITY, &attachment);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UPLINK, on_video_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_REMOTE, on_video_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, on_video_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, on_video_event, NULL);
    static int on = 1, off = 0, toggle = -1;
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_VIDEO_CALL_CAMERA_ON, on_camera, &on);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_VIDEO_CALL_CAMERA_OFF, on_camera, &off);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_VIDEO_CALL_CAMERA_TOGGLE, on_camera, &toggle);
    if (err == XIAOTAI_OK) err = video_call_at_init();
    s_video.status.available = err == XIAOTAI_OK;
    return err;
}

void xiaotai_video_call_reset(void)
{
    close_session();
    memset(&s_video, 0, sizeof(s_video));
}
