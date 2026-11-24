// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_h5_video_send.py
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
#include <assert.h>
#include <string.h>
#include "fakes.h"
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/video_profiles.h"

/* Drive the real session step with an exact monotonic clock. The camera has
 * an explicit acknowledgement so it can remain busy beyond four seconds. */
void __wrap_video_session_task(void *arg);
void __wrap_video_uplink_task(void *arg);
bool __wrap_video_uplink_prepare_boot(void);

void __wrap_video_session_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

void __wrap_video_uplink_task(void *arg) { __wrap_video_session_task(arg); }
bool __wrap_video_uplink_prepare_boot(void) { return true; }

static int64_t now = 1000000;
static unsigned begins, stops, ranges, takes, keys, received;
static uint32_t revision, started_generation;
static uint32_t bitrate_since, wanted;
static xiaotai_err_t begin_result = XIAOTAI_OK, range_result = XIAOTAI_ERR_NOT_SUPPORTED;
static uint8_t owned[4];
static bool input_room = true;
static unsigned remote_on, unsupported_events;
static bool started_before_running, saw_start;
static xiaotai_signal_t owner_done;

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    assert(event->size == sizeof(xiaotai_video_event_t));
    const xiaotai_video_event_t *payload = event->data;
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_REMOTE) == 0 && event->args.value == 1) {
        assert(payload->generation == 7);
        ++remote_on;
    } else if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_UNSUPPORTED) == 0) {
        assert(payload->generation == 7 && payload->error == XIAOTAI_ERR_NOT_SUPPORTED);
        ++unsupported_events;
    } else if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_UPLINK) == 0 && payload->generation == 7) {
        if (payload->phase == XIAOTAI_VIDEO_STARTING) saw_start = true;
        if (payload->phase == XIAOTAI_VIDEO_RUNNING) started_before_running = saw_start;
    }
}

static xiaotai_err_t subscribe(void)
{
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_REMOTE, on_event, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, on_event, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UPLINK, on_event, NULL) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static void barrier(void *arg) { xiaotai_signal_give(arg); }

static xiaotai_err_t begin(const video_session_config_t *cfg, const xiaotai_video_transform_config_t *plan)
{
    assert(cfg->has_downlink && plan->out_width == 320 && plan->out_height == 240);
    ++begins;
    revision = cfg->revision;
    started_generation = cfg->generation;
    return begin_result;
}

static void stop(uint32_t rev) { assert(rev == revision); ++stops; }

static bool receive(uint32_t rev, const xiaotai_video_packet_t *packet)
{
    assert(rev == revision && packet->size == sizeof(owned));
    if (!input_room) return false;
    for (size_t i = 0; i < sizeof(owned); ++i) owned[i] = packet->data[i];
    ++received;
    return true;
}

static const video_session_receiver_t receiver = {.begin = begin, .stop = stop, .receive = receive};

static bool ready(void *ctx, uint32_t generation) { (void)ctx; (void)generation; return true; }
static xiaotai_err_t send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet, uint32_t limit)
{
    (void)ctx; (void)generation; (void)packet; (void)limit;
    return XIAOTAI_OK;
}
static xiaotai_err_t key(void *ctx, uint32_t generation)
{
    (void)ctx;
    assert(generation == started_generation);
    ++keys;
    return XIAOTAI_OK;
}
static xiaotai_err_t set_bitrate(void *ctx, uint32_t generation, uint32_t min, uint32_t start, uint32_t max)
{
    (void)ctx;
    xiaotai_video_status_t snapshot;
    xiaotai_video_status(&snapshot);
    assert(snapshot.generation == generation && started_generation == generation);
    assert(min < start && start < max && max == 256000);
    assert(snapshot.uplink.phase == XIAOTAI_VIDEO_OFF);
    bitrate_since = start;
    ++ranges;
    return range_result;
}
static bool bitrate(void *ctx, uint32_t generation, uint32_t *target)
{
    (void)ctx;
    assert(generation == started_generation);
    ++takes;
    *target = wanted;
    return wanted != 0;
}

static const xiaotai_video_link_t link = {.ready = ready, .send = send, .request_key_frame = key,
                                         .set_bitrate_range = set_bitrate, .take_bitrate_target = bitrate};

static uint32_t step(uint32_t elapsed_ms)
{
    now += (int64_t)elapsed_ms * 1000;
    fake_clock_hold(now);
    return video_session_step(now);
}

static xiaotai_video_status_t snapshot(void)
{
    xiaotai_video_status_t s;
    xiaotai_video_status(&s);
    return s;
}

