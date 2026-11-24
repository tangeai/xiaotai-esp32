// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "video_call_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai/ui.h"
#include "xiaotai/video_link.h"

static unsigned up_requires, down_requires, display_requires, send_requires, opens, closes;
static unsigned receives, subscriptions, shows, clears, camera_switches, video_events, unsupported_events;
static unsigned supported_again;
static uint32_t activity, current_link;
static xiaotai_video_capabilities_t caps;
static bool use_display = true, phone, with_video, fail_receive, fail_subscribe, no_video_area, fail_open;
static bool missing_camera, unsupported_sensor;
static unsigned device_dials, wechat_dials;
static uint16_t phone_width = 1024, phone_height = 608;
static const xiaotai_video_viewport_t viewport = {.display = 2, .width = 480, .height = 640,
                                                  .pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .frames = 3};
static xiaotai_video_placement_t last_placement;
static xiaotai_video_profile_t last_profile;
static xiaotai_video_downlink_t last_downlink;
static char trace[128];
static size_t trace_len;
static xiaotai_signal_t sync;
static const xiaotai_video_link_t link;
static const char ratio[] = "\"3:4\"";
static const char ratio_wt99[] = "1.7066666667";
static const char *product_ratio = ratio;
static const size_t max_contrib = 2;

static xiaotai_video_call_status_t status(void)
{
    xiaotai_video_call_status_t out;
    xiaotai_video_call_status(&out);
    return out;
}

static void record(char operation)
{
    assert(trace_len + 1U < sizeof(trace));
    trace[trace_len++] = operation;
    trace[trace_len] = '\0';
}

void xiaotai_video_capabilities(xiaotai_video_capabilities_t *out) { *out = caps; }

