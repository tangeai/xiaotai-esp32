// SPDX-License-Identifier: MIT
#include "internal.h"

#include <inttypes.h>
#include <stdio.h>
#include "capture.h"
#include "probe.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"

static const char *phase(xiaotai_video_phase_t value)
{
    switch (value) {
    case XIAOTAI_VIDEO_OFF: return "off";
    case XIAOTAI_VIDEO_STARTING: return "starting";
    case XIAOTAI_VIDEO_RUNNING: return "running";
    case XIAOTAI_VIDEO_STOPPING: return "stopping";
    case XIAOTAI_VIDEO_ERROR: return "error";
    default: return "unknown";
    }
}

static const char *codec(xiaotai_video_codec_t value)
{
    if (value == XIAOTAI_VIDEO_CODEC_H264) return "h264";
    if (value == XIAOTAI_VIDEO_CODEC_JPEG) return "jpeg";
    return "none";
}

static void status_report(const xiaotai_reply_t *reply)
{
    xiaotai_video_status_t status;
    video_luma_t luma;
    xiaotai_video_capabilities_t capabilities;
    video_status_with_luma(&status, &luma);
    xiaotai_video_capabilities(&capabilities);
    const xiaotai_video_uplink_status_t *u = &status.uplink;
    const xiaotai_video_downlink_status_t *d = &status.downlink;
    char error[12];
    xiaotai_at_printf(reply,
        "+VIDEO: started=%u requested=%" PRIu32 " generation=%" PRIu32 " ignored=%" PRIu32
        " uplink_codecs=%" PRIu32 " downlink_codecs=%" PRIu32
        " camera_width=%u camera_height=%u camera_rotation=%u camera_full_range=%u"
        " up_y_state=%s up_y_mean=%d up_y_n=%u",
        (unsigned)status.started, status.requested, status.generation, status.ignored,
        capabilities.uplink_codecs, capabilities.downlink_codecs, (unsigned)capabilities.camera_width,
        (unsigned)capabilities.camera_height, 90U * (unsigned)capabilities.camera_rotation, (unsigned)capabilities.camera_full_range,
        video_luma_name(luma.state), video_luma_value(luma), (unsigned)luma.samples);
    xiaotai_at_printf(reply,
        "+VIDEO-UP: phase=%s error=%s codec=%s camera_enabled=%u width=%u height=%u quality=%u bitrate_bps=%" PRIu32
        " fps_x10=%" PRIu32 " captured=%" PRIu32 " sent=%" PRIu32 " key_frames=%" PRIu32 " key_requests=%" PRIu32
        " stale=%" PRIu32 " congested=%" PRIu32 " late=%" PRIu32 " oversize=%" PRIu32
        " capture_failed=%" PRIu32 " encode_failed=%" PRIu32 " send_failed=%" PRIu32 " backpressure=%" PRIu32
        " encode_max_us=%" PRIu32 " send_max_us=%" PRIu32 " age_max_ms=%" PRIu32,
        phase(u->phase), xiaotai_at_error_name(u->error, error, sizeof(error)), codec(u->codec), (unsigned)u->camera_enabled,
        (unsigned)u->width, (unsigned)u->height, (unsigned)u->quality, u->bitrate_bps, u->fps_x10, u->captured, u->sent,
        u->key_frames, u->key_requests, u->stale, u->congested, u->late, u->oversize, u->capture_failed, u->encode_failed,
        u->send_failed, u->backpressure, u->encode_max_us, u->send_max_us, u->age_max_ms);
    xiaotai_at_printf(reply,
        "+VIDEO-DOWN: phase=%s error=%s codec=%s waiting_key_frame=%u unsupported=%u quarantined=%u buffering=%u"
        " source_width=%u source_height=%u received_bytes=%" PRIu64 " received=%" PRIu32
        " stale=%" PRIu32 " rejected=%" PRIu32 " overflow=%" PRIu32 " discarded=%" PRIu32
        " decoded=%" PRIu32 " decode_failed=%" PRIu32 " converted=%" PRIu32 " convert_failed=%" PRIu32
        " presented=%" PRIu32 " skipped=%" PRIu32 " discontinuities=%" PRIu32
        " key_requests=%" PRIu32 " queue_depth=%" PRIu32 " playout_depth=%" PRIu32,
        phase(d->phase), xiaotai_at_error_name(d->error, error, sizeof(error)), codec(d->codec),
        (unsigned)d->waiting_key_frame, (unsigned)d->unsupported, (unsigned)d->quarantined, (unsigned)d->buffering,
        (unsigned)d->source_width, (unsigned)d->source_height, d->received_bytes, d->received, d->stale, d->rejected,
        d->overflow, d->discarded, d->decoded, d->decode_failed, d->converted, d->convert_failed, d->presented,
        d->skipped, d->discontinuities, d->key_requests, d->queue_depth, d->playout_depth);
}

static xiaotai_err_t status_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    status_report(reply);
    return XIAOTAI_OK;
}

static const char *capture_phase(video_capture_phase_t state)
{
    switch (state) {
    case VIDEO_CAPTURE_ARMED: return "armed";
    case VIDEO_CAPTURE_RECORDING: return "recording";
    case VIDEO_CAPTURE_STOPPED: return "stopped";
    default: return "empty";
    }
}

