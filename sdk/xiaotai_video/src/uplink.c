// SPDX-License-Identifier: MIT
#include "uplink.h"
#include "uplink_h264.h"

#include "capture.h"
#include "geometry.h"
#include "internal.h"
#include "pacing.h"
#include "probe.h"
#include "xiaotai/eventlog.h"

enum { CHECK_MS = 20, FRAME_WAIT_MS = 5, JPEG_GAP_US = 1500000, JPEG_ALIGNMENT = 16 };

typedef struct {
    const xiaotai_camera_t *camera;
    bool tried_power, tried_open, borrowed, reserved_h264, private_transform_buffer;
    bool saw_frame, did_reopen, reopen_exhausted;
    xiaotai_camera_mode_t mode;
    xiaotai_camera_frame_t loan;
    uint32_t revision;
    video_luma_state_t luma_state;
    const xiaotai_video_encoder_t *encoder;
    void *encoder_state;
    xiaotai_video_encoder_config_t config;
    const xiaotai_video_transform_t *transform;
    void *transform_state;
    uint8_t *transformed;
    size_t transformed_capacity;
} camera_owner_t;

/* A camera failing to return a loan must never end up with a pointer to a
 * worker stack that the host reset subsequently releases. */
static camera_owner_t s_camera;
static xiaotai_signal_t s_boot_done;
static atomic_bool s_boot_requested, s_boot_result;
static bool s_boot_attempted;

static bool prepare_cameras(void)
{
    if (s_camera.camera || video_probe_blocks_camera()) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "reserve.camera", 0, XIAOTAI_ERR_INVALID_STATE);
        return false;
    }
    if (!video_engine.requirements.profile_count) return true;
    bool ok = true;
    for (size_t i = 0; ; ++i) {
        const xiaotai_camera_t *camera = xiaotai_hal_camera(i);
        if (!camera) break;
        if (!camera->prepare) continue;
        const xiaotai_err_t err = camera->prepare(camera->self);
        if (err != XIAOTAI_OK) {
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "reserve.camera", 0, err);
            ok = false;
        }
    }
    return ok;
}

bool video_uplink_prepare_boot(void)
{
    video_task_t *task = &video_engine.tasks[VIDEO_TASK_UPLINK];
    if (!task->created) return false;
    if (s_boot_attempted) return atomic_load(&s_boot_result);
    s_boot_attempted = true;
    const xiaotai_err_t err = xiaotai_signal_init(&s_boot_done);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "reserve.camera", 0, err);
        return false;
    }
    atomic_store(&s_boot_requested, true);
    xiaotai_task_notify(&task->handle);
    (void)xiaotai_signal_take(&s_boot_done, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&s_boot_done);
    return atomic_load(&s_boot_result);
}

void video_uplink_reset(void)
{
    video_probe_reset();
    s_boot_attempted = false;
    atomic_store(&s_boot_requested, false);
    atomic_store(&s_boot_result, false);
}

static bool wanted(const video_session_config_t *request)
{
    return !atomic_load(&video_engine.tasks[VIDEO_TASK_UPLINK].stop) &&
           video_session_uplink_current(request->revision);
}

static bool ready(const video_session_config_t *request)
{
    return wanted(request) && request->link->ready(request->link->ctx, request->generation) && wanted(request);
}

static bool congested(const video_session_config_t *request)
{
    return request->uplink.backlog_limit_bytes && request->link->backlog &&
           request->link->backlog(request->link->ctx, request->generation) >= request->uplink.backlog_limit_bytes;
}

static bool aligned_size(size_t required, size_t alignment, size_t *rounded)
{
    if (!required || (alignment & (alignment - 1U)) || required > SIZE_MAX - (alignment - 1U)) return false;
    *rounded = (required + alignment - 1U) & ~(alignment - 1U);
    return true;
}

static xiaotai_err_t close_camera(camera_owner_t *owner)
{
    if (!owner->tried_open) return XIAOTAI_OK;
    const xiaotai_err_t err = owner->camera->close(owner->camera->self);
    if (err == XIAOTAI_OK) owner->tried_open = false;
    return err;
}

static void close_encoder(camera_owner_t *owner)
{
    if (owner->encoder_state) owner->encoder->close(owner->encoder_state);
    owner->encoder_state = NULL;
}

static xiaotai_err_t release_frame(camera_owner_t *owner)
{
    if (!owner->borrowed) return XIAOTAI_OK;
    const xiaotai_err_t err = owner->camera->release(owner->camera->self, &owner->loan);
    /* CSI clears the loan even when the driver's requeue reports an error. */
    if (err == XIAOTAI_OK || !owner->loan.data) owner->borrowed = false;
    return err;
}