static void expect_wait_log(void)
{
    xiaotai_eventlog_entry_t events[12];
    const size_t count = xiaotai_eventlog_read(0, events, 12);
    bool seen = false;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].generation == 7 && strcmp(events[i].label, "uplink.stop_wait") == 0) seen = true;
    }
    assert(seen);
}

int main(void)
{
    fake_backends_reset();
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&owner_done) == XIAOTAI_OK);
    static const xiaotai_module_t observer = {.name = "video-test", .init = subscribe};
    static const xiaotai_module_t *const modules[] = {&observer};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    static const char *const fields_up[] = {"generation", "captured", "sent"};
    static const char *const fields_down[] = {"generation", "received"};
    static const xiaotai_eventlog_topic_desc_t health_up = {
        .source = "video", .name = "uplink", .fields = fields_up, .count = 3,
    };
    static const xiaotai_eventlog_topic_desc_t health_down = {
        .source = "video", .name = "downlink", .fields = fields_down, .count = 2,
    };
    xiaotai_eventlog_topic_t *up = NULL, *down = NULL;
    assert(xiaotai_eventlog_topic_register(&health_up, &up) == XIAOTAI_OK);
    assert(xiaotai_eventlog_topic_register(&health_down, &down) == XIAOTAI_OK);
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    const xiaotai_camera_desc_t desc = {.max_width = 384, .max_height = 256, .formats = XIAOTAI_CAMERA_YUV422};
    assert(fake_camera_add(&desc, false));
    assert(fake_display_add(320, 240, XIAOTAI_PIXEL_RGB565, 0));
    fake_encoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_encoder_ctl[0].score = 10;
    fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_decoder_ctl[0].score = 10;
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_decoder_ctl[1].score = 10;
    assert(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    const xiaotai_video_profile_t uplink = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
    const xiaotai_video_downlink_t downlink = XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT;
    const xiaotai_video_downlink_t jpeg = XIAOTAI_VIDEO_DOWNLINK_JPEG(640, 480);
    xiaotai_video_viewport_t viewport;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_viewport(&viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_uplink(&uplink) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&downlink) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&jpeg) == XIAOTAI_OK);

    xiaotai_video_placement_t placement = {.fit = XIAOTAI_VIDEO_FIT_COVER};
    xiaotai_video_session_t s = {.generation = 7, .uplink = &uplink, .downlink = &downlink, .link = &link,
                                 .placement = &placement};
    assert(xiaotai_video_open(&s) == XIAOTAI_ERR_INVALID_STATE);
    video_session_install_receiver(&receiver);
    video_session_set_health_topics(up, down);
    xiaotai_engine_video_start();
    s.camera = 1;
    assert(xiaotai_video_open(&s) == XIAOTAI_ERR_NOT_FOUND);
    s.camera = 0;
    placement.rotation = (xiaotai_video_rotation_t)8;
    assert(xiaotai_video_open(&s) == XIAOTAI_ERR_INVALID_ARG);
    placement.rotation = XIAOTAI_VIDEO_ROTATE_0;
    assert(xiaotai_video_open(&s) == XIAOTAI_OK);
    assert(xiaotai_video_open(&s) == XIAOTAI_OK);
    placement.rotation = XIAOTAI_VIDEO_ROTATE_90;
    assert(xiaotai_video_open(&s) == XIAOTAI_ERR_INVALID_STATE);
    assert(step(0) == 20 && begins == 1 && ranges == 1 && keys == 1);
    assert(snapshot().generation == 7 && snapshot().uplink.bitrate_bps == 256000);
    assert(snapshot().downlink.waiting_key_frame);
    video_session_config_t work;
    assert(video_session_uplink_request(&work) && work.generation == 7);
    const uint32_t rev_a = work.revision;
    assert(work.placement.rotation == XIAOTAI_VIDEO_ROTATE_0);
    assert(video_session_uplink_take_key(rev_a) && !video_session_uplink_take_key(rev_a));
    xiaotai_video_request_key_frame(7, XIAOTAI_VIDEO_KEY_PEER_REQUEST);
    assert(video_session_uplink_take_key(rev_a));
    xiaotai_video_request_key_frame(7, XIAOTAI_VIDEO_KEY_PEER_REQUEST);
    assert(!video_session_uplink_take_key(rev_a));
    xiaotai_video_request_key_frame(7, XIAOTAI_VIDEO_KEY_NEW_VIEWER);
    assert(video_session_uplink_take_key(rev_a));
    video_session_uplink_report(rev_a, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);

    uint8_t bytes[] = {1, 2, 3, 4};
    xiaotai_video_packet_t packet = {.codec = XIAOTAI_VIDEO_CODEC_H264, .key_frame = true, .data = bytes, .size = sizeof(bytes)};
    assert(!xiaotai_video_push(6, &packet));
    assert(xiaotai_video_push(7, &packet));
    bytes[0] = 5;
    assert(owned[0] == 1 && received == 1);
    video_session_downlink_presentable(rev_a, true);
    assert(xiaotai_video_remote_generation() == 7 && snapshot().downlink.phase == XIAOTAI_VIDEO_RUNNING);
    video_session_downlink_presentable(rev_a + 1, false);
    assert(xiaotai_video_remote_generation() == 7);
    input_room = false;
    assert(!xiaotai_video_push(7, &packet));
    packet.size = 0;
    assert(!xiaotai_video_push(7, &packet));
    assert(snapshot().downlink.stale == 1 && snapshot().downlink.received == 1 &&
           snapshot().downlink.overflow == 1 && snapshot().downlink.rejected == 1);
    assert(step(999) == 20 && keys == 1 && takes == 0);
    assert(step(1) == 20 && keys == 2 && takes == 0);
    video_session_downlink_flags(rev_a, false, true);
    video_session_downlink_flags(rev_a, false, true);
    assert(xiaotai_video_remote_generation() == 0);
    assert(step(1000) == 20 && keys == 2);

    xiaotai_video_close(6);
    assert(xiaotai_video_set_camera_enabled(6, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(snapshot().ignored == 2);
    assert(xiaotai_video_set_camera_enabled(7, false) == XIAOTAI_OK);
    assert(!video_session_uplink_current(rev_a) && snapshot().generation == 7);
    assert(xiaotai_video_set_camera_enabled(7, true) == XIAOTAI_OK);
    assert(step(0) == 10 && !video_session_uplink_current(rev_a));
    video_session_uplink_report(rev_a, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
    assert(step(0) == 20 && video_session_uplink_current(rev_a) && video_session_uplink_take_key(rev_a));

    s.generation = 8;
    s.placement = NULL;
    assert(xiaotai_video_open(&s) == XIAOTAI_OK);
    assert(!video_session_uplink_current(rev_a) && snapshot().generation == 0);
    assert(step(0) == 10 && !stops);
    assert(step(3999) == 10 && !stops && begins == 1);
    xiaotai_eventlog_topic_snapshot_t health;
    assert(xiaotai_eventlog_topic_read(0, &health) && health.posts == 2);
    assert(xiaotai_eventlog_topic_read(1, &health) && health.posts == 2 && health.values[1] == 1);
    assert(step(1) == 10 && !stops && begins == 1);
    expect_wait_log();
    video_session_uplink_report(rev_a, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
    begin_result = XIAOTAI_ERR_NO_MEM;
    assert(step(0) == 20 && stops == 1 && begins == 2 && snapshot().generation == 0);
    assert(step(499) == 20 && begins == 2);
    begin_result = XIAOTAI_OK;
    range_result = XIAOTAI_OK;
    assert(step(1) == 20 && begins == 3 && ranges == 2 && snapshot().generation == 8);
    assert(snapshot().uplink.bitrate_bps == bitrate_since && bitrate_since < 256000);
    assert(step(199) == 20 && takes == 0);
    wanted = 96000;
    assert(step(1) == 20 && takes == 1 && snapshot().uplink.bitrate_bps == 96000);
    assert(video_session_uplink_request(&work) && work.revision != rev_a && work.generation == 8);
    video_session_uplink_report(work.revision, XIAOTAI_VIDEO_ERROR, XIAOTAI_FAIL);
    assert(snapshot().uplink.phase == XIAOTAI_VIDEO_ERROR);
    assert(step(499) == 20 && !video_session_uplink_current(work.revision));
    assert(step(1) == 20 && video_session_uplink_current(work.revision));

    s.generation = 9;
    s.uplink = NULL;
    s.downlink = &jpeg;
    assert(xiaotai_video_open(&s) == XIAOTAI_OK);
    video_session_uplink_report(work.revision, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
    const unsigned keys_before = keys;
    assert(step(0) == 20 && snapshot().generation == 9 && !snapshot().downlink.waiting_key_frame);
    assert(xiaotai_video_set_camera_enabled(9, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(step(3000) == 20 && keys == keys_before);
    xiaotai_video_close(9);
    assert(step(0) == 20 && stops == 3);
    assert(step(0) == XIAOTAI_WAIT_FOREVER);
    xiaotai_video_reset();
    assert(xiaotai_run(barrier, &owner_done) == XIAOTAI_OK && xiaotai_signal_take(&owner_done, 3000));
    assert(remote_on == 1 && unsupported_events == 1 && started_before_running);
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&owner_done);
    xiaotai_eventlog_reset();
    fake_clock_release();
    return 0;
}
