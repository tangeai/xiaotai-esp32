// SPDX-License-Identifier: MIT
#include "convert.h"

#include <string.h>
#include "downlink.h"
#include "geometry.h"
#include "present.h"
#include "xiaotai/eventlog.h"

typedef struct {
    const xiaotai_video_transform_t *backend;
    xiaotai_video_transform_config_t plan;
    void *handle;
} transform_t;

typedef struct {
    transform_t transforms[XIAOTAI_VIDEO_BACKENDS_MAX];
    uint32_t revision, accelerator_failures;
} converter_t;

static converter_t s_converter[2];

static bool equal_rect(xiaotai_video_rect_t a, xiaotai_video_rect_t b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool equal_plan(const xiaotai_video_transform_config_t *a, const xiaotai_video_transform_config_t *b)
{
    return a->in_pixel == b->in_pixel && a->in_width == b->in_width && a->in_height == b->in_height &&
           a->in_stride == b->in_stride && a->in_full_range == b->in_full_range && equal_rect(a->crop, b->crop) &&
           a->rotation == b->rotation && a->out_pixel == b->out_pixel && a->out_width == b->out_width &&
           a->out_height == b->out_height && equal_rect(a->place, b->place) && a->fill == b->fill;
}

static xiaotai_err_t retain(transform_t *active, const xiaotai_video_transform_t *backend,
                            const xiaotai_video_transform_config_t *plan)
{
    if (active->handle && active->backend == backend && equal_plan(&active->plan, plan)) return XIAOTAI_OK;
    if (active->handle && active->backend == backend && backend->reconfigure) {
        const xiaotai_err_t err = backend->reconfigure(active->handle, plan);
        if (err == XIAOTAI_OK) active->plan = *plan;
        return err;
    }
    if (active->handle) active->backend->close(active->handle);
    *active = (transform_t){.backend = backend, .plan = *plan};
    const xiaotai_err_t err = backend->open(plan, &active->handle);
    if (err != XIAOTAI_OK && active->handle) {
        backend->close(active->handle);
        active->handle = NULL;
    }
    return err == XIAOTAI_OK && !active->handle ? XIAOTAI_ERR_INVALID_STATE : err;
}

static uint8_t render_view(const video_session_config_t *config, xiaotai_video_viewport_t *viewport)
{
    *viewport = config->viewport;
    const uint8_t scale = video_present_render_scale(viewport, &config->downlink);
    if (scale) { viewport->width /= scale; viewport->height /= scale; }
    return scale;
}

static int plan_backend(const xiaotai_video_transform_t *backend, const xiaotai_video_image_t *image,
                        const xiaotai_video_viewport_t *target, const xiaotai_video_placement_t *placement,
                        xiaotai_video_transform_config_t *plan)
{
    xiaotai_video_scale_limits_t limits;
    if (!backend->limits(image->pixel, target->pixel, &limits) ||
        !video_geometry_downlink(image, target, placement, &limits, plan)) return 0;
    return backend->match(plan);
}

/* Reserve one representative, declared transform for each decoder task.
 * JPEG may supply a different shape or orientation at runtime: retain that
 * actual configuration afterwards rather than reopening for each picture. */
void video_convert_prepare(void)
{
    const video_requirements_t *req = &video_engine.requirements;
    bool prepared[2] = {false, false};
    for (size_t d = 0; d < VIDEO_DOWNLINK_CODECS; ++d) {
        const xiaotai_video_downlink_t *down = &req->downlinks[d];
        if (!down->codec) continue;
        const xiaotai_video_decoder_t *decoder = xiaotai_video_decoder_find(&(const xiaotai_video_decoder_config_t){
            down->codec, down->max_width, down->max_height});
        if (!decoder || !req->viewport_count) continue;
        const unsigned load = decoder->load == XIAOTAI_VIDEO_LOAD_HARDWARE;
        if (prepared[load]) continue;
        const video_session_config_t sample = {.downlink = *down, .viewport = req->viewports[0].viewport};
        xiaotai_video_viewport_t target;
        if (!render_view(&sample, &target)) continue;
        const xiaotai_video_image_t image = {.pixel = decoder->output, .width = down->max_width,
            .height = down->max_height, .stride = down->max_width,
            .full_range = video_geometry_rgb_bytes(decoder->output) != 0};
        for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
            xiaotai_video_transform_config_t plan;
            const xiaotai_video_transform_t *backend = video_transform_at(i);
            if (plan_backend(backend, &image, &target, NULL, &plan) <= 0) continue;
            const xiaotai_err_t err = retain(&s_converter[load].transforms[i], backend, &plan);
            if (err != XIAOTAI_OK) xiaotai_eventlog_warn(VIDEO_EVENTLOG_SOURCE, "convert.reserve", 0, err);
            prepared[load] = true;
        }
    }
}