static xiaotai_err_t stop_camera(camera_owner_t *owner)
{
    if (!owner->camera) return XIAOTAI_OK;
    const xiaotai_err_t release = release_frame(owner);
    const xiaotai_err_t closed = close_camera(owner);
    if (owner->reserved_h264) {
        if (closed != XIAOTAI_OK || owner->borrowed) return closed != XIAOTAI_OK ? closed : release;
        const xiaotai_err_t parked = video_h264_reserve_return(xiaotai_time_us());
        if (parked != XIAOTAI_OK && parked != XIAOTAI_ERR_BUSY)
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "reserve.encoder", 0, parked);
        owner->reserved_h264 = false;
        owner->encoder_state = NULL;
    } else {
        close_encoder(owner);
        xiaotai_mem_free(owner->config.output);
    }
    owner->config.output = NULL;
    if (owner->transform_state) owner->transform->close(owner->transform_state);
    if (owner->private_transform_buffer) xiaotai_mem_free(owner->transformed);
    owner->transform_state = NULL;
    owner->transformed = NULL;
    owner->private_transform_buffer = false;
    if (closed != XIAOTAI_OK || owner->borrowed) return closed != XIAOTAI_OK ? closed : release;
    const xiaotai_err_t power = owner->tried_power ? owner->camera->set_power(owner->camera->self, false) : XIAOTAI_OK;
    if (power != XIAOTAI_OK) return power;
    *owner = (camera_owner_t){0};
    return release;
}

static xiaotai_err_t open_camera(camera_owner_t *owner)
{
    owner->luma_state = VIDEO_LUMA_NO_FRAME;
    video_session_uplink_luma(owner->revision, (video_luma_t){0});
    owner->tried_open = true; /* even a failed open needs a close before power-off */
    return owner->camera->open(owner->camera->self, &owner->mode);
}

static xiaotai_err_t open_mode(camera_owner_t *owner, uint16_t width, uint16_t height, uint32_t format, uint8_t fps)
{
    owner->mode = (xiaotai_camera_mode_t){.width = width, .height = height, .format = format, .fps = fps};
    return open_camera(owner);
}

static void sample_camera_luma(camera_owner_t *owner, bool valid)
{
    if (owner->luma_state == VIDEO_LUMA_VALID || owner->luma_state == VIDEO_LUMA_UNAVAILABLE) return;
    const video_luma_t sample = valid ? video_luma_sample(&owner->loan) : (video_luma_t){.state = VIDEO_LUMA_INVALID};
    if (sample.state == owner->luma_state) return;
    owner->luma_state = sample.state;
    video_session_uplink_luma(owner->revision, sample);
}

static void borrowed_frame(camera_owner_t *owner, const video_session_config_t *request,
                           xiaotai_video_uplink_status_t *stats)
{
    owner->borrowed = true;
    if (!owner->saw_frame && owner->did_reopen && wanted(request)) {
        ++stats->first_frame_recovered;
        xiaotai_eventlog_note(VIDEO_EVENTLOG_SOURCE, "uplink.recovered", request->generation, 0);
    }
    owner->saw_frame = true;
    ++stats->captured;
}

static bool first_frame_timeout(const camera_owner_t *owner, xiaotai_err_t err)
{
    /* The encode/release/send paths all run after a camera loan was taken. */
    return err == XIAOTAI_ERR_TIMEOUT && !owner->saw_frame && !video_engine.disable_first_frame_recovery;
}

static xiaotai_err_t recover_first_frame(camera_owner_t *owner, const video_session_config_t *request,
                                        xiaotai_video_uplink_status_t *stats, bool *cancelled)
{
    xiaotai_err_t err = XIAOTAI_ERR_TIMEOUT;
    if (!ready(request)) goto cancelled;
    if (owner->did_reopen) goto exhausted;

    /* Keep the encoder and transform, and reopen exactly the accepted mode.
     * This is not a quality or resolution fallback. */
    owner->did_reopen = true;
    ++stats->first_frame_reopens;
    xiaotai_eventlog_note(VIDEO_EVENTLOG_SOURCE, "uplink.reopen", request->generation, 0);
    video_session_uplink_stats(request->revision, stats);
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STOPPING, XIAOTAI_OK);
    err = close_camera(owner);
    if (err != XIAOTAI_OK) goto exhausted;
    err = owner->camera->set_power(owner->camera->self, false);
    if (err != XIAOTAI_OK) goto exhausted;
    owner->tried_power = false;
    if (!ready(request)) goto cancelled;

    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    owner->tried_power = true;
    err = owner->camera->set_power(owner->camera->self, true);
    if (err != XIAOTAI_OK) goto exhausted;
    if (!ready(request)) goto cancelled;
    const xiaotai_camera_mode_t mode = owner->mode;
    err = open_mode(owner, mode.width, mode.height, mode.format, mode.fps);
    if (err != XIAOTAI_OK) goto exhausted;
    if (!ready(request)) goto cancelled;
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    return XIAOTAI_OK;

