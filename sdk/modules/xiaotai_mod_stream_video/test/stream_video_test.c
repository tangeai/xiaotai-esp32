// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_camera_pacing.py
// The selected H5 JPEG uplink retains the 12 Hz ceiling.
#include "xiaotai/framework.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/stream_video.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai/ui.h"
#include "xiaotai/video_link.h"
#include "xiaotai/video_profiles.h"
#include "xiaotai_os_host.h"
#if SV_TEST_CASE == 4
#include "stream_video_matrix_fixture.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == XIAOTAI_OK)
#define H264 XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264)
#define JPEG XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG)

static struct {
    xiaotai_video_capabilities_t caps;
    const xiaotai_video_profile_t *uplink;
    const xiaotai_video_downlink_t *downlink;
    const xiaotai_video_viewport_t *viewport;
    xiaotai_video_session_t opened;
    const xiaotai_link_media_t *media;
    const xiaotai_platform_scene_part_t *part;
    xiaotai_platform_profile_fn_t writer;
    void *writer_ctx;
    xiaotai_err_t open_error, viewport_error, resolve_error;
    bool missing_viewport, ui_rejected;
    unsigned receive_failures, opens, closes, receives, stops, switches, sets, clears;
    unsigned order, installed_at, cleared_at, remote_at, remote, unsupported, recovered, uplink_events;
    unsigned requirements, overflows;
    int remote_value;
    uint32_t reserve, generation, active, shown_generation;
    char profile[512];
    int profile_size;
} f;
static xiaotai_signal_t barrier;

#if SV_TEST_CASE == 4
static uint32_t ai_permit;

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    CHECK(xiaotai_on_owner() && generation && kind == XIAOTAI_LINK_VIDEO && !f.active);
    CHECK(ai_permit == (allowed ? 0U : generation));
    ai_permit = allowed ? generation : 0U;
    return XIAOTAI_OK;
}
#endif