void video_convert_reset(void)
{
    for (size_t load = 0; load < 2U; ++load)
        for (size_t i = 0; i < XIAOTAI_VIDEO_BACKENDS_MAX; ++i) {
            transform_t *entry = &s_converter[load].transforms[i];
            if (entry->handle) entry->backend->close(entry->handle);
        }
    memset(s_converter, 0, sizeof(s_converter));
}

static void convert(converter_t *converter, const video_downlink_frame_t *frame)
{
    video_session_config_t config;
    if (!video_session_downlink_config(frame->revision, &config)) return;
    xiaotai_video_viewport_t target;
    const uint8_t scale = render_view(&config, &target);
    const size_t bytes = video_geometry_image_bytes(target.pixel, target.width, target.height);
    if (!scale || !bytes) { video_session_downlink_converted(frame->revision, false); return; }
    video_present_write_t out;
    if (!video_present_claim(frame->revision, bytes, &out)) return;

    if (converter->revision != frame->revision) {
        converter->revision = frame->revision;
        converter->accelerator_failures = 0;
    }
    const size_t count = video_registry_count(VIDEO_BACKEND_TRANSFORM);
    int scores[XIAOTAI_VIDEO_BACKENDS_MAX];
    xiaotai_video_transform_config_t plans[XIAOTAI_VIDEO_BACKENDS_MAX];
    for (size_t i = 0; i < count; ++i)
        scores[i] = plan_backend(video_transform_at(i), &frame->image, &target, &config.placement, &plans[i]);

    xiaotai_video_transform_config_t used;
    bool succeeded = false;
    for (;;) {
        size_t best = SIZE_MAX;
        int score = 0;
        for (size_t i = 0; i < count; ++i) {
            if (scores[i] > score) { score = scores[i]; best = i; }
        }
        if (best == SIZE_MAX) break;
        scores[best] = 0;
        const xiaotai_video_transform_t *backend = video_transform_at(best);
        if (backend->output_alignment > 1U && (uintptr_t)out.pixels % backend->output_alignment) continue;
        transform_t *entry = &converter->transforms[best];
        xiaotai_err_t err = retain(entry, backend, &plans[best]);
        if (err == XIAOTAI_OK)
            err = backend->run(entry->handle, frame->image.data, frame->image.size, out.pixels, out.capacity);
        if (err == XIAOTAI_OK) { used = plans[best]; succeeded = true; break; }
        if (backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE && converter->accelerator_failures++ % 100U == 0U)
            xiaotai_eventlog_warn(VIDEO_EVENTLOG_SOURCE, "convert.fallback", frame->generation, err);
    }
    if (succeeded) {
        const xiaotai_video_frame_t ready = {.generation = frame->generation, .pixel = target.pixel,
            .width = target.width, .height = target.height, .scale = scale, .picture = used.place,
            .pixels = out.pixels, .size = bytes};
        video_downlink_finish_conversion(frame, &out, &ready);
    } else {
        video_present_cancel(&out);
        video_session_downlink_converted(frame->revision, false);
    }
}

static bool convert_one(bool hardware)
{
    video_downlink_frame_t frame;
    if (!video_downlink_take_for_conversion(hardware, &frame)) return false;
    convert(&s_converter[hardware], &frame);
    video_downlink_release_decoded(&frame);
    return true;
}

void video_convert_kick(bool hardware)
{
    if (hardware) { (void)convert_one(true); return; }
    video_task_t *task = &video_engine.tasks[VIDEO_TASK_CONVERT];
    if (task->created && !atomic_load(&task->stop)) xiaotai_task_notify(&task->handle);
}

void video_convert_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) {
        if (!video_started() || !convert_one(false)) (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    }
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}