xiaotai_err_t xiaotai_video_resolve(const xiaotai_video_profile_t *profile, size_t camera,
                                    uint16_t *width, uint16_t *height)
{
    assert(camera == 3 && profile->codec == XIAOTAI_VIDEO_CODEC_H264);
    if (missing_camera) return XIAOTAI_ERR_NOT_FOUND;
    if (unsupported_sensor && profile->upright) return XIAOTAI_ERR_NOT_SUPPORTED;
    *width = profile->width ? profile->width : 640;
    *height = profile->height ? profile->height : 480;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_require_uplink(const xiaotai_video_profile_t *profile)
{
    assert(profile->codec == XIAOTAI_VIDEO_CODEC_H264);
    ++up_requires;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_require_downlink(const xiaotai_video_downlink_t *down)
{
    if (down->codec == XIAOTAI_VIDEO_CODEC_H264) {
        assert(down->max_width == 384 && down->key_refresh_ms == 1000);
    } else {
        assert(down->codec == XIAOTAI_VIDEO_CODEC_JPEG && down->max_width == phone_width && down->max_height == phone_height);
    }
    ++down_requires;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_viewport_for_display(size_t display, xiaotai_video_viewport_t *out)
{
    assert(display == viewport.display);
    if (!use_display) return XIAOTAI_ERR_NOT_FOUND;
    *out = viewport;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_require_viewport(const xiaotai_video_viewport_t *view)
{
    assert(view->display == viewport.display && view->width == viewport.width && view->height == viewport.height);
    assert(view->pixel == viewport.pixel && view->frames == viewport.frames);
    assert(view->fit == XIAOTAI_VIDEO_FIT_CONTAIN && !view->match_orientation && view->rotation == XIAOTAI_VIDEO_ROTATE_0);
    ++display_requires;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *require)
{
    assert(require->send_buffer_bytes == 2U * 1024U * 1024U);
    ++send_requires;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_open(const xiaotai_video_session_t *session)
{
    assert(session->generation == current_link && session->camera == 3 && session->display == viewport.display);
    assert(session->link == &link);
    last_placement = *session->placement;
    last_profile = session->uplink ? *session->uplink : (xiaotai_video_profile_t){0};
    last_downlink = session->downlink ? *session->downlink : (xiaotai_video_downlink_t){0};
    ++opens;
    record('O');
    if (session->uplink) {
        uint16_t width, height;
        const xiaotai_err_t err = xiaotai_video_resolve(session->uplink, session->camera, &width, &height);
        if (err != XIAOTAI_OK) return err;
    }
    return fail_open ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
}

void xiaotai_video_close(uint32_t generation)
{
    assert(generation != 0);
    ++closes;
    record('C');
}

xiaotai_err_t xiaotai_video_set_camera_enabled(uint32_t generation, bool enabled)
{
    assert(generation == current_link);
    ++camera_switches;
    record(enabled ? '+' : '-');
    return XIAOTAI_OK;
}

const xiaotai_video_link_t *xiaotai_video_transport_link(void) { return &link; }

bool xiaotai_video_transport_receive(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size)
{
    (void)ctx; (void)generation; (void)frame; (void)data; (void)size;
    return false;
}

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    assert(generation != 0 && kind == XIAOTAI_LINK_VIDEO);
    record(allowed ? 'P' : 'p');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                  xiaotai_link_receive_fn_t callback, void *ctx)
{
    assert(generation == current_link && kind == XIAOTAI_LINK_VIDEO);
    assert(callback == xiaotai_video_transport_receive && !ctx);
    ++receives;
    record('R');
    return fail_receive ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}

void xiaotai_link_receive_stop(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    assert(generation != 0 && kind == XIAOTAI_LINK_VIDEO);
    record('r');
}

void xiaotai_call_status(xiaotai_call_status_t *out)
{
    *out = (xiaotai_call_status_t){
        .state = XIAOTAI_CALL_ACTIVE,
        .link = phone ? 0U : current_link,
        .flags = with_video ? XIAOTAI_CALL_VIDEO : 0U,
    };
}

void xiaotai_wechat_status(xiaotai_wechat_status_t *out)
{
    *out = (xiaotai_wechat_status_t){
        .state = XIAOTAI_WECHAT_ACTIVE,
        .link = phone ? current_link : 0U,
        .flags = with_video ? XIAOTAI_WECHAT_VIDEO : 0U,
    };
}

xiaotai_err_t xiaotai_call_dial_contact(size_t index, uint32_t flags, uint32_t *session)
{
    assert(xiaotai_on_owner() && flags == XIAOTAI_CALL_VIDEO && !session);
    ++device_dials;
    if (index == 42) return XIAOTAI_ERR_BUSY;
    return index == 0 ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_wechat_dial_contact(size_t index, uint32_t flags, uint32_t *session)
{
    assert(xiaotai_on_owner() && flags == XIAOTAI_WECHAT_VIDEO && !session);
    ++wechat_dials;
    return index == 1 ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_wechat_request_video(uint32_t generation)
{
    assert(generation == current_link && phone);
    ++subscriptions;
    record('W');
    return fail_subscribe ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ui_video_show(uint8_t display, uint32_t generation)
{
    assert(display == viewport.display && generation == current_link);
    ++shows;
    record('V');
    return no_video_area ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK;
}

void xiaotai_ui_video_clear(uint8_t display, uint32_t generation)
{
    assert(display == viewport.display && generation != 0);
    ++clears;
    record('v');
}

static void host_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id; (void)reason; (void)ctx;
    assert(!status().active && status().link == 0);
    record('X');
}

static void emit_video(const char *name, uint32_t generation, int32_t value, xiaotai_err_t error)
{
    const xiaotai_video_event_t data = {.generation = generation, .phase = (xiaotai_video_phase_t)value, .error = error};
    const xiaotai_args_t args = {.value = value};
    assert(xiaotai_event_emit(name, &args, &data, sizeof(data)) == XIAOTAI_OK);
}

static void count_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(xiaotai_video_call_status_t));
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_REMOTE) == 0) ++video_events;
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED) == 0) {
        if (event->args.value) ++unsupported_events;
        else ++supported_again;
    }
}

static xiaotai_err_t observer_init(void)
{
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, count_event, NULL) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, count_event, NULL);
}

static void begin(void)
{
    const xiaotai_activity_t host = {.name = "call", .resources = XIAOTAI_RESOURCE_LINK, .revoked = host_revoked};
    assert(xiaotai_activity_begin(&host, &activity) == XIAOTAI_OK);
}