void xiaotai_video_capabilities(xiaotai_video_capabilities_t *caps) { *caps = f.caps; }
xiaotai_err_t xiaotai_video_resolve(const xiaotai_video_profile_t *p, size_t camera, uint16_t *width, uint16_t *height)
{
    CHECK(p && camera < 2);
    *width = p->width ? p->width : 960;
    *height = p->height ? p->height : 720;
    return f.resolve_error;
}
xiaotai_err_t xiaotai_video_require_uplink(const xiaotai_video_profile_t *p) { f.uplink = p; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_video_require_downlink(const xiaotai_video_downlink_t *p) { f.downlink = p; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_video_require_viewport(const xiaotai_video_viewport_t *p)
{
    f.viewport = p;
    return f.viewport_error;
}
xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *r)
{
    CHECK(r->links == 0 && r->command_pool_bytes == 0);
    f.reserve = r->send_buffer_bytes;
    ++f.requirements;
    return XIAOTAI_OK;
}

static const xiaotai_video_link_t link;
const xiaotai_video_link_t *xiaotai_video_transport_link(void) { return &link; }
bool xiaotai_video_transport_receive(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size)
{
    (void)ctx; (void)generation; (void)frame; (void)data; (void)size;
    return false;
}
xiaotai_err_t xiaotai_video_open(const xiaotai_video_session_t *session)
{
    CHECK(xiaotai_on_owner() && session->link == &link && !f.active);
    f.opened = *session;
    ++f.opens;
    if (f.missing_viewport && session->downlink) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (f.open_error != XIAOTAI_OK) return f.open_error;
    f.active = session->generation;
    return XIAOTAI_OK;
}
void xiaotai_video_close(uint32_t generation)
{
    CHECK(xiaotai_on_owner() && f.active == generation && !f.shown_generation);
    if (f.opened.downlink) CHECK(f.stops == f.closes + 1U);
    f.active = 0;
    ++f.closes;
}
xiaotai_err_t xiaotai_video_set_camera_enabled(uint32_t generation, bool enabled)
{
    CHECK(xiaotai_on_owner() && f.active == generation && f.opened.uplink);
    CHECK(enabled == !!(f.switches % 2U));
    ++f.switches;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t receiver, void *ctx)
{
    CHECK(xiaotai_on_owner() && generation == f.active && kind == XIAOTAI_LINK_VIDEO && !ctx);
    CHECK(receiver == xiaotai_video_transport_receive && f.opened.downlink);
    return ++f.receives <= f.receive_failures ? XIAOTAI_FAIL : XIAOTAI_OK;
}
void xiaotai_link_receive_stop(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    CHECK(xiaotai_on_owner() && generation == f.active && kind == XIAOTAI_LINK_VIDEO);
    ++f.stops;
}

xiaotai_err_t xiaotai_ui_video_show(uint8_t display, uint32_t generation)
{
    CHECK(xiaotai_on_owner() && display == f.opened.display && f.active == generation && !f.shown_generation);
    ++f.sets;
    f.installed_at = ++f.order;
    if (f.ui_rejected) return XIAOTAI_ERR_NOT_SUPPORTED;
    f.shown_generation = generation;
    return XIAOTAI_OK;
}
void xiaotai_ui_video_clear(uint8_t display, uint32_t generation)
{
    CHECK(xiaotai_on_owner() && display == f.opened.display && f.shown_generation == generation);
    f.shown_generation = 0;
    ++f.clears;
    f.cleared_at = ++f.order;
}
/* Link the actual ST-1 scene writer; these fakes are its host status/config
 * and the not-yet-implemented platform SV scene collector. */
xiaotai_err_t stream_profile_register(void);
const xiaotai_stream_config_t *stream_config(void)
{
    static const xiaotai_stream_config_t config = {
        .up_audio_stream = 10, .down_audio_stream = 14, .up_video_stream = 11, .down_video_stream = 15,
    };
    return &config;
}
bool stream_has_video(void) { return f.media != NULL; }
void stream_profile_overflow(void) { ++f.overflows; }
xiaotai_err_t xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t writer, void *ctx)
{
    CHECK(!strcmp(scene, "stream"));
    f.writer = writer;
    f.writer_ctx = ctx;
    return XIAOTAI_OK;
}
size_t xiaotai_platform_scene_parts(const char *activity, const char *scene, char *out, size_t capacity, size_t *length)
{
    const void *part = NULL;
    if (!xiaotai_activity_contributions(activity, XIAOTAI_PLATFORM_SLOT_SCENE, scene, &part, 1)) return 0;
    CHECK(part == f.part);
    if (*length >= capacity) return 0;
    const int n = f.part->write(out + *length + 1U, capacity - *length - 1U, f.part->ctx);
    if (n <= 0) return 0;
    out[*length] = ',';
    *length += 1U + (size_t)n;
    return 1;
}

static void observer(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_REMOTE)) {
        CHECK(event->size == sizeof(uint32_t) && *(const uint32_t *)event->data == f.generation);
        f.remote_value = event->args.value;
        f.remote_at = ++f.order;
        ++f.remote;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED)) {
        CHECK((uint32_t)event->args.value == f.generation);
        ++f.unsupported;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED)) {
        CHECK((uint32_t)event->args.value == f.generation);
        ++f.recovered;
    } else {
        CHECK(event->size == sizeof(xiaotai_video_event_t) && event->args.value == XIAOTAI_VIDEO_ERROR);
        CHECK(((const xiaotai_video_event_t *)event->data)->generation == f.generation);
        ++f.uplink_events;
    }
}
static xiaotai_err_t test_init(void)
{
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, observer, NULL));
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, observer, NULL));
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, observer, NULL));
    OK(xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_UPLINK, observer, NULL));
    return stream_profile_register();
}

static void sync(void *ctx) { (void)ctx; xiaotai_signal_give(&barrier); }

static void drain(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        OK(xiaotai_run(sync, NULL));
        CHECK(xiaotai_signal_take(&barrier, 3000));
    }
}
static void owner(void (*fn)(void *), void *ctx) { OK(xiaotai_run(fn, ctx)); drain(); }

static void contributions(void *ctx)
{
    (void)ctx;
    const void *media = NULL, *part = NULL;
    CHECK(xiaotai_activity_contributions("stream", XIAOTAI_LINK_SLOT_VIDEO, "stream", &media, 1) <= 1);
    CHECK(xiaotai_activity_contributions("stream", XIAOTAI_PLATFORM_SLOT_SCENE, "stream", &part, 1) <= 1);
    CHECK(!xiaotai_activity_contributions("stream", XIAOTAI_LINK_SLOT_VIDEO, "call", NULL, 0));
    f.media = media;
    f.part = part;
}

