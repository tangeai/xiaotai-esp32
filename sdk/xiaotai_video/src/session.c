// SPDX-License-Identifier: MIT
/* The owner publishes intent, the session task serializes transitions, and
 * the camera task alone touches the camera. No transition waits on another
 * task in the session loop: even a slow close leaves deadlines runnable. */
#include "internal.h"

#include <string.h>
#include "capture.h"
#include "downlink.h"
#include "geometry.h"
#include "present.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/video_governor.h"

#define SESSION_TICK_MS 20U
#define STOP_TICK_MS 10U
#define STOP_WARN_MS 4000U
#define START_RETRY_MS 500U
#define BITRATE_STEP_MS 200U
#define REMOTE_KEY_MIN_MS 1000U
#define LOCAL_KEY_MIN_MS 2000U
#define HEALTH_INTERVAL_MS 5000U
#define MAX_PACKET_BYTES (256U * 1024U)
#define EVENT_SLOTS 12U

typedef struct {
    uint32_t serial;
    const char *name;
    int32_t value;
    xiaotai_video_event_t event;
} pending_event_t;

static struct {
    /* Except for the fields below the worker-only comment, protected by the
     * engine's status lock. The receiver table has static lifetime. */
    video_session_config_t desired, active;
    uint32_t serial;
    const video_session_receiver_t *receiver;
    bool camera_on, camera_work, camera_allowed;
    bool uplink_failed, local_key, remote_key;
    uint32_t starved;           /* revision whose decoder finds no memory */
    int64_t camera_retry, local_key_after;
    pending_event_t events[EVENT_SLOTS];
    uint32_t event_serial;
    unsigned event_head, event_count, lost_events;
    /* Session task only. */
    xiaotai_video_governor_t governor;
    bool feedback;
    uint32_t retry_revision, retry_count;
    int64_t bitrate_due, key_due, stop_due, retry_due, health_due;
    xiaotai_eventlog_topic_t *health_up, *health_down;
} state;

#define status (video_engine.status)

static int64_t after_ms(int64_t now_us, uint32_t ms)
{
    const int64_t delay = (int64_t)ms * 1000;
    return now_us > INT64_MAX - delay ? INT64_MAX : now_us + delay;
}

static void lock(void) { xiaotai_critical_enter(&video_engine.lock); }
static void unlock(void) { xiaotai_critical_exit(&video_engine.lock); }

static void notify(video_task_kind_t kind)
{
    video_task_t *task = &video_engine.tasks[kind];
    if (video_started() && task->created && !atomic_load(&task->stop)) xiaotai_task_notify(&task->handle);
}

static void append_event(const char *name, int32_t value, uint32_t generation,
                         xiaotai_video_phase_t phase, xiaotai_err_t error)
{
    if (state.event_count == EVENT_SLOTS) {
        /* Keep current transitions visible; log the gap rather than
         * pretending the owner kept up with an indefinitely full queue. */
        state.event_head = (state.event_head + 1U) % EVENT_SLOTS;
        --state.event_count;
        ++state.lost_events;
    }
    const unsigned slot = (state.event_head + state.event_count) % EVENT_SLOTS;
    state.events[slot] = (pending_event_t){++state.event_serial, name, value, {generation, phase, error}};
    ++state.event_count;
}

static void uplink_phase(xiaotai_video_phase_t phase, xiaotai_err_t error)
{
    if (status.uplink.phase == phase && status.uplink.error == error) return;
    status.uplink.phase = phase;
    status.uplink.error = error;
    xiaotai_eventlog_debug(VIDEO_EVENTLOG_SOURCE, "uplink.phase", state.active.generation, phase);
    append_event(XIAOTAI_EVENT_VIDEO_UPLINK, phase, state.active.generation, phase, error);
}

static bool equal_downlink(const xiaotai_video_downlink_t *a, const xiaotai_video_downlink_t *b)
{
    return a->codec == b->codec && a->max_width == b->max_width && a->max_height == b->max_height &&
           a->fps == b->fps && a->max_frame_bytes == b->max_frame_bytes && a->burst_ms == b->burst_ms &&
           a->key_refresh_ms == b->key_refresh_ms && a->recover_h264_profile == b->recover_h264_profile &&
           a->preserve_h264_parameter_chain == b->preserve_h264_parameter_chain;
}