static void generation(uint32_t value)
{
    current_link = value;
    xiaotai_activity_set_generation(activity, value);
}

static void check_contrib(void *accept_peer)
{
    const bool can_show = use_display && video_call_ui != NULL && caps.downlink_codecs != 0;
    size_t send_scenes = 0;
    size_t count = 0;
    for (; video_call_hosts[count]; ++count) {
        const video_call_host_t *host = video_call_hosts[count];
        const bool send = caps.uplink_codecs != 0 && !missing_camera &&
                          (!unsupported_sensor || host->peer_codec != XIAOTAI_VIDEO_CODEC_JPEG);
        if (send) ++send_scenes;
        const bool receive = can_show && (caps.downlink_codecs & XIAOTAI_VIDEO_CODEC_BIT(host->peer_codec));
        const void *parts[2] = {0};
        assert(xiaotai_activity_contributions("call", XIAOTAI_LINK_SLOT_VIDEO, host->scene, parts, max_contrib) ==
               (send || receive ? 1U : 0U));
        if (!send && !receive) continue;
        const xiaotai_link_media_t *media = parts[0];
        assert(media->send == send && media->peer_streams == XIAOTAI_LINK_PEER_STREAM_REFUSE);
        assert(media->send_stream == (send ? 11U : 0U) && media->send_format.rate_hz == 0 && !media->send_format.channels);
        assert(media->send_format.codec == (send ? XIAOTAI_LINK_CODEC_H264 : XIAOTAI_LINK_CODEC_NONE));
        assert(media->receive == receive && media->receive_request == receive);
        assert(media->receive_stream == (receive ? 11U : 0U));
        assert(media->accept_peer_stream == (receive && accept_peer && host->peer_codec == XIAOTAI_VIDEO_CODEC_JPEG));
        assert(media->receive_formats[0].codec == (receive ? host->peer_codec : XIAOTAI_LINK_CODEC_NONE));
        assert(xiaotai_activity_contributions("call", XIAOTAI_PLATFORM_SLOT_SCENE, host->scene, parts, max_contrib) == 1);
        const xiaotai_platform_scene_part_t *writer = parts[0];
        const video_call_scene_part_t *part = writer->ctx;
        assert(writer->write == video_call_profile_write && part->host == host);
        assert((part->uplink != NULL) == send && (part->downlink != NULL) == receive);
        assert(part->camera == 3 && part->scene->aspect_ratio_json ==
               (host->peer_codec == XIAOTAI_VIDEO_CODEC_JPEG ? product_ratio : NULL));
    }
    assert(send_requires == (send_scenes ? 1U : 0U));
    assert(up_requires == (caps.uplink_codecs && !missing_camera ? (unsupported_sensor ? 1U : 2U) : 0U));
    assert(down_requires == (can_show ? count : 0U) && display_requires == (can_show ? 1U : 0U));
}

static void check_peer_stream_contrib(void *unused)
{
    (void)unused;
    bool enabled = true;
    check_contrib(&enabled);
}

static void missing_uplink(void *unused)
{
    (void)unused;
    assert(video_call_ui != NULL && status().available);
    begin();
    phone = true;
    with_video = true;
    generation(601);
    assert(status().active && status().receiving && !status().camera_enabled && status().error == XIAOTAI_OK);
    assert(last_profile.codec == XIAOTAI_VIDEO_CODEC_NONE && last_downlink.codec == XIAOTAI_VIDEO_CODEC_JPEG);
    assert(strcmp(trace, "RWOV") == 0);
    generation(0);
    assert(strcmp(trace, "RWOVvCr") == 0);
    if (unsupported_sensor) {
        phone = false;
        generation(602);
        assert(status().active && status().camera_enabled && status().receiving);
        assert(last_profile.codec == XIAOTAI_VIDEO_CODEC_H264 && !last_profile.upright);
        assert(last_downlink.codec == XIAOTAI_VIDEO_CODEC_H264);
    }
    xiaotai_activity_end(activity);
}