static xiaotai_err_t setup_modules(const xiaotai_stream_video_config_t *config,
                                 bool h5, const xiaotai_module_t *ai)
{
    static const xiaotai_module_t extension = {.name = "stream_video", .init = xiaotai_module_stream_video_init};
    static const xiaotai_module_t bench = {.name = "bench", .init = test_init};
    static const xiaotai_module_t *modules[3];
    static const xiaotai_priority_t priorities[] = {
        {XIAOTAI_STREAM_ACTIVITY, 1},
#if SV_TEST_CASE == 4
        {stream_video_matrix_extension_activity, 2},
#endif
        {NULL, 0},
    };
    size_t count = 0;
    if (h5) modules[count++] = &extension;
    if (ai) modules[count++] = ai;
    modules[count++] = &bench;
    const xiaotai_framework_config_t cfg = {
        .modules = modules, .module_count = count, .priorities = priorities, .available_needs = UINT32_MAX,
    };
    if (h5) OK(xiaotai_stream_video_configure(config));
    const xiaotai_err_t err = xiaotai_framework_start(&cfg);
    if (err == XIAOTAI_OK) owner(contributions, NULL);
    return err;
}

static xiaotai_err_t setup(const xiaotai_stream_video_config_t *config)
{
    return setup_modules(config, true, NULL);
}

static void reset(void)
{
    xiaotai_framework_reset();
    xiaotai_stream_video_reset();
    memset(&f, 0, sizeof(f));
}

static xiaotai_stream_video_status_t status(void)
{
    xiaotai_stream_video_status_t result;
    xiaotai_stream_video_status(&result);
    return result;
}
static xiaotai_stream_video_stats_t stats(void)
{
    xiaotai_stream_video_stats_t result;
    xiaotai_stream_video_stats(&result);
    return result;
}