exhausted:
    owner->reopen_exhausted = true;
    ++stats->first_frame_exhausted;
    xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "uplink.reopen_failed", request->generation, err);
    video_session_uplink_stats(request->revision, stats);
    return err;

cancelled:
    *cancelled = true;
    return XIAOTAI_OK;
}

static xiaotai_err_t open_jpeg_transform(camera_owner_t *owner, const xiaotai_video_transform_config_t *plan,
                                         bool refused_direct)
{
    const size_t alignment = owner->transform->output_alignment > JPEG_ALIGNMENT ?
                             owner->transform->output_alignment : JPEG_ALIGNMENT;
    const size_t bytes = video_geometry_image_bytes(plan->out_pixel, plan->out_width, plan->out_height);
    if (!aligned_size(bytes, alignment, &owner->transformed_capacity)) return XIAOTAI_ERR_INVALID_SIZE;
    owner->transformed = video_uplink_reserve_transform(owner->transformed_capacity, alignment, &owner->transformed_capacity);
    /* Descriptor-only boot planning cannot see a discrete mode rejection.
     * Do not reserve a conversion surface for every working direct mode. */
    if (!owner->transformed && refused_direct) {
        owner->transformed = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, owner->transformed_capacity);
        owner->private_transform_buffer = owner->transformed != NULL;
    }
    if (!owner->transformed) return XIAOTAI_ERR_NO_MEM;
    return owner->transform->open(plan, &owner->transform_state);
}

static xiaotai_err_t start_camera(camera_owner_t *owner, const video_session_config_t *request,
                                 const video_jpeg_pressure_t *pressure, bool *cancelled)
{
    xiaotai_video_profile_t mode = request->uplink;
    mode.width = pressure->smaller ? mode.fallback_width : request->width;
    mode.height = pressure->smaller ? mode.fallback_height : request->height;
    mode.quality = pressure->quality;
    const xiaotai_camera_t *camera = xiaotai_hal_camera(request->camera);
    if (!camera) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    owner->camera = camera;
    owner->tried_power = true;
    xiaotai_err_t err = camera->set_power(camera->self, true);
    if (err != XIAOTAI_OK) return err;
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }

    const xiaotai_camera_desc_t *desc = &camera->desc;
    const bool native_size = mode.width == desc->max_width && mode.height == desc->max_height;
    bool direct = video_jpeg_can_direct(desc, &mode), refused_direct = false;
    if (direct) {
        err = open_mode(owner, mode.width, mode.height, XIAOTAI_CAMERA_YUV422, mode.fps);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED && !native_size) {
            err = close_camera(owner);
            if (err != XIAOTAI_OK) return err;
            direct = false;
            refused_direct = true;
        } else if (err != XIAOTAI_OK) {
            return err;
        }
    }
    xiaotai_video_transform_config_t transform;
    if (!direct) {
        if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
        uint32_t format;
        owner->transform = video_jpeg_native_transform(desc, &mode, &format, &transform);
        if (!owner->transform) return XIAOTAI_ERR_NOT_SUPPORTED;
        err = open_mode(owner, desc->max_width, desc->max_height, format, mode.fps);
        if (err != XIAOTAI_OK) return err;
    }
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    if (!direct) {
        err = open_jpeg_transform(owner, &transform, refused_direct);
        if (err != XIAOTAI_OK) return err;
    }
    owner->config = (xiaotai_video_encoder_config_t){.codec = XIAOTAI_VIDEO_CODEC_JPEG,
        .pixel = XIAOTAI_VIDEO_PIXEL_YUYV422, .width = mode.width, .height = mode.height, .rows = mode.height,
        .full_range = (desc->flags & XIAOTAI_CAMERA_FULL_RANGE) != 0, .fps = mode.fps, .quality = mode.quality};
    owner->encoder = xiaotai_video_encoder_find(&owner->config);
    if (!owner->encoder) return XIAOTAI_ERR_NOT_SUPPORTED;
    const size_t alignment = owner->encoder->output_alignment > JPEG_ALIGNMENT ?
                             owner->encoder->output_alignment : JPEG_ALIGNMENT;
    if (!aligned_size(mode.max_frame_bytes, alignment, &owner->config.output_capacity)) return XIAOTAI_ERR_INVALID_SIZE;
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    owner->config.output = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, owner->config.output_capacity);
    if (!owner->config.output) return XIAOTAI_ERR_NO_MEM;
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    return owner->encoder->open(&owner->config, &owner->encoder_state);
}

static uint32_t elapsed(int64_t start, int64_t now, uint32_t min_unit)
{
    if (now < start) return UINT32_MAX;
    const uint64_t value = ((uint64_t)now - (uint64_t)start) / min_unit;
    return value <= UINT32_MAX ? (uint32_t)value : UINT32_MAX;
}

static void maximum(uint32_t *peak, uint32_t sample) { if (sample > *peak) *peak = sample; }