static void session(void *unused)
{
    (void)unused;
    begin();
    assert(!opens && !status().active);
    generation(10);
    assert(!opens && !status().active);
    with_video = true;
    generation(11);
    assert(status().active && status().camera_enabled && status().link == 11U);
    assert(!last_profile.upright && last_profile.width == 384 && last_downlink.codec == XIAOTAI_VIDEO_CODEC_H264);
    assert(last_placement.fit == XIAOTAI_VIDEO_FIT_CONTAIN && last_placement.rotation == XIAOTAI_VIDEO_ROTATE_0);
    generation(0);
    assert(strcmp(trace, "PROVvCrp") == 0 && !status().active);
    phone = true;
    generation(12);
    assert(last_profile.upright && last_downlink.codec == XIAOTAI_VIDEO_CODEC_JPEG);
    assert(last_placement.fit == XIAOTAI_VIDEO_FIT_COVER && last_placement.rotation == XIAOTAI_VIDEO_ROTATE_90);
    assert(last_placement.match_orientation && last_downlink.max_width == 1024U);
    const xiaotai_activity_t emergency = {.name = "emergency", .resources = XIAOTAI_RESOURCE_LINK};
    uint32_t replacement;
    assert(xiaotai_activity_begin(&emergency, &replacement) == XIAOTAI_OK);
    assert(strcmp(trace, "PROVvCrpPRWOVvCrpX") == 0 && opens == 2 && closes == 2);
    xiaotai_activity_end(replacement);
    fail_open = true;
    phone = false;
    xiaotai_activity_end(activity);
    begin();
    generation(13);
    assert(!status().active && status().error == XIAOTAI_ERR_NO_MEM);
    assert(strstr(trace, "PROrp") && shows == 2U && clears == 2U && closes == 2U);
    xiaotai_activity_end(activity);
}

static void camera_start(void *unused)
{
    (void)unused;
    begin();
    with_video = true;
    generation(101);
    assert(xiaotai_video_call_set_camera(100, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(status().stale_calls == 1 && camera_switches == 0);
    emit_video(XIAOTAI_EVENT_VIDEO_UPLINK, 101, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    emit_video(XIAOTAI_EVENT_VIDEO_REMOTE, 101, 1, XIAOTAI_OK);
    emit_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 100, 1, XIAOTAI_OK);
    emit_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 101, 1, XIAOTAI_OK);
    emit_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 101, 1, XIAOTAI_OK);
}

static void camera_switch(void *unused)
{
    (void)unused;
    assert(status().remote && status().sending && video_events == 1 && unsupported_events == 1);
    assert(xiaotai_video_call_set_camera(0, false) == XIAOTAI_OK);
    assert(!status().camera_enabled && !status().sending && status().receiving && status().remote);
    assert(xiaotai_video_call_set_camera(101, true) == XIAOTAI_OK && camera_switches == 2);
    assert(status().camera_enabled && receives == 1 && opens == 1);
    emit_video(XIAOTAI_EVENT_VIDEO_UPLINK, 101, XIAOTAI_VIDEO_ERROR, XIAOTAI_ERR_TIMEOUT);
    emit_video(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 100, 0, XIAOTAI_OK);
}

static void camera_format_recover(void *unused)
{
    (void)unused;
    assert(status().unsupported && !supported_again); /* Unrelated generation. */
    emit_video(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 101, 0, XIAOTAI_OK);
    emit_video(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 101, 0, XIAOTAI_OK);
}

static void camera_format_changes(void *unused)
{
    (void)unused;
    assert(!status().unsupported && supported_again == 1);
    emit_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 101, 0, XIAOTAI_ERR_NOT_SUPPORTED);
}

static void camera_end(void *unused)
{
    (void)unused;
    assert(status().active && status().error == XIAOTAI_ERR_TIMEOUT);
    assert(status().unsupported && unsupported_events == 2 && supported_again == 1);
    xiaotai_activity_end(activity);
    begin();
    generation(102);
    assert(status().camera_enabled && !status().unsupported && status().error == XIAOTAI_OK);
    xiaotai_activity_end(activity);
    assert(video_events == 1 || video_events == 2);
}