static void test_offer(void)
{
    CHECK(xiaotai_stream_video_configure(&(xiaotai_stream_video_config_t){.no_uplink = true, .no_downlink = true})
          == XIAOTAI_ERR_INVALID_ARG);
    for (unsigned i = 0; i < 4; ++i) {
        const bool send = (i & 1U) != 0;
        const bool receive = (i & 2U) != 0;
        f.caps.uplink_codecs = send ? H264 | JPEG : 0;
        f.caps.downlink_codecs = receive ? H264 : 0;
        OK(setup(NULL));
        const xiaotai_stream_video_status_t s = status();
        CHECK(s.uplink_offered == send && s.downlink_offered == receive);
        CHECK(!!f.uplink == send && !!f.downlink == receive);
        CHECK(f.requirements == (unsigned)send && f.reserve == (send ? 512U * 1024U : 0));
        CHECK(!!f.media == (i != 0) && !!f.part == (i != 0));
        CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_STREAM_VIDEO_CAMERA) == (i != 0));
        if (f.media) {
            CHECK(f.media->send == send && f.media->receive == receive && f.media->receive_request);
            CHECK(f.media->peer_streams == XIAOTAI_LINK_PEER_STREAM_REFUSE);
            CHECK(!f.media->send_stream && !f.media->receive_stream);
            CHECK(f.media->send_format.codec == (send ? XIAOTAI_LINK_CODEC_H264 : XIAOTAI_LINK_CODEC_NONE));
            CHECK(!f.media->send_format.rate_hz && !f.media->send_format.channels);
            for (size_t k = 0; k < XIAOTAI_LINK_RECEIVE_FORMATS; ++k) {
                CHECK(f.media->receive_formats[k].codec == (receive && !k ? XIAOTAI_LINK_CODEC_H264 : XIAOTAI_LINK_CODEC_NONE));
                CHECK(!f.media->receive_formats[k].rate_hz && !f.media->receive_formats[k].channels);
            }
        }
        CHECK(xiaotai_stream_video_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
        reset();
    }
    f.caps.uplink_codecs = JPEG;
    f.caps.downlink_codecs = H264;
    OK(setup(&(xiaotai_stream_video_config_t){.no_downlink = true}));
    CHECK(f.uplink->codec == XIAOTAI_VIDEO_CODEC_JPEG && f.uplink->backlog_limit_bytes == 32768U);
    CHECK(status().uplink_width == 240 && status().uplink_height == 176 && !f.downlink && !f.reserve);
    reset();
    f.caps.uplink_codecs = JPEG;
    f.caps.downlink_codecs = JPEG;
    const xiaotai_video_downlink_t jpeg = XIAOTAI_VIDEO_DOWNLINK_JPEG(640, 480);
    const xiaotai_video_profile_t jpeg_uplink = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
    OK(setup(&(xiaotai_stream_video_config_t){.no_uplink = true, .downlink = &jpeg}));
    CHECK(!status().uplink_offered && status().downlink_offered);
    CHECK(f.downlink == &jpeg && f.media->receive_formats[0].codec == XIAOTAI_LINK_CODEC_JPEG);
    CHECK(xiaotai_stream_video_set_camera(1, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    reset();
    f.caps.uplink_codecs = JPEG;
    OK(setup(&(xiaotai_stream_video_config_t){.uplink = &jpeg_uplink}));
    CHECK(f.uplink == &jpeg_uplink && !f.requirements);
    reset();
    f.caps.uplink_codecs = JPEG;
    f.resolve_error = XIAOTAI_ERR_NOT_SUPPORTED;
    OK(setup(NULL));
    CHECK(!f.part && !f.media && !f.uplink);
    reset();
    f.caps.downlink_codecs = H264;
    f.viewport_error = XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_video_viewport_t viewport = {0};
    CHECK(xiaotai_stream_video_configure(&(xiaotai_stream_video_config_t){.viewport = &viewport, .display = 1})
          == XIAOTAI_ERR_INVALID_ARG);
    CHECK(setup(&(xiaotai_stream_video_config_t){.viewport = &viewport}) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(f.viewport == &viewport);
    reset();
}

static void begin(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t request = {.name = "stream", .generation = f.generation};
    uint32_t id = 0;
    OK(xiaotai_activity_begin(&request, &id));
    CHECK(id && xiaotai_activity_generation(id) == f.generation);
}
static void end(void *ctx)
{
    (void)ctx;
    const uint32_t id = xiaotai_activity_current(NULL);
    CHECK(id);
    xiaotai_activity_set_generation(id, 0);
    CHECK(!f.active);
    xiaotai_activity_end(id);
}
static void end_without_zero(void *ctx)
{
    (void)ctx;
    xiaotai_activity_end(xiaotai_activity_current(NULL));
    CHECK(!f.active);
}
static void signal_video(const char *name, int32_t value, uint32_t generation)
{
    const xiaotai_args_t args = {.value = value};
    xiaotai_video_event_t event = {.generation = generation};
    if (!strcmp(name, XIAOTAI_EVENT_VIDEO_UPLINK)) {
        event.phase = XIAOTAI_VIDEO_ERROR;
        event.error = XIAOTAI_FAIL;
    }
    OK(xiaotai_event_emit(name, &args, &event, sizeof(event)));
    drain();
}

static void test_session(void)
{
    const xiaotai_video_placement_t position = {.fit = XIAOTAI_VIDEO_FIT_COVER};
    const xiaotai_video_viewport_t viewport = {0};
    const xiaotai_stream_video_config_t cfg = {.viewport = &viewport, .placement = &position};
    f.caps.uplink_codecs = f.caps.downlink_codecs = H264;
    f.receive_failures = 1;
    OK(setup(&cfg));
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    CHECK(f.opens == 1 && f.receives == 1 && f.opened.placement == &position && f.viewport == &viewport);
    CHECK(!status().receiving && status().camera_enabled && status().generation == f.generation);
    xiaotai_os_host_advance_us(600000);
    drain();
    CHECK(f.receives == 1);
    xiaotai_os_host_advance_us(500000);
    drain();
    CHECK(f.receives == 2 && status().receiving);
    CHECK(stats().receive_failures == 1 && stats().receive_requests == 2);
    signal_video(XIAOTAI_EVENT_VIDEO_UPLINK, XIAOTAI_VIDEO_ERROR, f.generation);
    CHECK(f.uplink_events == 1 && status().uplink_error == XIAOTAI_FAIL && stats().uplink_errors == 1 && f.active);
    signal_video(XIAOTAI_EVENT_VIDEO_REMOTE, 1, f.generation + 1);
    CHECK(stats().stale == 1 && !status().remote_ready && !f.sets);
    signal_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 0, f.generation);
    signal_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 0, f.generation);
    CHECK(f.unsupported == 1 && stats().remote_unsupported == 1 && status().remote_unsupported);
    signal_video(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 0, f.generation);
    signal_video(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 0, f.generation);
    CHECK(f.recovered == 1 && !status().remote_unsupported);
    signal_video(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 0, f.generation);
    CHECK(f.unsupported == 2 && stats().remote_unsupported == 2 && status().remote_unsupported);
    CHECK(xiaotai_stream_video_set_camera(0, false) == XIAOTAI_ERR_INVALID_ARG);
    OK(xiaotai_stream_video_set_camera(f.generation + 1, false));
    drain();
    CHECK(stats().stale == 2 && !f.switches);
    OK(xiaotai_intent_post(XIAOTAI_INTENT_STREAM_VIDEO_CAMERA, &(xiaotai_args_t){.value = 0}));
    drain();
    CHECK(f.switches == 1 && !status().camera_enabled);
    OK(xiaotai_intent_post(XIAOTAI_INTENT_STREAM_VIDEO_CAMERA, &(xiaotai_args_t){.value = 1}));
    drain();
    CHECK(f.switches == 2 && status().camera_enabled);
    OK(xiaotai_stream_video_set_camera(f.generation, false));
    drain();
    CHECK(f.switches == 3 && !status().camera_enabled);
    owner(end, NULL);
    CHECK(f.closes == 1 && f.stops == 1 && !status().generation);
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    CHECK(f.opens == 2 && status().camera_enabled);
    owner(end, NULL);
    reset();

    f.caps.uplink_codecs = JPEG;
    f.caps.downlink_codecs = H264;
    f.missing_viewport = true;
    OK(setup(NULL));
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    CHECK(f.opens == 2 && !f.opened.placement && !f.opened.downlink && f.opened.uplink);
    CHECK(f.opened.uplink->codec == XIAOTAI_VIDEO_CODEC_JPEG && f.opened.uplink->fps == 12);
    CHECK(!status().downlink_offered && f.media->receive && stats().session_failures == 1);
    CHECK(!f.receives);
    owner(end, NULL);
    CHECK(f.closes == 1 && !f.stops);
    reset();

    f.caps.downlink_codecs = H264;
    f.receive_failures = 100;
    OK(setup(NULL));
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    CHECK(f.receives == 1 && !status().receiving);
    owner(end_without_zero, NULL);
    xiaotai_os_host_advance_us(2000000);
    drain();
    CHECK(f.receives == 1 && f.closes == 1 && f.stops == 1);
    reset();

    f.caps.uplink_codecs = JPEG;
    f.open_error = XIAOTAI_FAIL;
    OK(setup(NULL));
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    CHECK(f.opens == 1 && !status().generation && stats().session_failures == 1);
    owner(end, NULL);
    CHECK(!f.closes);
    reset();
}