static bool stale(const video_session_config_t *request, int64_t at_us, int64_t now)
{
    return !ready(request) || (request->uplink.max_age_ms && elapsed(at_us, now, 1000U) > request->uplink.max_age_ms);
}

static xiaotai_err_t encode(camera_owner_t *owner, const video_session_config_t *request,
                            xiaotai_video_image_t image, xiaotai_video_packet_t *packet, bool *skipped)
{
    if (owner->transform) {
        const xiaotai_err_t result = owner->transform->run(owner->transform_state, image.data, image.size,
            owner->transformed, owner->transformed_capacity);
        if (result != XIAOTAI_OK) return result;
        image.data = owner->transformed;
        image.pixel = owner->config.pixel;
        image.width = image.stride = owner->config.width;
        image.height = image.rows = owner->config.height;
        image.size = video_geometry_image_bytes(image.pixel, image.stride, image.rows);
    }
    if (stale(request, image.captured_us, xiaotai_time_us())) {
        *skipped = true;
        return XIAOTAI_OK;
    }
    const xiaotai_err_t result = owner->encoder->encode(owner->encoder_state, &image, true, packet);
    if (result != XIAOTAI_OK) return result;
    if (!video_jpeg_complete(packet, owner->config.output, owner->config.output_capacity,
                             request->uplink.max_frame_bytes)) return XIAOTAI_ERR_INVALID_SIZE;
    *packet = (xiaotai_video_packet_t){.codec = XIAOTAI_VIDEO_CODEC_JPEG, .key_frame = true,
        .width = image.width, .height = image.height, .timestamp_ms = (uint32_t)(image.captured_us / 1000),
        .data = packet->data, .size = packet->size};
    return XIAOTAI_OK;
}