static void subscribe_begin(void *unused)
{
    (void)unused;
    begin();
    phone = true;
    with_video = true;
    fail_receive = true;
    fail_subscribe = true;
    no_video_area = true;
    generation(301);
    assert(receives == 1 && subscriptions == 0 && opens == 1 && status().active && !status().receiving);
    fail_receive = false;
}

static void subscribe_recover(void *unused)
{
    (void)unused;
    assert(status().active && status().receiving && subscriptions >= 1 && receives == 2);
    fail_subscribe = false;
}

static void subscribe_end(void *unused)
{
    (void)unused;
    assert(receives == 2 && subscriptions >= 2 && status().subscribe_retries >= 2);
    assert(!status().remote);
    emit_video(XIAOTAI_EVENT_VIDEO_REMOTE, 301, 1, XIAOTAI_OK);
    xiaotai_activity_end(activity);
    assert(clears == 0U && closes == 1U);
}

static void unavailable(void *unused)
{
    (void)unused;
    assert(!status().available);
    begin();
    with_video = true;
    generation(401);
    assert(!status().active && !opens);
    xiaotai_activity_end(activity);
}

static void check_selection(void *unused)
{
    (void)unused;
    bool device_built = false, phone_built = false;
    for (size_t i = 0; video_call_hosts[i]; ++i) {
        device_built |= strcmp(video_call_hosts[i]->scene, XIAOTAI_CALL_SCENE) == 0;
        phone_built |= strcmp(video_call_hosts[i]->scene, XIAOTAI_WECHAT_SCENE) == 0;
    }
    const bool display_built = video_call_ui != NULL;
#if defined(TEST_SELECT)
#  if defined(TEST_WITH_CALL)
    assert(device_built);
#  else
    assert(!device_built);
#  endif
#  if defined(TEST_WITH_WECHAT)
    assert(phone_built);
#  else
    assert(!phone_built);
#  endif
#  if defined(TEST_WITH_UI)
    assert(display_built);
#  else
    assert(!display_built);
#  endif
#else
    assert(device_built && phone_built && display_built);
#endif
    const bool select_phone = !device_built && phone_built;
    phone = select_phone;
    with_video = true;
    begin();
    generation(501);
    assert(status().active && status().camera_enabled && opens == 1);
    assert(shows == (display_built ? 1U : 0U) && status().receiving == display_built);
    assert(last_profile.upright == select_phone);
    xiaotai_activity_end(activity);
}

static void config_tests(xiaotai_video_call_config_t config)
{
    config.scenes[1] = config.scenes[0];
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].scene = "";
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].scene = "call";
    config.scenes[1].camera_rotation = 91;
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].camera_rotation = 270;
    config.scenes[1].layout.rotation = (xiaotai_video_rotation_t)4;
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].layout.rotation = XIAOTAI_VIDEO_ROTATE_0;
    config.scenes[1].aspect_ratio_json = "\"3:4\",\"up_video_mt\":[\"fake\"]";
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].aspect_ratio_json = "true";
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.scenes[1].aspect_ratio_json = ratio;
    config.scenes[1].video_res_mode = "auto\"";
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
}

static void check_codec(const xiaotai_json_doc_t *doc, const char *key, const char *codec, bool arrays)
{
    const int index = xiaotai_json_get(doc, 0, key);
    assert(index >= 0);
    if (!arrays) {
        assert(xiaotai_json_equals(doc, index, codec ? codec : "none"));
        return;
    }
    assert(doc->tokens[index].type == XIAOTAI_JSON_ARRAY && doc->tokens[index].children == (codec ? 1 : 0));
    if (codec) assert(xiaotai_json_equals(doc, xiaotai_json_at(doc, index, 0), codec));
}