static void capture_report(const xiaotai_reply_t *reply)
{
    video_capture_status_t status;
    video_capture_status(&status, xiaotai_time_us());
    xiaotai_at_printf(reply, "+VIDEO-CAPTURE-STATUS: phase=%s generation=%" PRIu32 " bytes=%zu frames=%" PRIu32
        " width=%u height=%u reason=%s", capture_phase(status.phase), status.generation, status.bytes,
        status.frames, (unsigned)status.width, (unsigned)status.height, status.reason);
}

static xiaotai_err_t capture_dump(const xiaotai_reply_t *reply)
{
    video_capture_dump_t dump;
    const bool busy = video_session_diagnostics_busy() || xiaotai_activity_current(NULL) != 0;
    const xiaotai_err_t err = video_capture_dump_take(busy, &dump);
    if (err != XIAOTAI_OK) return xiaotai_at_fail(reply, err, busy ? "SESSION" : "CAPTURE_STATE");
    char header[112];
    (void)snprintf(header, sizeof(header), "format=h264 generation=%" PRIu32 " width=%u height=%u frames=%" PRIu32,
                   dump.status.generation, (unsigned)dump.status.width, (unsigned)dump.status.height, dump.status.frames);
    const xiaotai_at_blob_t blob = {.name = "VIDEO-CAPTURE", .header = header, .data = dump.data,
        .size = dump.status.bytes, .release = video_capture_dump_release, .ctx = dump.ctx};
    const xiaotai_err_t handed = xiaotai_at_blob(reply, &blob);
    if (handed != XIAOTAI_OK) video_capture_dump_release(dump.ctx);
    return handed;
}

static xiaotai_err_t capture_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    enum { START, STOP, STATUS, CLEAR, DUMP };
    static const char *const words[] = {"START", "STOP", "STATUS", "CLEAR", "DUMP", NULL};
    const int command = xiaotai_at_keyword(argv[0], words);
    if (command < 0) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "CAPTURE_VERB");
    if (command != STATUS && !xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    xiaotai_video_capabilities_t caps;
    xiaotai_video_capabilities(&caps);
    if (!(caps.uplink_codecs & XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264)))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_NOT_SUPPORTED, "H264_UPLINK");
    switch (command) {
    case START: {
        uint32_t generation, revision;
        if (!video_session_capture_current(&generation, &revision))
            return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "H264_UPLINK");
        xiaotai_err_t err = video_capture_init();
        if (err == XIAOTAI_OK) err = video_capture_start(generation, revision, xiaotai_time_us());
        if (err != XIAOTAI_OK) return xiaotai_at_fail(reply, err, "CAPTURE_STATE");
        if (!video_session_capture_key(revision)) {
            (void)video_capture_clear();
            return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "H264_UPLINK");
        }
        break;
    }
    case STOP: video_capture_stop(); break;
    case CLEAR: {
        const xiaotai_err_t err = video_capture_clear();
        if (err != XIAOTAI_OK) return xiaotai_at_fail(reply, err, "CAPTURE_STATE");
        break;
    }
    case DUMP: return capture_dump(reply);
    default: break;
    }
    capture_report(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t probe_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    const xiaotai_err_t err = video_probe_request();
    return err == XIAOTAI_ERR_BUSY ? xiaotai_at_fail(reply, err, "PROBE_PENDING") : err;
}

xiaotai_err_t video_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = status_verb}, {.word = "CAPTURE", .min_args = 1, .max_args = 1, .run = capture_verb,
         .usage = "START|STOP|STATUS|CLEAR|DUMP"}, {.word = "PROBE", .run = probe_verb},
    };
    static const char *const fields_up[] = {"generation", "captured", "sent", "fps_x10", "bitrate_bps", "key_frames",
        "stale", "congested", "late", "oversize", "backpressure", "capture_failed", "encode_failed", "send_failed",
        "encode_max_us", "send_max_us", "age_max_ms"};
    static const char *const fields_down[] = {"generation", "received", "stale", "rejected", "overflow", "discarded",
        "decoded", "decode_failed", "converted", "convert_failed", "presented", "skipped", "discontinuities",
        "key_requests", "queue_depth", "playout_depth", "buffering"};
    static const xiaotai_eventlog_topic_desc_t up = {.source = VIDEO_EVENTLOG_SOURCE, .name = "uplink",
        .level = XIAOTAI_EVENTLOG_INFO, .fields = fields_up, .count = sizeof(fields_up) / sizeof(*fields_up)};
    static const xiaotai_eventlog_topic_desc_t down = {.source = VIDEO_EVENTLOG_SOURCE, .name = "downlink",
        .level = XIAOTAI_EVENTLOG_INFO, .fields = fields_down, .count = sizeof(fields_down) / sizeof(*fields_down)};
    xiaotai_eventlog_topic_t *up_topic = NULL, *down_topic = NULL;
    xiaotai_err_t err = xiaotai_diag_register("video", status_report);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+VIDEO", verbs, sizeof(verbs) / sizeof(*verbs));
    if (err == XIAOTAI_OK) err = xiaotai_eventlog_topic_register(&up, &up_topic);
    if (err == XIAOTAI_OK) err = xiaotai_eventlog_topic_register(&down, &down_topic);
    if (err == XIAOTAI_OK) video_session_set_health_topics(up_topic, down_topic);
    return err;
}