static xiaotai_err_t frame_once(camera_owner_t *owner, const video_session_config_t *request,
                               video_jpeg_pressure_t *pressure, xiaotai_video_uplink_status_t *stats,
                               bool *refetch, int64_t *last_frame)
{
    xiaotai_err_t err = owner->camera->borrow(owner->camera->self, &owner->loan);
    const int64_t now = xiaotai_time_us();
    if (err == XIAOTAI_ERR_NOT_FOUND && now - *last_frame >= JPEG_GAP_US) err = XIAOTAI_ERR_TIMEOUT;
    if (err != XIAOTAI_OK) {
        if (err != XIAOTAI_ERR_NOT_FOUND) ++stats->capture_failed;
        return err;
    }
    *last_frame = now;
    borrowed_frame(owner, request, stats);
    const xiaotai_camera_frame_t *frame = &owner->loan;
    const int64_t at = frame->captured_us ? frame->captured_us : now;
    const uint8_t align = owner->camera->desc.row_align ? owner->camera->desc.row_align : 1U;
    const uint32_t padded = ((uint32_t)frame->height + align - 1U) / align * align;
    const xiaotai_video_pixel_t pixel = video_camera_pixel(frame->format);
    const size_t bytes = video_geometry_image_bytes(pixel, frame->width, frame->rows);
    xiaotai_video_packet_t packet = {0};
    const bool valid = frame->data && bytes && frame->format == owner->mode.format && frame->width == owner->mode.width &&
        frame->height == owner->mode.height && frame->size == bytes &&
        (frame->rows == frame->height || (owner->transform && frame->rows == padded));
    sample_camera_luma(owner, valid);
    if (!valid) {
        err = XIAOTAI_ERR_INVALID_SIZE;
        ++stats->capture_failed;
    } else if (stale(request, at, now)) {
        ++stats->stale;
        *refetch = true;
    } else if (congested(request)) {
        ++stats->congested;
        ++pressure->pressure;
    } else {
        const xiaotai_video_image_t image = {.pixel = pixel, .width = frame->width,
            .height = frame->height, .rows = frame->rows, .stride = frame->width, .full_range = owner->config.full_range,
            .data = frame->data, .size = frame->size, .captured_us = at};
        const int64_t begin = xiaotai_time_us();
        err = encode(owner, request, image, &packet, refetch);
        maximum(&stats->encode_max_us, elapsed(begin, xiaotai_time_us(), 1U));
        if (*refetch) ++stats->stale;
        if (err != XIAOTAI_OK) ++stats->encode_failed;
    }
    /* Encoding kept a view of the camera bitmap, transmission only uses the
     * independent JPEG buffer. A failed release must not send anything. */
    const xiaotai_err_t released = release_frame(owner);
    if (released != XIAOTAI_OK) ++stats->capture_failed;
    if (err == XIAOTAI_OK) err = released;
    if (err != XIAOTAI_OK || !packet.size) return err;
    const int64_t begin = xiaotai_time_us();
    maximum(&stats->age_max_ms, elapsed(at, begin, 1000U));
    if (stale(request, at, begin) || !wanted(request)) {
        *refetch = true;
        ++stats->stale;
    } else if (congested(request)) {
        ++stats->congested;
        ++pressure->pressure;
    } else if (wanted(request)) {
        const xiaotai_err_t sent = request->link->send(request->link->ctx, request->generation, &packet,
                                                     request->uplink.backlog_limit_bytes);
        maximum(&stats->send_max_us, elapsed(begin, xiaotai_time_us(), 1U));
        switch (sent) {
        case XIAOTAI_OK: ++stats->sent; ++stats->key_frames; break;
        case XIAOTAI_ERR_INVALID_STATE: ++stats->stale; break;
        case XIAOTAI_ERR_BUSY: ++stats->congested; ++pressure->pressure; break;
        default: ++stats->send_failed; ++pressure->pressure; break;
        }
    } else {
        ++stats->stale;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t lower_quality(camera_owner_t *owner, const video_session_config_t *request,
                                  uint8_t quality, bool *cancelled)
{
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    owner->config.quality = quality;
    if (owner->encoder->reconfigure) {
        const xiaotai_err_t err = owner->encoder->reconfigure(owner->encoder_state, &owner->config);
        if (err != XIAOTAI_ERR_NOT_SUPPORTED) return err;
    }
    close_encoder(owner);
    if (!ready(request)) { *cancelled = true; return XIAOTAI_ERR_INVALID_STATE; }
    return owner->encoder->open(&owner->config, &owner->encoder_state);
}

static xiaotai_err_t run_camera(camera_owner_t *owner, const video_session_config_t *request,
                               video_jpeg_pressure_t *pressure, xiaotai_video_uplink_status_t *stats,
                               bool *cancelled, bool *smaller)
{
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    xiaotai_err_t err = start_camera(owner, request, pressure, cancelled);
    if (err != XIAOTAI_OK) return err;
    stats->width = owner->config.width;
    stats->height = owner->config.height;
    stats->quality = owner->config.quality;
    video_session_uplink_stats(request->revision, stats);
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    const uint32_t period = video_pacing_period_us(request->uplink.fps);
    int64_t next = xiaotai_time_us(), last_frame = next;
    while (ready(request)) {
        (void)video_probe_work(true, NULL);
        const int64_t now = xiaotai_time_us();
        const uint32_t wait_ms = video_pacing_wait_ms(next, now);
        if (wait_ms) { (void)xiaotai_task_wait_notify(wait_ms); continue; }
        bool refetch = false;
        err = frame_once(owner, request, pressure, stats, &refetch, &last_frame);
        if (err == XIAOTAI_ERR_NOT_FOUND) { (void)xiaotai_task_wait_notify(FRAME_WAIT_MS); continue; }
        if (first_frame_timeout(owner, err)) {
            err = recover_first_frame(owner, request, stats, cancelled);
            if (err != XIAOTAI_OK || *cancelled) return err;
            next = last_frame = xiaotai_time_us();
            continue;
        }
        const int64_t done = xiaotai_time_us();
        if (done - now > period) { ++stats->late; ++pressure->pressure; }
        next = refetch && request->uplink.refetch_stale ? done : video_pacing_next_us(next, done, period);
        if (err != XIAOTAI_OK) { video_session_uplink_stats(request->revision, stats); return err; }
        const video_jpeg_step_t step = video_jpeg_evaluate(pressure, &request->uplink, ready(request), done,
                                                         stats->sent, &stats->fps_x10);
        if (step == VIDEO_JPEG_LOWER_QUALITY) {
            err = lower_quality(owner, request, pressure->quality, cancelled);
            if (err != XIAOTAI_OK) {
                if (!*cancelled) ++stats->encode_failed;
                video_session_uplink_stats(request->revision, stats);
                return err;
            }
            stats->quality = pressure->quality;
        }
        video_session_uplink_stats(request->revision, stats);
        xiaotai_delay_ms(1); /* every completed frame gives lower-priority peers a chance */
        if (step == VIDEO_JPEG_SMALLER) {
            if (video_engine.restore_initial_quality_on_resolution_reopen) pressure->quality = request->uplink.quality;
            *smaller = true;
            return XIAOTAI_OK;
        }
    }
    *cancelled = true;
    return XIAOTAI_OK;
}

static xiaotai_err_t start_h264(camera_owner_t *owner, const video_session_config_t *request)
{
    const xiaotai_camera_t *camera = xiaotai_hal_camera(request->camera);
    if (!camera) return XIAOTAI_ERR_NOT_FOUND;
    video_h264_plan_t plan;
    if (!video_h264_plan(&camera->desc, &request->uplink, request->width, request->height, &plan))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    const int64_t now = xiaotai_time_us();
    const int64_t settled = now > INT64_MAX - 40000 ? INT64_MAX : now + 40000;
    uint32_t delay;
    while ((delay = video_pacing_wait_ms(settled, xiaotai_time_us()))) {
        if (!ready(request)) return XIAOTAI_ERR_INVALID_STATE;
        (void)xiaotai_task_wait_notify(delay);
    }
    if (!ready(request)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = video_h264_reserve_take(&plan, video_session_uplink_bitrate(request->revision),
        xiaotai_time_us(), &owner->encoder_state, &owner->config);
    if (err != XIAOTAI_OK) return err;
    owner->reserved_h264 = true;
    owner->camera = camera;
    owner->encoder = plan.encoder;
    owner->transform = plan.transform;
    owner->mode = plan.mode;
    if (plan.transform) {
        owner->transformed = video_uplink_reserve_transform(plan.transform_bytes, plan.transform->output_alignment,
                                                            &owner->transformed_capacity);
        if (!owner->transformed) return XIAOTAI_ERR_NO_MEM;
        err = plan.transform->open(&plan.conversion, &owner->transform_state);
        if (err != XIAOTAI_OK) return err;
    }
    if (!ready(request)) return XIAOTAI_ERR_INVALID_STATE;
    owner->tried_power = true;
    err = camera->set_power(camera->self, true);
    if (err != XIAOTAI_OK) return err;
    if (!ready(request)) return XIAOTAI_ERR_INVALID_STATE;
    return open_camera(owner);
}

static bool h264_packet_valid(const xiaotai_video_encoder_config_t *config, const xiaotai_video_packet_t *packet)
{
    if (!packet->data || !packet->size || packet->size > config->output_capacity) return false;
    const uintptr_t at = (uintptr_t)packet->data, begin = (uintptr_t)config->output;
    return at >= begin && at - begin <= config->output_capacity - packet->size;
}

static xiaotai_err_t encode_h264(camera_owner_t *owner, const video_session_config_t *request,
                              video_h264_policy_t *policy, xiaotai_video_image_t image, xiaotai_video_packet_t *packet)
{
    if (owner->transform) {
        const xiaotai_err_t err = owner->transform->run(owner->transform_state, image.data, image.size,
            owner->transformed, owner->transformed_capacity);
        if (err != XIAOTAI_OK) return err;
        image.data = owner->transformed;
        image.pixel = owner->config.pixel;
        image.width = image.stride = owner->config.width;
        image.height = owner->config.height;
        image.rows = owner->config.rows;
        image.size = video_geometry_image_bytes(image.pixel, image.stride, image.rows);
        if (XIAOTAI_VIDEO_UPLINK_CLOCK_OVERLAY) (void)video_overlay_clock(owner->transformed, image.size, &image);
    }
    if (!ready(request)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = owner->encoder->encode(owner->encoder_state, &image, policy->key_needed, packet);
    if (err != XIAOTAI_OK) return err;
    if (!h264_packet_valid(&owner->config, packet)) return XIAOTAI_ERR_INVALID_SIZE;
    packet->codec = XIAOTAI_VIDEO_CODEC_H264;
    packet->width = image.width;
    packet->height = image.height;
    packet->timestamp_ms = (uint32_t)(image.captured_us / 1000);
    return XIAOTAI_OK;
}

static xiaotai_err_t frame_h264(camera_owner_t *owner, const video_session_config_t *request,
                                video_h264_policy_t *policy, xiaotai_video_uplink_status_t *stats, int64_t *last_frame)
{
    xiaotai_err_t err = owner->camera->borrow(owner->camera->self, &owner->loan);
    const int64_t now = xiaotai_time_us();
    if (err == XIAOTAI_ERR_NOT_FOUND && elapsed(*last_frame, now, 1000U) < 3000U) return err;
    *last_frame = now;
    if (err != XIAOTAI_OK) {
        ++stats->capture_failed;
        return err == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_ERR_TIMEOUT : err;
    }
    borrowed_frame(owner, request, stats);
    const xiaotai_camera_frame_t *frame = &owner->loan;
    const xiaotai_video_pixel_t pixel = video_camera_pixel(frame->format);
    const size_t needed = video_geometry_image_bytes(pixel, frame->width, frame->rows);
    const int64_t at = frame->captured_us > 0 ? frame->captured_us : now;
    xiaotai_video_packet_t packet = {0};
    const bool valid = frame->data && frame->format == owner->mode.format && needed && frame->size >= needed &&
        frame->width == owner->mode.width && frame->height == owner->mode.height && frame->rows >= frame->height &&
        (owner->transform || (frame->rows == owner->config.rows && pixel == owner->config.pixel));
    sample_camera_luma(owner, valid);
    if (!valid) {
        ++stats->capture_failed;
        err = XIAOTAI_ERR_INVALID_SIZE;
    } else if (stale(request, at, now)) {
        ++stats->stale;
    } else if (congested(request)) {
        ++stats->congested;
    } else {
        const xiaotai_video_image_t image = {.pixel = pixel, .width = frame->width, .height = frame->height,
            .rows = frame->rows, .stride = frame->width, .full_range = owner->config.full_range,
            .data = frame->data, .size = needed, .captured_us = at};
        const int64_t begun = xiaotai_time_us();
        err = encode_h264(owner, request, policy, image, &packet);
        maximum(&stats->encode_max_us, elapsed(begun, xiaotai_time_us(), 1U));
        if (err == XIAOTAI_ERR_INVALID_STATE && !ready(request)) {
            ++stats->stale;
            packet.size = 0;
            err = XIAOTAI_OK;
        } else if (err != XIAOTAI_OK) {
            ++stats->encode_failed;
        }
    }
    const xiaotai_err_t returned = release_frame(owner);
    if (returned != XIAOTAI_OK) ++stats->capture_failed;
    if (err != XIAOTAI_OK || returned != XIAOTAI_OK) {
        policy->key_needed = true;
        return err != XIAOTAI_OK ? err : returned;
    }
    if (!packet.size) return XIAOTAI_OK;
    const int64_t begun = xiaotai_time_us();
    maximum(&stats->age_max_ms, elapsed(at, begun, 1000U));
    bool oversize;
    if (!video_h264_protect(policy, request->uplink.max_frame_bytes, begun, &packet, &oversize)) {
        if (oversize) ++stats->oversize;
        return XIAOTAI_OK;
    }
    xiaotai_err_t sent;
    if (stale(request, at, begun)) sent = XIAOTAI_ERR_INVALID_STATE;
    else if (congested(request)) sent = XIAOTAI_ERR_BUSY;
    else if (wanted(request)) {
        video_capture_offer(request->generation, request->revision, &packet, begun);
        sent = request->link->send(request->link->ctx, request->generation, &packet, request->uplink.backlog_limit_bytes);
    }
    else sent = XIAOTAI_ERR_INVALID_STATE;
    maximum(&stats->send_max_us, elapsed(begun, xiaotai_time_us(), 1U));
    video_h264_send_result(policy, &packet, sent, xiaotai_time_us());
    if (sent == XIAOTAI_OK) { ++stats->sent; if (packet.key_frame) ++stats->key_frames; }
    else if (sent == XIAOTAI_ERR_INVALID_STATE) ++stats->stale;
    else if (sent == XIAOTAI_ERR_BUSY) { ++stats->congested; ++stats->backpressure; }
    else { ++stats->send_failed; ++stats->backpressure; }
    return XIAOTAI_OK;
}

static xiaotai_err_t run_h264(camera_owner_t *owner, const video_session_config_t *request,
                              xiaotai_video_uplink_status_t *stats, bool *cancelled)
{
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    xiaotai_err_t err = start_h264(owner, request);
    if (err != XIAOTAI_OK) { *cancelled = err == XIAOTAI_ERR_INVALID_STATE && !ready(request); return err; }
    stats->width = owner->config.width;
    stats->height = owner->config.height;
    video_session_uplink_stats(request->revision, stats);
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_RUNNING, XIAOTAI_OK);
    int64_t next = xiaotai_time_us(), last_frame = next, retry_at = 0;
    int64_t fps_window = next;
    uint32_t sent_at_start = stats->sent;
    video_h264_policy_t policy;
    video_h264_policy_start(&policy, next);
    const uint32_t period = video_h264_period_us(request->uplink.fps);
    const xiaotai_video_link_t *link = request->link;
    while (ready(request)) {
        (void)video_probe_work(true, NULL);
        const int64_t now = xiaotai_time_us();
        if (elapsed(fps_window, now, 1000U) >= 5000U) {
            stats->fps_x10 = (uint32_t)((uint64_t)(stats->sent - sent_at_start) * UINT64_C(10000000) /
                                       ((uint64_t)now - (uint64_t)fps_window));
            fps_window = now;
            sent_at_start = stats->sent;
            video_session_uplink_stats(request->revision, stats);
        }
        if (now < policy.pause_until_us) { (void)xiaotai_task_wait_notify(1); continue; }
        const uint32_t wait = video_pacing_wait_ms(retry_at > next ? retry_at : next, now);
        if (wait) { (void)xiaotai_task_wait_notify(wait); continue; }
        if (link->take_key_frame_request && link->take_key_frame_request(link->ctx, request->generation))
            xiaotai_video_request_key_frame(request->generation, XIAOTAI_VIDEO_KEY_PEER_REQUEST);
        if (video_session_uplink_take_key(request->revision)) policy.key_needed = true;
        const uint32_t bitrate = video_session_uplink_bitrate(request->revision);
        if (bitrate && bitrate != owner->config.bitrate_bps) {
            bool reopened;
            err = video_h264_reserve_rate(bitrate, now, &owner->encoder_state, &owner->config, &reopened);
            if (err != XIAOTAI_OK) return err;
            if (reopened) policy.key_needed = true;
        }
        err = frame_h264(owner, request, &policy, stats, &last_frame);
        if (err == XIAOTAI_ERR_NOT_FOUND) { (void)xiaotai_task_wait_notify(FRAME_WAIT_MS); continue; }
        if (first_frame_timeout(owner, err)) {
            err = recover_first_frame(owner, request, stats, cancelled);
            if (err != XIAOTAI_OK || *cancelled) return err;
            next = last_frame = xiaotai_time_us();
            retry_at = 0;
            continue;
        }
        const int64_t done = xiaotai_time_us();
        if (elapsed(now, done, 1U) > period) ++stats->late;
        next = video_h264_next_us(next, done, period);
        video_session_uplink_stats(request->revision, stats);
        if (owner->borrowed) return err; /* the loan must leave before another borrow */
        if (err != XIAOTAI_OK) retry_at = done > INT64_MAX - 200000 ? INT64_MAX : done + 200000;
        xiaotai_delay_ms(1);
    }
    *cancelled = true;
    return XIAOTAI_OK;
}

static void run_request(const video_session_config_t *request, video_jpeg_pressure_t *pressure,
                        xiaotai_video_uplink_status_t *stats)
{
    pressure->window_us = xiaotai_time_us();
    pressure->pressure = 0;
    pressure->sent_before = stats->sent;
    while (wanted(request)) {
        (void)video_probe_work(true, NULL);
        if (!ready(request)) { (void)xiaotai_task_wait_notify(CHECK_MS); continue; }
        bool cancelled = false, smaller = false;
        s_camera.revision = request->revision;
        const xiaotai_err_t result = request->uplink.codec == XIAOTAI_VIDEO_CODEC_H264 ?
            run_h264(&s_camera, request, stats, &cancelled) :
            run_camera(&s_camera, request, pressure, stats, &cancelled, &smaller);
        const bool exhausted = s_camera.reopen_exhausted;
        if (s_camera.camera) video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STOPPING, XIAOTAI_OK);
        const xiaotai_err_t cleanup = stop_camera(&s_camera);
        if (cleanup != XIAOTAI_OK && s_camera.camera) {
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "uplink.close", request->generation, cleanup);
            video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STOPPING, cleanup);
            while (!atomic_load(&video_engine.tasks[VIDEO_TASK_UPLINK].stop)) {
                (void)video_probe_work(true, NULL);
                (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
            }
            return;
        }
        if (cleanup != XIAOTAI_OK || (result != XIAOTAI_OK && !cancelled)) {
            const xiaotai_err_t error = cleanup != XIAOTAI_OK ? cleanup : result;
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "uplink.failed", request->generation, error);
            if (exhausted) video_session_uplink_exhausted(request->revision, error);
            else video_session_uplink_report(request->revision, XIAOTAI_VIDEO_ERROR, error);
            return;
        }
        if (!smaller) video_session_uplink_report(request->revision, XIAOTAI_VIDEO_STARTING, XIAOTAI_OK);
    }
    video_session_uplink_report(request->revision, XIAOTAI_VIDEO_OFF, XIAOTAI_OK);
}