static void check_profile(void *unused)
{
    (void)unused;
    const char *const scenes[] = {XIAOTAI_CALL_SCENE, XIAOTAI_WECHAT_SCENE};
    for (unsigned i = 0; i < 2; ++i) {
        const bool arrays = i == 0;
        const void *parts[1];
        assert(xiaotai_activity_contributions("call", XIAOTAI_PLATFORM_SLOT_SCENE, scenes[i], parts, 1) == 1);
        const xiaotai_platform_scene_part_t *writer = parts[0];
        char text[512], exhausted[512];
        text[0] = '{';
        int n = writer->write(text + 1, sizeof(text) - 2, writer->ctx);
        assert(n > 0 && (size_t)n < sizeof(text) - 2);
        text[1 + n] = '}';
        memset(exhausted, '!', sizeof(exhausted));
        assert(writer->write(exhausted, (size_t)n - 1, writer->ctx) < 0 && exhausted[0] == '!');
        assert(writer->write(exhausted, 0, writer->ctx) < 0);
        assert(writer->write(exhausted, (size_t)n, writer->ctx) == n);
        assert(!memcmp(exhausted, text + 1, n) && exhausted[n] == '!');

        xiaotai_json_token_t tokens[32];
        xiaotai_json_doc_t doc;
        size_t count;
        assert(xiaotai_json_parse(text, (size_t)n + 2, tokens, 32, &count, &doc) == XIAOTAI_OK);
        assert(tokens[0].children == (arrays ? 6U : 8U) + (missing_camera ? 0U : 1U));
        check_codec(&doc, "up_video_mt", missing_camera ? NULL : "h264", arrays);
        check_codec(&doc, "down_video_mt", !use_display ? NULL : arrays ? "h264" : "mjpeg", arrays);

        int64_t rotation;
        assert(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "camera_rotation"), &rotation) == XIAOTAI_OK);
        assert(rotation == (!arrays && product_ratio == ratio ? 270 : 0));
        const int aspect = xiaotai_json_get(&doc, 0, "aspect_ratio");
        if (missing_camera) {
            assert(aspect == -1);
        } else if (arrays) {
            assert(xiaotai_json_equals(&doc, aspect, "3:2"));
        } else if (product_ratio == ratio_wt99) {
            size_t len;
            const char *raw = xiaotai_json_raw(&doc, aspect, &len);
            assert(aspect >= 0 && tokens[aspect].type == XIAOTAI_JSON_NUMBER);
            assert(raw && len == strlen(ratio_wt99) && memcmp(raw, ratio_wt99, len) == 0);
        } else {
            assert(xiaotai_json_equals(&doc, aspect, product_ratio ? "3:4" : "4:3"));
        }
        const int down_rotate = xiaotai_json_get(&doc, 0, "down_video_rotation");
        int mode_field = xiaotai_json_get(&doc, 0, "video_res_mode");
        if (arrays) {
            assert(down_rotate == -1 && mode_field == -1);
        } else {
            assert(xiaotai_json_int(&doc, down_rotate, &rotation) == XIAOTAI_OK && rotation == 0);
            assert(xiaotai_json_equals(&doc, mode_field, product_ratio == ratio_wt99 ? "fit_screen" : "auto"));
        }
        bool mirror;
        assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "hor_mirror"), &mirror) == XIAOTAI_OK && !mirror);
        assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "vert_mirror"), &mirror) == XIAOTAI_OK && !mirror);
        assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "object_fit"), "contain"));
        assert(xiaotai_json_get(&doc, 0, "no_video") < 0 && xiaotai_json_get(&doc, 0, "up_video_streamid") < 0);
    }
}

static char command_reply[1200];

static void write_line(void *unused, const char *line)
{
    (void)unused;
    assert(strlen(command_reply) + strlen(line) + 2 <= sizeof(command_reply));
    strcat(command_reply, line);
    strcat(command_reply, "\n");
}

static const xiaotai_reply_t reply = {.write = write_line};

static xiaotai_err_t command(const char *line)
{
    command_reply[0] = '\0';
    return xiaotai_command_run(line, &reply, 1000);
}

static xiaotai_err_t peer_status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(out, "+CALL: kind=device");
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t register_peer(void)
{
    static const xiaotai_at_verb_t verbs[] = {{.run = peer_status}};
    return xiaotai_at_verbs_register("AT+CALL", verbs, 1);
}