static bool same_description(const video_session_config_t *a, const video_session_config_t *b)
{
    if (a->has_uplink != b->has_uplink || a->has_downlink != b->has_downlink || a->link != b->link) return false;
    if (a->has_uplink && (a->camera != b->camera || a->declared_uplink != b->declared_uplink)) return false;
    return !a->has_downlink || (a->display == b->display && equal_downlink(&a->downlink, &b->downlink) &&
           a->placement.fit == b->placement.fit && a->placement.rotation == b->placement.rotation &&
           a->placement.match_orientation == b->placement.match_orientation);
}

static xiaotai_err_t copy_config(const xiaotai_video_session_t *in, video_session_config_t *out)
{
    if (!in || !in->generation || (!in->uplink && !in->downlink) || !in->link ||
        !in->link->ready || !in->link->send || (in->placement &&
        ((unsigned)in->placement->fit > XIAOTAI_VIDEO_FIT_COVER ||
         (unsigned)in->placement->rotation > XIAOTAI_VIDEO_ROTATE_270))) return XIAOTAI_ERR_INVALID_ARG;
    if (!video_started()) return XIAOTAI_ERR_INVALID_STATE;
    *out = (video_session_config_t){.generation = in->generation, .link = in->link,
                                  .has_uplink = in->uplink != NULL, .has_downlink = in->downlink != NULL,
                                  .camera = in->camera, .display = in->display};
    if (in->uplink) {
        if (!xiaotai_hal_camera(in->camera)) return XIAOTAI_ERR_NOT_FOUND;
        const xiaotai_video_profile_t *profile = video_require_find_uplink(in->uplink);
        if (!profile || !video_engine.uplink_available ||
            (profile->codec == XIAOTAI_VIDEO_CODEC_H264 && video_engine.h264_unavailable) ||
            xiaotai_video_resolve(profile, in->camera, &out->width, &out->height) != XIAOTAI_OK)
            return XIAOTAI_ERR_NOT_SUPPORTED;
        out->uplink = *profile;
        out->declared_uplink = profile;
    }
    if (in->downlink) {
        const xiaotai_video_downlink_t *d = in->downlink;
        const xiaotai_video_downlink_t *max = video_require_downlink(d->codec);
        const video_viewport_entry_t *viewport = video_require_viewport_for(in->display);
        const xiaotai_video_decoder_config_t probe = {d->codec, d->max_width, d->max_height};
        if (!max || !viewport || !video_engine.downlink_available || !d->max_width || !d->max_height ||
            !d->max_frame_bytes || d->max_width > max->max_width || d->max_height > max->max_height ||
            d->fps > max->fps || d->max_frame_bytes > max->max_frame_bytes || d->burst_ms > max->burst_ms ||
            (d->burst_ms && !d->fps) || !xiaotai_video_decoder_find(&probe)) return XIAOTAI_ERR_NOT_SUPPORTED;
        out->downlink = *d;
        out->viewport = viewport->viewport;
        out->placement = in->placement ? *in->placement : (xiaotai_video_placement_t){
            .fit = out->viewport.fit, .rotation = out->viewport.rotation,
            .match_orientation = out->viewport.match_orientation,
        };
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_open(const xiaotai_video_session_t *session)
{
    video_session_config_t next;
    const xiaotai_err_t err = copy_config(session, &next);
    if (err != XIAOTAI_OK) return err;
    lock();
    if (next.generation == state.desired.generation) {
        const bool equal = same_description(&state.desired, &next);
        unlock();
        return equal ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    }
    if (++state.serial == 0U) ++state.serial;
    next.revision = state.serial;
    state.desired = next;
    state.camera_on = true;
    state.camera_allowed = false;
    status.requested = next.generation;
    status.generation = 0;
    atomic_store(&video_engine.remote_generation, 0U);
    unlock();
    notify(VIDEO_TASK_SESSION);
    notify(VIDEO_TASK_UPLINK);
    return XIAOTAI_OK;
}

void xiaotai_video_close(uint32_t generation)
{
    if (!video_engine.lock_ready) return;
    lock();
    const bool close = generation && state.desired.generation == generation;
    if (close) {
        state.desired = (video_session_config_t){0};
        state.camera_allowed = false;
        status.requested = status.generation = 0;
        atomic_store(&video_engine.remote_generation, 0U);
    } else {
        ++status.ignored;
    }
    unlock();
    if (close) {
        notify(VIDEO_TASK_UPLINK);
        notify(VIDEO_TASK_SESSION);
    }
}

xiaotai_err_t xiaotai_video_set_camera_enabled(uint32_t generation, bool enabled)
{
    if (!video_started()) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    if (!generation || state.desired.generation != generation || !state.desired.has_uplink) {
        if (!generation || state.desired.generation != generation) ++status.ignored;
        unlock();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const bool changed = state.camera_on != enabled;
    state.camera_on = enabled;
    if (!enabled) state.camera_allowed = false;
    status.uplink.camera_enabled = enabled;
    unlock();
    if (changed) {
        notify(VIDEO_TASK_SESSION);
        notify(VIDEO_TASK_UPLINK);
    }
    return XIAOTAI_OK;
}

/* ----------------------- bridge to the camera and receive tasks */

bool video_session_uplink_request(video_session_config_t *out)
{
    lock();
    const bool working = state.camera_work;
    if (working) *out = state.active;
    unlock();
    return working;
}

bool video_session_uplink_current(uint32_t revision)
{
    lock();
    const bool current = state.camera_work && state.camera_allowed && state.active.revision == revision;
    unlock();
    return current;
}

bool video_session_diagnostics_busy(void)
{
    if (!video_engine.lock_ready) return false;
    lock();
    const bool busy = state.desired.generation || state.active.generation || state.camera_work;
    unlock();
    return busy;
}

bool video_session_capture_current(uint32_t *generation, uint32_t *revision)
{
    lock();
    const bool current = video_started() && status.generation && state.camera_allowed && state.camera_work &&
                         state.active.uplink.codec == XIAOTAI_VIDEO_CODEC_H264 && status.uplink.phase == XIAOTAI_VIDEO_RUNNING;
    if (current) { *generation = status.generation; *revision = state.active.revision; }
    unlock();
    return current;
}

bool video_session_capture_key(uint32_t revision)
{
    lock();
    const bool current = status.generation && state.camera_allowed && state.active.revision == revision &&
                         status.uplink.phase == XIAOTAI_VIDEO_RUNNING;
    if (current) state.local_key = true;
    unlock();
    if (current) notify(VIDEO_TASK_UPLINK);
    return current;
}

static void report_uplink(uint32_t revision, xiaotai_video_phase_t phase, xiaotai_err_t error, bool exhausted)
{
    const int64_t now = xiaotai_time_us();
    lock();
    if (state.camera_work && state.active.revision == revision) {
        const bool terminal = phase == XIAOTAI_VIDEO_OFF || phase == XIAOTAI_VIDEO_ERROR;
        if (terminal) {
            state.camera_work = state.camera_allowed = false;
            if (phase == XIAOTAI_VIDEO_ERROR) {
                state.uplink_failed = exhausted || !state.active.uplink.retry_ms;
                state.camera_retry = after_ms(now, state.active.uplink.retry_ms);
            }
        }
        if (terminal || phase == XIAOTAI_VIDEO_STOPPING || state.camera_allowed) uplink_phase(phase, error);
    }
    unlock();
    notify(VIDEO_TASK_SESSION);
}

void video_session_uplink_report(uint32_t revision, xiaotai_video_phase_t phase, xiaotai_err_t error)
{
    report_uplink(revision, phase, error, false);
}

void video_session_uplink_exhausted(uint32_t revision, xiaotai_err_t error)
{
    report_uplink(revision, XIAOTAI_VIDEO_ERROR, error, true);
}

void video_session_uplink_stats(uint32_t revision, const xiaotai_video_uplink_status_t *snapshot)
{
    if (!snapshot) return;
    lock();
    if (state.camera_work && state.active.revision == revision) {
        const xiaotai_video_uplink_status_t previous = status.uplink;
        status.uplink = *snapshot;
        status.uplink.phase = previous.phase;
        status.uplink.error = previous.error;
        status.uplink.codec = previous.codec;
        status.uplink.camera_enabled = previous.camera_enabled;
        status.uplink.bitrate_bps = previous.bitrate_bps;
        status.uplink.key_requests = previous.key_requests;
    }
    unlock();
}

void video_session_uplink_luma(uint32_t revision, video_luma_t luma)
{
    lock();
    if (status.generation && state.camera_work && state.active.revision == revision &&
        state.desired.revision == revision) video_engine.uplink_luma = luma;
    unlock();
}

void xiaotai_video_request_key_frame(uint32_t generation, xiaotai_video_key_reason_t reason)
{
    if (!video_started() || (unsigned)reason > XIAOTAI_VIDEO_KEY_NEW_VIEWER) return;
    const int64_t now = xiaotai_time_us();
    lock();
    if (generation && status.generation == generation) {
        if (state.camera_allowed && state.active.uplink.codec == XIAOTAI_VIDEO_CODEC_H264 &&
            (reason == XIAOTAI_VIDEO_KEY_NEW_VIEWER || now >= state.local_key_after)) {
            state.local_key = true;
            state.local_key_after = after_ms(now, LOCAL_KEY_MIN_MS);
            ++status.uplink.key_requests;
        }
    } else {
        ++status.ignored;
    }
    unlock();
}

bool video_session_uplink_take_key(uint32_t revision)
{
    lock();
    const bool pending = state.camera_allowed && state.active.revision == revision && state.local_key;
    if (pending) state.local_key = false;
    unlock();
    return pending;
}

uint32_t video_session_uplink_bitrate(uint32_t revision)
{
    lock();
    const uint32_t bitrate = state.active.revision == revision ? status.uplink.bitrate_bps : 0;
    unlock();
    return bitrate;
}

void video_session_install_receiver(const video_session_receiver_t *receiver)
{
    /* During engine start, before the session task is published. */
    lock();
    state.receiver = receiver;
    unlock();
}

bool video_session_has_receiver(void)
{
    lock();
    const bool installed = state.receiver != NULL;
    unlock();
    return installed;
}

void video_session_downlink_update(uint32_t revision, const video_downlink_update_t *update)
{
    lock();
    if (status.generation && revision == state.active.revision) {
        xiaotai_video_downlink_status_t *down = &status.downlink;
        down->discarded += update->discarded;
        down->rejected += update->rejected;
        down->decoded += update->decoded;
        down->decode_failed += update->failed;
        down->discontinuities += update->gaps;
        down->queue_depth = update->depth;
        if (update->width && update->height) {
            down->source_width = update->width;
            down->source_height = update->height;
        }
    }
    unlock();
}

bool video_session_downlink_buffering(uint32_t revision)
{
    return video_present_buffering(revision, xiaotai_time_us());
}

bool video_session_downlink_current(uint32_t revision)
{
    lock();
    const bool current = status.generation && state.active.revision == revision &&
        !status.downlink.quarantined && !status.downlink.unsupported;
    unlock();
    return current;
}

bool video_session_downlink_config(uint32_t revision, video_session_config_t *out)
{
    lock();
    const bool current = status.generation && state.active.revision == revision &&
                         state.active.has_downlink && !status.downlink.quarantined && !status.downlink.unsupported;
    if (current) *out = state.active;
    unlock();
    return current;
}

void video_session_downlink_converted(uint32_t revision, bool success)
{
    lock();
    if (status.generation && state.active.revision == revision) {
        if (success) ++status.downlink.converted;
        else ++status.downlink.convert_failed;
    }
    unlock();
}

void video_session_downlink_presented(uint32_t revision)
{
    lock();
    if (status.generation && state.active.revision == revision) ++status.downlink.presented;
    unlock();
}

void video_session_downlink_starved(uint32_t revision)
{
    lock();
    if (status.generation && revision == state.active.revision && !state.active.has_uplink) state.starved = revision;
    unlock();
    notify(VIDEO_TASK_SESSION);
}

void video_session_downlink_need_key(uint32_t revision)
{
    lock();
    if (status.generation && revision == state.active.revision && !status.downlink.unsupported)
        state.remote_key = true;
    unlock();
}

void video_session_downlink_quarantine(uint32_t revision, bool quarantined)
{
    lock();
    if (status.generation && revision == state.active.revision) {
        status.downlink.quarantined = quarantined;
        status.downlink.phase = quarantined ? XIAOTAI_VIDEO_ERROR : XIAOTAI_VIDEO_STARTING;
        status.downlink.error = quarantined ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    }
    unlock();
}

void video_session_downlink_flags(uint32_t revision, bool waiting_key, bool unsupported)
{
    lock();
    if (status.generation && state.active.revision == revision && state.active.has_downlink) {
        if (unsupported && !status.downlink.unsupported) {
            status.downlink.unsupported = true;
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "downlink.unsupported", state.active.generation,
                                   XIAOTAI_ERR_NOT_SUPPORTED);
            if (atomic_exchange(&video_engine.remote_generation, 0U) != 0U)
                append_event(XIAOTAI_EVENT_VIDEO_REMOTE, 0, state.active.generation, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
            append_event(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, 0, state.active.generation, XIAOTAI_VIDEO_OFF,
                         XIAOTAI_ERR_NOT_SUPPORTED);
        } else if (!unsupported && status.downlink.unsupported && state.active.downlink.recover_h264_profile) {
            status.downlink.unsupported = false;
            state.remote_key = true;
            append_event(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, 0, state.active.generation, XIAOTAI_VIDEO_STARTING,
                         XIAOTAI_OK);
        }
        status.downlink.waiting_key_frame = waiting_key && !status.downlink.unsupported &&
                                             state.active.downlink.codec == XIAOTAI_VIDEO_CODEC_H264;
    }
    unlock();
}

void video_session_downlink_presentable(uint32_t revision, bool presentable)
{
    lock();
    if (status.generation && state.active.revision == revision && state.active.has_downlink) {
        const uint32_t generation = presentable && !status.downlink.unsupported ? state.active.generation : 0;
        if (atomic_exchange(&video_engine.remote_generation, generation) != generation) {
            append_event(XIAOTAI_EVENT_VIDEO_REMOTE, generation != 0U, state.active.generation,
                         XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
        }
        if (generation) status.downlink.phase = XIAOTAI_VIDEO_RUNNING;
    }
    unlock();
}

bool xiaotai_video_push(uint32_t generation, const xiaotai_video_packet_t *packet)
{
    if (!video_started()) return false;
    lock();
    if (!video_started()) { unlock(); return false; }
    if (!generation || generation != status.generation) {
        ++status.downlink.stale;
        unlock();
        return false;
    }
    const xiaotai_video_downlink_t *down = &state.active.downlink;
    if (!state.active.has_downlink || status.downlink.quarantined ||
        (status.downlink.unsupported && !down->recover_h264_profile) || !packet ||
        packet->codec != down->codec || !packet->data || !packet->size || packet->size > MAX_PACKET_BYTES ||
        packet->size > down->max_frame_bytes) {
        ++status.downlink.rejected;
        if (down->codec == XIAOTAI_VIDEO_CODEC_H264 && !status.downlink.unsupported) state.remote_key = true;
        unlock();
        return false;
    }
    const video_session_receiver_t *receiver = state.receiver;
    const uint32_t revision = state.active.revision;
    const bool h264 = down->codec == XIAOTAI_VIDEO_CODEC_H264;
    unlock();
    const bool taken = receiver && receiver->receive && receiver->receive(revision, packet);
    lock();
    if (revision == state.active.revision && generation == status.generation) {
        if (taken) {
            ++status.downlink.received;
            status.downlink.received_bytes += packet->size;
        } else {
            ++status.downlink.overflow;
            if (h264) state.remote_key = true;
        }
    }
    unlock();
    return taken;
}

/* -------------------------------------- session-task transitions */

static void publish_bitrate(uint32_t revision)
{
    lock();
    if (status.generation && state.active.revision == revision) status.uplink.bitrate_bps = state.governor.bitrate_bps;
    unlock();
}

static void register_bitrate(const video_session_config_t *config, int64_t now)
{
    if (!config->has_uplink || config->uplink.codec != XIAOTAI_VIDEO_CODEC_H264) return;
    const xiaotai_video_link_t *link = config->link;
    xiaotai_video_governor_reset(&state.governor, config->width, config->height, config->uplink.bitrate_bps);
    if (link->set_bitrate_range && link->take_bitrate_target) {
        const xiaotai_video_bitrate_range_t r = state.governor.range;
        const xiaotai_err_t err = link->set_bitrate_range(link->ctx, config->generation, r.min_bps, r.start_bps, r.max_bps);
        if (err == XIAOTAI_OK) {
            state.feedback = true;
            /* Registration aligns the first frame exactly to the negotiated
             * start, even when the delta is below the governor deadband. */
            state.governor.bitrate_bps = r.start_bps;
        } else {
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "bitrate.refused", config->generation, err);
        }
    }
    state.bitrate_due = after_ms(now, BITRATE_STEP_MS);
    publish_bitrate(config->revision);
}

static void maintain_link(const video_session_config_t *config, int64_t now)
{
    const xiaotai_video_link_t *link = config->link;
    if (state.feedback && now >= state.bitrate_due) {
        uint32_t target;
        const bool changed = link->take_bitrate_target(link->ctx, config->generation, &target) ?
            xiaotai_video_governor_target(&state.governor, target, now) : xiaotai_video_governor_step(&state.governor, now);
        if (changed) publish_bitrate(config->revision);
        state.bitrate_due = after_ms(now, BITRATE_STEP_MS);
    }
    if (config->downlink.codec != XIAOTAI_VIDEO_CODEC_H264 || !link->request_key_frame || now < state.key_due) return;
    lock();
    const bool needed = status.generation == config->generation && !status.downlink.unsupported &&
                        (state.remote_key || status.downlink.waiting_key_frame || config->downlink.key_refresh_ms);
    if (needed) state.remote_key = false;
    unlock();
    if (!needed) return;
    const xiaotai_err_t err = link->request_key_frame(link->ctx, config->generation);
    uint32_t period = REMOTE_KEY_MIN_MS;
    lock();
    if (state.active.revision == config->revision) {
        if (err == XIAOTAI_OK) {
            ++status.downlink.key_requests;
            if (!status.downlink.waiting_key_frame && config->downlink.key_refresh_ms > period)
                period = config->downlink.key_refresh_ms;
        } else {
            state.remote_key = true;
        }
    }
    unlock();
    state.key_due = after_ms(now, period);
}

static void stop_active(const video_session_config_t *config, const video_session_receiver_t *receiver)
{
    video_capture_session_ended(config->revision);
    if (config->has_downlink && receiver && receiver->stop) receiver->stop(config->revision);
    lock();
    atomic_store(&video_engine.remote_generation, 0U);
    if (config->has_downlink) append_event(XIAOTAI_EVENT_VIDEO_REMOTE, 0, config->generation, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
    state.active = (video_session_config_t){0};
    status.downlink.phase = XIAOTAI_VIDEO_OFF;
    status.downlink.waiting_key_frame = false;
    state.remote_key = state.local_key = state.uplink_failed = false;
    state.camera_retry = state.local_key_after = 0;
    unlock();
    xiaotai_eventlog_note(VIDEO_EVENTLOG_SOURCE, "session.stop", config->generation, 0);
    state.feedback = false;
    state.governor = (xiaotai_video_governor_t){0};
    state.stop_due = state.key_due = state.retry_due = 0;
}

static xiaotai_err_t begin_receive(const video_session_config_t *config, const video_session_receiver_t *receiver)
{
    if (!config->has_downlink) return XIAOTAI_OK;
    const xiaotai_video_image_t input = {.width = config->downlink.max_width, .height = config->downlink.max_height};
    const xiaotai_video_scale_limits_t scale = {0};
    xiaotai_video_transform_config_t plan;
    if (!video_geometry_downlink(&input, &config->viewport, &config->placement, &scale, &plan))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    return receiver && receiver->begin ? receiver->begin(config, &plan) : XIAOTAI_OK;
}

static void start_camera(int64_t now)
{
    lock();
    const bool start = state.camera_on && !state.camera_work && !state.uplink_failed && status.generation &&
                       state.active.has_uplink && state.active.revision == state.desired.revision && now >= state.camera_retry;
    if (start) {
        state.camera_work = state.camera_allowed = true;
        state.local_key = state.active.uplink.codec == XIAOTAI_VIDEO_CODEC_H264;
        state.local_key_after = 0;
        uplink_phase(XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    }
    unlock();
    if (start) notify(VIDEO_TASK_UPLINK);
}

static void post_events(void)
{
    for (unsigned i = 0; i < EVENT_SLOTS; ++i) {
        lock();
        if (!state.event_count) {
            unlock();
            return;
        }
        const unsigned at = state.event_head;
        const pending_event_t event = state.events[at];
        unlock();
        const xiaotai_args_t args = {.value = event.value};
        const xiaotai_err_t err = xiaotai_event_emit(event.name, &args, &event.event, sizeof(event.event));
        if (err == XIAOTAI_ERR_NO_MEM) return;
        lock();
        if (state.events[state.event_head].serial == event.serial) {
            state.event_head = (at + 1U) % EVENT_SLOTS;
            --state.event_count;
        }
        const unsigned lost = state.lost_events;
        state.lost_events = 0;
        unlock();
        if (lost) xiaotai_eventlog_warn(VIDEO_EVENTLOG_SOURCE, "event.overflow", event.event.generation, (int32_t)lost);
    }
}

static void post_health(int64_t now)
{
    if ((!state.health_up && !state.health_down) || now < state.health_due) return;
    xiaotai_video_status_t snapshot;
    xiaotai_video_status(&snapshot);
    if (snapshot.requested || snapshot.generation) {
        const xiaotai_video_uplink_status_t *u = &snapshot.uplink;
        const xiaotai_video_downlink_status_t *d = &snapshot.downlink;
        const uint32_t up[] = {snapshot.generation, u->captured, u->sent, u->fps_x10, u->bitrate_bps, u->key_frames,
            u->stale, u->congested, u->late, u->oversize, u->backpressure, u->capture_failed, u->encode_failed,
            u->send_failed, u->encode_max_us, u->send_max_us, u->age_max_ms};
        const uint32_t down[] = {snapshot.generation, d->received, d->stale, d->rejected, d->overflow, d->discarded,
            d->decoded, d->decode_failed, d->converted, d->convert_failed, d->presented, d->skipped,
            d->discontinuities, d->key_requests, d->queue_depth, d->playout_depth, d->buffering};
        xiaotai_eventlog_post(state.health_up, up);
        xiaotai_eventlog_post(state.health_down, down);
    }
    state.health_due = after_ms(now, HEALTH_INTERVAL_MS);
}

uint32_t video_session_step(int64_t now)
{
    video_capture_tick(now);
    video_downlink_monitor(now);
    post_events();
    post_health(now);
    lock();
    video_session_config_t desired = state.desired;
    const video_session_config_t active = state.active;
    const bool working = state.camera_work;
    const bool camera_allowed = state.camera_allowed;
    const bool starved = active.generation && state.starved == active.revision;
    state.starved = 0;
    const video_session_receiver_t *receiver = state.receiver;
    const bool pending = state.event_count != 0U;
    unlock();

    if (active.revision != desired.revision && active.generation) {
        if (working) {
            if (!state.stop_due) state.stop_due = after_ms(now, STOP_WARN_MS);
            else if (now >= state.stop_due) {
                xiaotai_eventlog_warn(VIDEO_EVENTLOG_SOURCE, "uplink.stop_wait", active.generation, STOP_WARN_MS);
                state.stop_due = after_ms(now, STOP_WARN_MS);
            }
            return STOP_TICK_MS;
        }
        stop_active(&active, receiver);
        lock();
        desired = state.desired; /* owner may have replaced it again during release */
        unlock();
    } else if (starved) {
        /* Nothing is held until it is tried again. */
        stop_active(&active, receiver);
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "decoder.no_memory", active.generation, XIAOTAI_ERR_NO_MEM);
        lock();
        status.generation = 0;
        status.downlink.phase = XIAOTAI_VIDEO_ERROR;
        status.downlink.error = XIAOTAI_ERR_NO_MEM;
        unlock();
        state.retry_revision = active.revision;
        state.retry_due = after_ms(now, VIDEO_DECODER_STARVED_MS);
    } else if (active.generation) {
        if (active.has_downlink) {
            const bool buffering = video_present_buffering(active.revision, now);
            lock();
            if (active.revision == state.active.revision) status.downlink.buffering = buffering;
            unlock();
        }
        maintain_link(&active, now);
        if (!camera_allowed && working) return STOP_TICK_MS;
        start_camera(now);
        return SESSION_TICK_MS;
    }

    if (!desired.generation) {
        const bool settled = video_downlink_release_idle();
        return active.generation || pending || !settled ? SESSION_TICK_MS : XIAOTAI_WAIT_FOREVER;
    }
    if (desired.revision == state.retry_revision && now < state.retry_due) {
        if (video_engine.downlink_on_demand) (void)video_downlink_release_idle();
        return SESSION_TICK_MS;
    }
    const xiaotai_err_t err = begin_receive(&desired, receiver);
    if (err != XIAOTAI_OK) {
        /* Tried again while it lasts: said once, and slower after a while. */
        if (state.retry_revision != desired.revision) {
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "session.start_failed", desired.generation, err);
            state.retry_count = 0;
        }
        if (state.retry_count < UINT32_MAX) ++state.retry_count;
        state.retry_revision = desired.revision;
        state.retry_due = after_ms(now, video_engine.downlink_on_demand && state.retry_count > VIDEO_ON_DEMAND_QUICK_TRIES ?
                                        VIDEO_ON_DEMAND_SLOW_MS : START_RETRY_MS);
        return SESSION_TICK_MS;
    }
    lock();
    const bool current = desired.revision == state.desired.revision;
    if (current) {
        state.active = desired;
        video_engine.uplink_luma = (video_luma_t){0};
        status.uplink = (xiaotai_video_uplink_status_t){.codec = desired.uplink.codec,
                                                      .camera_enabled = desired.has_uplink && state.camera_on};
        status.downlink = (xiaotai_video_downlink_status_t){
            .codec = desired.downlink.codec,
            .phase = desired.has_downlink ? XIAOTAI_VIDEO_STARTING : XIAOTAI_VIDEO_OFF,
            .waiting_key_frame = desired.downlink.codec == XIAOTAI_VIDEO_CODEC_H264,
        };
        if (desired.has_downlink)
            append_event(XIAOTAI_EVENT_VIDEO_REMOTE, 0, desired.generation, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
        status.generation = desired.generation;
    }
    unlock();
    if (!current) {
        if (desired.has_downlink && receiver && receiver->stop) receiver->stop(desired.revision);
        return SESSION_TICK_MS;
    }
    state.retry_due = 0;
    state.retry_count = 0;
    xiaotai_eventlog_note(VIDEO_EVENTLOG_SOURCE, "session.start", desired.generation, 0);
    register_bitrate(&desired, now);
    start_camera(now);
    maintain_link(&desired, now);
    return SESSION_TICK_MS;
}

void video_session_set_health_topics(xiaotai_eventlog_topic_t *up, xiaotai_eventlog_topic_t *down)
{
    state.health_up = up;
    state.health_down = down;
}

void video_session_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) {
        const uint32_t wait = video_started() ? video_session_step(xiaotai_time_us()) : XIAOTAI_WAIT_FOREVER;
        if (!atomic_load(&task->stop)) (void)xiaotai_task_wait_notify(wait);
    }
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

void video_session_reset(void)
{
    lock();
    const video_session_receiver_t *receiver = state.receiver;
    const uint32_t revision = state.active.has_downlink ? state.active.revision : 0;
    /* A push may already hold this receiver/revision pair; do not let it
     * alias the first session after a reset. */
    const uint32_t serial = state.serial;
    memset(&state, 0, sizeof(state));
    state.serial = serial;
    unlock();
    if (revision && receiver && receiver->stop) receiver->stop(revision);
}