static void test_present(void)
{
    f.caps.downlink_codecs = H264;
    OK(setup(&(xiaotai_stream_video_config_t){.display = 1}));
    f.generation = xiaotai_generation_next();
    owner(begin, NULL);
    signal_video(XIAOTAI_EVENT_VIDEO_REMOTE, 1, f.generation);
    CHECK(f.remote == 1 && f.remote_value == 1 && status().remote_ready);
    if (SV_TEST_UI) CHECK(f.sets == 1 && f.shown_generation == f.generation && f.installed_at < f.remote_at);
    else CHECK(!f.sets);
    signal_video(XIAOTAI_EVENT_VIDEO_REMOTE, 0, f.generation);
    CHECK(f.remote == 2 && f.remote_value == 0 && !status().remote_ready);
    if (SV_TEST_UI) CHECK(f.clears == 1 && f.cleared_at < f.remote_at && !f.shown_generation);
    signal_video(XIAOTAI_EVENT_VIDEO_REMOTE, 1, f.generation);
    owner(end, NULL);
    CHECK(f.remote == 4 && f.remote_value == 0 && !status().remote_ready);
    if (SV_TEST_UI) CHECK(f.clears == 2 && f.cleared_at < f.remote_at);
    else CHECK(!f.sets && !f.clears);
    CHECK(stats().remote_ready == 1);

    f.generation = xiaotai_generation_next();
    f.ui_rejected = true;
    owner(begin, NULL);
    signal_video(XIAOTAI_EVENT_VIDEO_REMOTE, 1, f.generation);
    CHECK(f.remote == 5 && status().remote_ready && !f.shown_generation);
    owner(end, NULL);
    CHECK(f.remote == 6 && f.remote_value == 0 && f.clears == (SV_TEST_UI ? 2U : 0U));
    reset();
}