static void at_begin(void *unused)
{
    (void)unused;
    begin();
    with_video = true;
    generation(733);
}

static void at_report_end(void *unused)
{
    (void)unused;
    assert(status().active && status().camera_enabled && status().receiving);
    assert(camera_switches == 2);
    xiaotai_diag_report("video_call", &reply);
    xiaotai_activity_end(activity);
}

static void run_on_owner(void (*fn)(void *));

static void run_at(void)
{
    assert(command("AT+CALL") == XIAOTAI_OK && strstr(command_reply, "+CALL-VIDEO: video=0 camera=0 available=1"));
    assert(strstr(command_reply, "+CALL: kind=device"));
    assert(command("AT+CALL VIDEO 0") == XIAOTAI_OK && device_dials == 1 && wechat_dials == 0);
    assert(command("AT+CALL VIDEO 1") == XIAOTAI_OK && device_dials == 2 && wechat_dials == 1);
    assert(command("AT+CALL VIDEO 42") == XIAOTAI_ERR_BUSY && device_dials == 3 && wechat_dials == 1);
    assert(command("AT+CALL VIDEO 255") == XIAOTAI_ERR_NOT_FOUND && device_dials == 4 && wechat_dials == 2);
    assert(command("AT+CALL VIDEO -1") == XIAOTAI_ERR_INVALID_ARG);
    assert(command("AT+CALL VIDEO 256") == XIAOTAI_ERR_INVALID_ARG);
    assert(command("AT+CALL VIDEO") == XIAOTAI_ERR_INVALID_ARG);
    assert(command("AT+CALL VIDEO 0 extra") == XIAOTAI_ERR_INVALID_ARG && device_dials == 4 && wechat_dials == 2);
    assert(command("AT+CALL CAMERA 1") == XIAOTAI_ERR_INVALID_STATE);
    run_on_owner(at_begin);
    assert(command("AT+CALL CAMERA 00") == XIAOTAI_ERR_INVALID_ARG);
    assert(command("AT+CALL CAMERA 0") == XIAOTAI_OK);
    assert(command("AT+CALL CAMERA 0") == XIAOTAI_OK);
    assert(command("AT+CALL") == XIAOTAI_OK && strstr(command_reply, "+CALL-VIDEO: video=1 camera=0 available=1 gen=733"));
    assert(strstr(command_reply, "sending=0 receiving=1"));
    assert(command("AT+CALL CAMERA 1") == XIAOTAI_OK);
    command_reply[0] = '\0';
    run_on_owner(at_report_end);
    assert(strstr(command_reply, "video=1 camera=1") && strstr(command_reply, "sessions=1"));
    assert(command("AT+CALL=CAMERA,0") == XIAOTAI_ERR_NOT_FOUND);
    assert(command("AT+VIDCALL=0") == XIAOTAI_ERR_NOT_FOUND);
}

static void sync_give(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&sync);
}

static void run_on_owner(void (*fn)(void *))
{
    assert(xiaotai_run(fn, NULL) == XIAOTAI_OK);
    for (unsigned i = 0; i < 3U; ++i) {
        assert(xiaotai_run(sync_give, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&sync, 3000));
    }
}

static void restart(void) {}