static bool probe_cancelled(void)
{
    return atomic_load(&video_engine.tasks[VIDEO_TASK_UPLINK].stop) || video_session_diagnostics_busy();
}

void video_uplink_task(void *arg)
{
    video_task_t *task = arg;
    uint32_t revision = 0;
    video_jpeg_pressure_t pressure = {0};
    xiaotai_video_uplink_status_t stats = {0};
    while (!atomic_load(&task->stop)) {
        if (atomic_exchange(&s_boot_requested, false)) {
            const bool prepared = prepare_cameras();
            if (prepared) {
                const xiaotai_err_t reserved = video_uplink_reserve_prepare();
                video_engine.h264_unavailable = reserved != XIAOTAI_OK;
                if (video_engine.h264_unavailable) {
                    xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "reserve.encoder", 0, reserved);
                }
            }
            atomic_store(&s_boot_result, prepared);
            xiaotai_signal_give(&s_boot_done);
        }
        if (video_started() && video_probe_work(video_session_diagnostics_busy() || s_camera.camera,
                                                probe_cancelled)) continue;
        video_session_config_t request;
        if (video_started() && video_session_uplink_request(&request)) {
            if (video_probe_blocks_camera()) {
                video_session_uplink_report(request.revision, XIAOTAI_VIDEO_ERROR, XIAOTAI_ERR_BUSY);
                continue;
            }
            if (revision != request.revision) {
                revision = request.revision;
                pressure = (video_jpeg_pressure_t){.quality = request.uplink.quality};
                stats = (xiaotai_video_uplink_status_t){0};
            }
            run_request(&request, &pressure, &stats);
        } else {
            (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
        }
    }
    if (s_camera.camera) {
        const xiaotai_err_t err = stop_camera(&s_camera);
        if (err != XIAOTAI_OK) xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "uplink.close", 0, err);
    }
    video_probe_shutdown();
    video_uplink_reserve_reset();
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}