static void write_profile(void *ctx)
{
    (void)ctx;
    f.profile_size = f.writer(f.profile, sizeof(f.profile) - 1U, f.writer_ctx);
    CHECK(f.profile_size >= 0 && (size_t)f.profile_size < sizeof(f.profile));
    f.profile[f.profile_size] = '\0';
}

#define AUDIO "{\"up_audio_streamid\":10,\"down_audio_streamid\":14,\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],\"audio_rate\":8000,\"audio_channels\":1,"
#define VIDEO_IDS ",\"up_video_streamid\":11,\"down_video_streamid\":15,\"no_video\":false}"
#define VIDEO_S3 "\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[],\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,\"aspect_ratio\":\"4:3\",\"object_fit\":\"contain\""
#define VIDEO_NONE "\"up_video_mt\":[],\"down_video_mt\":[],\"no_video\":true}"
#define VIDEO_P4(ratio) "\"up_video_mt\":[\"h264\"],\"down_video_mt\":[\"h264\"],\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,\"aspect_ratio\":" ratio ",\"object_fit\":\"contain\""

static void verify_profile(const char *part, const char *done)
{
    owner(write_profile, NULL);
    CHECK(strcmp(f.profile, done) == 0 && !f.overflows);
    if (!part) { CHECK(!f.part); return; }
    const size_t length = strlen(part);
    char output[200];
    memset(output, '!', sizeof(output));
    CHECK(f.part->write(output, length - 1U, f.part->ctx) < 0 && output[0] == '!');
    CHECK(f.part->write(output, length, f.part->ctx) == (int)length);
    CHECK(memcmp(output, part, length) == 0 && output[length] == '!');
}

static void test_profile(void)
{
    const char *bad[] = {NULL, "", "0", "00.1", ".1", "1.", "1,2", "1:2", "1e2", "1\"}", "-1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(xiaotai_stream_video_configure(&(xiaotai_stream_video_config_t){.aspect_ratio = bad[i],
              .aspect_ratio_number = true}) == XIAOTAI_ERR_INVALID_ARG);
    }
    CHECK(xiaotai_stream_video_configure(&(xiaotai_stream_video_config_t){.object_fit = "a\"b"}) == XIAOTAI_ERR_INVALID_ARG);
    OK(setup(NULL));
    verify_profile(NULL, AUDIO VIDEO_NONE);
    reset();
    f.caps.uplink_codecs = JPEG;
    OK(setup(NULL));
    verify_profile(VIDEO_S3, AUDIO VIDEO_S3 VIDEO_IDS);
    reset();
    f.caps.uplink_codecs = f.caps.downlink_codecs = H264;
    f.caps.camera_rotation = XIAOTAI_VIDEO_ROTATE_90;
    OK(setup(&(xiaotai_stream_video_config_t){.aspect_ratio = "3:4"}));
    verify_profile(VIDEO_P4("\"3:4\""), AUDIO VIDEO_P4("\"3:4\"") VIDEO_IDS);
    reset();
    f.caps.uplink_codecs = f.caps.downlink_codecs = H264;
    OK(setup(&(xiaotai_stream_video_config_t){.aspect_ratio = "1.7066666667", .aspect_ratio_number = true}));
    verify_profile(VIDEO_P4("1.7066666667"), AUDIO VIDEO_P4("1.7066666667") VIDEO_IDS);
    reset();
    f.caps.uplink_codecs = JPEG;
    f.caps.camera_rotation = XIAOTAI_VIDEO_ROTATE_270;
    OK(setup(&(xiaotai_stream_video_config_t){.aspect_ratio = "1.5", .object_fit = "cover", .mirror_horizontal = true}));
    verify_profile("\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[],\"camera_rotation\":270,\"hor_mirror\":true,\"vert_mirror\":false,\"aspect_ratio\":\"1.5\",\"object_fit\":\"cover\"",
                   AUDIO "\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[],\"camera_rotation\":270,\"hor_mirror\":true,\"vert_mirror\":false,\"aspect_ratio\":\"1.5\",\"object_fit\":\"cover\"" VIDEO_IDS);
    reset();
}