int main(int argc, char **argv)
{
#if defined(TEST_SELECT)
    const char *scenario = argc == 1 ? "select" : argv[1];
#else
    const char *scenario = argc == 1 ? "session" : argv[1];
#endif
    if (strcmp(scenario, "unavailable") != 0) {
        caps.uplink_codecs = XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264);
        caps.downlink_codecs = caps.uplink_codecs | XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG);
    }
    use_display = strcmp(scenario, "configure") != 0;
    missing_camera = strcmp(scenario, "missing_camera") == 0 || strcmp(scenario, "profile_receive_only") == 0;
    unsupported_sensor = strcmp(scenario, "unsupported_sensor") == 0;
    const bool profile_case = strncmp(scenario, "profile", 7) == 0;
    const bool at_case = strcmp(scenario, "at") == 0;
    const bool accept_peer = strcmp(scenario, "peer_streams") == 0;
    if (strcmp(scenario, "profile_headless") == 0) use_display = false;
    if (strcmp(scenario, "profile_wt99") == 0) product_ratio = ratio_wt99;
    if (strcmp(scenario, "profile_default") == 0 || strcmp(scenario, "profile_receive_only") == 0) product_ratio = NULL;
    if (profile_case && product_ratio != ratio_wt99) {
        phone_width = 640;
        phone_height = 480;
    }
    xiaotai_video_call_config_t config = {
        .display = viewport.display, .camera = 3,
        .scenes = {{
            .scene = XIAOTAI_WECHAT_SCENE, .picture_width = phone_width, .picture_height = phone_height,
            .layout = {.fit = XIAOTAI_VIDEO_FIT_COVER, .rotation = XIAOTAI_VIDEO_ROTATE_90, .match_orientation = true},
            .aspect_ratio_json = product_ratio,
            .accept_peer_video_stream = accept_peer,
        }},
    };
    if (profile_case) {
        config.scenes[0].camera_rotation = product_ratio == ratio ? 270 : 0;
        config.scenes[0].video_res_mode = product_ratio == ratio_wt99 ? "fit_screen" : NULL;
        if (product_ratio != ratio) config.scenes[0].layout = (xiaotai_video_call_layout_t){0};
        if (product_ratio != ratio_wt99) config.scenes[0].picture_width = config.scenes[0].picture_height = 0;
    }
    if (strcmp(scenario, "configure") == 0) config_tests(config);
    assert(xiaotai_video_call_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&sync) == XIAOTAI_OK);
    xiaotai_eventlog_init(NULL);
    static const xiaotai_module_t subject = {.name = "video_call", .init = xiaotai_module_video_call_init};
    static const xiaotai_module_t observer = {.name = "observer", .init = observer_init};
    static const xiaotai_module_t peer = {.name = "peer_at", .init = register_peer};
    const xiaotai_module_t *modules[] = {&subject, &observer, &peer};
    const xiaotai_priority_t priorities[] = {{"call", 3}, {"emergency", 4}, {NULL, 0}};
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = at_case ? 3 : 2, .priorities = priorities, .restart = restart,
    };
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    assert(xiaotai_video_call_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    if (!profile_case && (missing_camera || unsupported_sensor)) run_on_owner(missing_uplink);
    run_on_owner(accept_peer ? check_peer_stream_contrib : check_contrib);
    if (strcmp(scenario, "session") == 0) {
        run_on_owner(session);
    } else if (strcmp(scenario, "subscribe") == 0) {
        run_on_owner(subscribe_begin);
        xiaotai_delay_ms(1200);
        run_on_owner(subscribe_recover);
        xiaotai_delay_ms(1200);
        run_on_owner(subscribe_end);
    } else if (strcmp(scenario, "camera") == 0) {
        run_on_owner(camera_start);
        run_on_owner(camera_switch);
        run_on_owner(camera_format_recover);
        run_on_owner(camera_format_changes);
        run_on_owner(camera_end);
        assert(video_events == 2);
    } else if (strcmp(scenario, "unavailable") == 0) {
        run_on_owner(unavailable);
        assert(command("AT+CALL") == XIAOTAI_OK && strstr(command_reply, "available=0"));
        assert(command("AT+CALL VIDEO 0") == XIAOTAI_ERR_NOT_SUPPORTED && !device_dials && !wechat_dials);
    } else if (strcmp(scenario, "select") == 0) {
        run_on_owner(check_selection);
        for (size_t i = 0; video_call_hosts[i]; ++i) {
            const bool wechat = video_call_hosts[i]->peer_codec == XIAOTAI_VIDEO_CODEC_JPEG;
            assert(command(wechat ? "AT+CALL VIDEO 1" : "AT+CALL VIDEO 0") == XIAOTAI_OK);
        }
    } else if (profile_case) {
        run_on_owner(check_profile);
    } else if (at_case) {
        run_at();
    }
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_video_call_reset();
    printf("mod_video_call %s: ok\n", scenario);
    return 0;
}