#if SV_TEST_CASE == 4
typedef struct { bool vision, offered; } camera_selection_t;

static void exercise_camera_owner(void *ctx)
{
    const camera_selection_t *selection = ctx;
    const char *name = selection->vision ? stream_video_matrix_extension_activity : XIAOTAI_STREAM_ACTIVITY;
    const size_t opens = f.opens, closes = f.closes;
    const xiaotai_activity_t request = {.name = name};
    uint32_t id = 0;
    OK(xiaotai_activity_begin(&request, &id));
    CHECK(id && f.opens == opens && !f.active && !ai_permit);
    const uint32_t generation = xiaotai_generation_next();
    xiaotai_activity_set_generation(id, generation);
    xiaotai_activity_set_generation(id, generation);
    CHECK(f.opens == opens + selection->offered);
    CHECK(f.active == (selection->offered ? generation : 0U));
    CHECK(ai_permit == (selection->vision && selection->offered ? generation : 0U));
    if (selection->offered) {
        CHECK(f.opened.uplink && !f.opened.downlink && !f.receives);
        CHECK(f.opened.uplink->codec == XIAOTAI_VIDEO_CODEC_JPEG);
        CHECK(f.opened.uplink->fps == (selection->vision ? 1 : 12));
    }
    xiaotai_activity_set_generation(id, 0);
    xiaotai_activity_end(id);
    CHECK(f.closes == closes + selection->offered && !f.active && !ai_permit);
}

static void verify_ai_contribution(void *ctx)
{
    const bool *expected = ctx;
    const void *data = NULL;
    CHECK(xiaotai_activity_contributions(stream_video_matrix_extension_activity, XIAOTAI_LINK_SLOT_VIDEO,
                                         stream_video_matrix_extension_scene, &data, 1) == (size_t)*expected);
    if (*expected) {
        const xiaotai_link_media_t *media = data;
        CHECK(media && media->send && !media->receive && media->send_stream == 0);
        CHECK(media->send_format.codec == XIAOTAI_LINK_CODEC_JPEG);
    }
    CHECK(!xiaotai_activity_contributions(stream_video_matrix_extension_activity, XIAOTAI_LINK_SLOT_VIDEO,
                                          XIAOTAI_STREAM_PROFILE_SCENE, NULL, 0));
    CHECK(!xiaotai_activity_contributions(XIAOTAI_STREAM_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                                          stream_video_matrix_extension_scene, NULL, 0));
}

/* Legacy H5 and AI camera flags were independent. Exercise each combination
 * with the real module set, real scene writers and an exclusive camera fake. */
static void test_ai_matrix(void)
{
    const xiaotai_stream_video_config_t s3 = {.no_downlink = true};
    for (unsigned i = 0; i < 4; ++i) {
        const bool h5_enabled = (i & 1U) != 0;
        bool ai_enabled = (i & 2U) != 0;
        f.caps.uplink_codecs = JPEG;
        OK(setup_modules(&s3, h5_enabled, ai_enabled ? &stream_video_matrix_extension : NULL));
        owner(verify_ai_contribution, &ai_enabled);
        verify_profile(h5_enabled ? VIDEO_S3 : NULL, h5_enabled ? AUDIO VIDEO_S3 VIDEO_IDS : AUDIO VIDEO_NONE);
        camera_selection_t h5 = {.offered = h5_enabled};
        camera_selection_t ai = {.vision = true, .offered = ai_enabled};
        owner(exercise_camera_owner, &ai);
        owner(exercise_camera_owner, &h5);
        owner(exercise_camera_owner, &ai);
        verify_profile(h5_enabled ? VIDEO_S3 : NULL, h5_enabled ? AUDIO VIDEO_S3 VIDEO_IDS : AUDIO VIDEO_NONE);
        reset();
        stream_video_matrix_extension_reset();
    }
}
#endif

int main(void)
{
    OK(xiaotai_signal_init(&barrier));
    if (SV_TEST_CASE == 0) test_offer();
    if (SV_TEST_CASE == 1) test_session();
    if (SV_TEST_CASE == 2) test_present();
    if (SV_TEST_CASE == 3) test_profile();
#if SV_TEST_CASE == 4
    test_ai_matrix();
#endif
    xiaotai_signal_deinit(&barrier);
    return 0;
}
