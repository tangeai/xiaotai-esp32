// SPDX-License-Identifier: MIT
#include "uplink_h264.h"

#include <string.h>
#include "geometry.h"
#include "uplink.h"

enum { WORKSPACE_MIN = 1024 * 1024, CACHE_ALIGNMENT = 64, OPEN_COOLDOWN_US = 2000000 };

/* Accessed only by the uplink task, including boot preparation and teardown. */
static struct {
    video_h264_plan_t idle;
    const xiaotai_video_encoder_t *encoder;
    xiaotai_video_encoder_config_t config;
    void *handle;
    uint8_t *output, *transform;
    size_t output_capacity, transform_capacity;
    int64_t retry_at;
    uint32_t encoder_epoch;
    bool active;
} reserve;

static xiaotai_video_encoder_config_t encode_config(const xiaotai_camera_desc_t *camera,
                                                    const xiaotai_video_profile_t *profile,
                                                    uint16_t width, uint16_t height, uint16_t rows,
                                                    xiaotai_video_pixel_t pixel)
{
    const unsigned interval = (unsigned)profile->fps * profile->key_interval_s;
    return (xiaotai_video_encoder_config_t){
        .codec = XIAOTAI_VIDEO_CODEC_H264, .pixel = pixel, .width = width, .height = height, .rows = rows,
        .full_range = (camera->flags & XIAOTAI_CAMERA_FULL_RANGE) != 0, .fps = profile->fps,
        .bitrate_bps = profile->bitrate_bps, .min_qp = profile->min_qp, .max_qp = profile->max_qp,
        .key_interval_frames = (uint16_t)(interval > 255U ? 255U : interval),
    };
}

static uint32_t format_of(const xiaotai_camera_desc_t *camera, xiaotai_video_pixel_t pixel)
{
    for (uint32_t bit = 1U; bit && bit <= camera->formats; bit <<= 1U)
        if ((camera->formats & bit) && video_camera_pixel(bit) == pixel) return bit;
    return 0;
}

bool video_h264_plan(const xiaotai_camera_desc_t *camera, const xiaotai_video_profile_t *profile,
                     uint16_t width, uint16_t height, video_h264_plan_t *plan)
{
    if (!camera || !profile || !plan || !width || !height || profile->codec != XIAOTAI_VIDEO_CODEC_H264) return false;
    const unsigned mounting = camera->rotation & 3U;
    const bool direct = width == camera->max_width && height == camera->max_height && (!profile->upright || !mounting);
    xiaotai_video_pixel_t pixel;
    const xiaotai_video_encoder_t *encoder = video_uplink_encoder(profile, profile->codec, camera, width, height, direct, &pixel);
    if (!encoder) return false;
    video_h264_plan_t result = {
        .mode = {.width = camera->max_width, .height = camera->max_height, .fps = profile->fps},
    };
    if (direct && (result.mode.format = format_of(camera, pixel))) {
        const uint32_t alignment = camera->row_align ? camera->row_align : 1U;
        const uint32_t rows = ((uint32_t)height + alignment - 1U) / alignment * alignment;
        if (rows > UINT16_MAX) return false;
        result.config = encode_config(camera, profile, width, height, (uint16_t)rows, pixel);
        result.encoder = xiaotai_video_encoder_find(&result.config);
        if (result.encoder == encoder) { *plan = result; return true; }
    }
    /* The encoder picked by resolution/format also has to accept the exact
     * transform output. Scores decide the transform, not a board name. */
    result.config = encode_config(camera, profile, width, height, height, pixel);
    result.encoder = xiaotai_video_encoder_find(&result.config);
    if (!result.encoder) return false;
    const xiaotai_video_profile_t exact = {.width = width, .height = height, .upright = profile->upright};
    int best = 0;
    for (uint32_t bit = 1U; bit && bit <= camera->formats; bit <<= 1U) {
        const xiaotai_video_pixel_t from = (camera->formats & bit) ? video_camera_pixel(bit) : XIAOTAI_VIDEO_PIXEL_NONE;
        if (from == XIAOTAI_VIDEO_PIXEL_NONE) continue;
        const xiaotai_video_image_t source = {.pixel = from, .width = camera->max_width, .height = camera->max_height,
                                              .full_range = result.config.full_range};
        for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
            const xiaotai_video_transform_t *candidate = video_transform_at(i);
            xiaotai_video_scale_limits_t limits;
            xiaotai_video_transform_config_t conversion;
            if (!candidate->limits(from, pixel, &limits) ||
                !video_geometry_uplink(&source, &exact, (xiaotai_video_rotation_t)mounting, pixel, &limits, &conversion)) continue;
            const int score = candidate->match(&conversion);
            if (score <= best) continue;
            best = score;
            result.mode.format = bit;
            result.transform = candidate;
            result.conversion = conversion;
        }
    }
    if (!result.transform) return false;
    result.transform_bytes = video_geometry_image_bytes(pixel, width, height);
    if (!result.transform_bytes) return false;
    *plan = result;
    return true;
}

static size_t aligned_size(size_t bytes, size_t alignment)
{
    if (alignment & (alignment - 1U) || bytes > SIZE_MAX - (alignment - 1U)) return 0;
    return (bytes + alignment - 1U) & ~(alignment - 1U);
}

static bool same_resources(const xiaotai_video_encoder_config_t *a, const xiaotai_video_encoder_config_t *b)
{
    return a->codec == b->codec && a->pixel == b->pixel && a->full_range == b->full_range &&
           a->width == b->width && a->height == b->height && a->rows == b->rows &&
           a->min_qp == b->min_qp && a->max_qp == b->max_qp && a->output == b->output &&
           a->output_capacity == b->output_capacity;
}

static void close_encoder(void)
{
    if (reserve.handle) reserve.encoder->close(reserve.handle);
    reserve.handle = NULL;
}

static void cooldown(int64_t now)
{
    reserve.retry_at = now > INT64_MAX - OPEN_COOLDOWN_US ? INT64_MAX : now + OPEN_COOLDOWN_US;
}

static xiaotai_err_t arrange(const xiaotai_video_encoder_t *encoder, xiaotai_video_encoder_config_t next, int64_t now)
{
    if (!reserve.output || !encoder) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (now < reserve.retry_at) return XIAOTAI_ERR_BUSY;
    next.output = reserve.output;
    next.output_capacity = reserve.output_capacity;
    if (reserve.handle && reserve.encoder == encoder && same_resources(&reserve.config, &next)) {
        if (reserve.config.fps == next.fps && reserve.config.bitrate_bps == next.bitrate_bps &&
            reserve.config.key_interval_frames == next.key_interval_frames) return XIAOTAI_OK;
        if (encoder->reconfigure) {
            const xiaotai_err_t err = encoder->reconfigure(reserve.handle, &next);
            if (err == XIAOTAI_OK) { reserve.config = next; return XIAOTAI_OK; }
            if (err != XIAOTAI_ERR_NOT_SUPPORTED) {
                close_encoder();
                cooldown(now);
                return err;
            }
        }
    }
    close_encoder();
    reserve.encoder = encoder;
    const xiaotai_err_t err = encoder->open(&next, &reserve.handle);
    if (err != XIAOTAI_OK || !reserve.handle) {
        close_encoder();
        cooldown(now);
        return err == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_STATE : err;
    }
    reserve.config = next;
    ++reserve.encoder_epoch;
    return XIAOTAI_OK;
}

xiaotai_err_t video_uplink_reserve_prepare(void)
{
    const video_requirements_t *req = &video_engine.requirements;
    size_t dma_max = 0, out = WORKSPACE_MIN, scaled = 0, output_align = CACHE_ALIGNMENT, scale_align = CACHE_ALIGNMENT;
    for (size_t i = 0; i < req->profile_count; ++i) {
        const xiaotai_video_profile_t *profile = &req->profiles[i];
        for (size_t j = 0; xiaotai_hal_camera(j); ++j) {
            const xiaotai_camera_desc_t *camera = &xiaotai_hal_camera(j)->desc;
            uint16_t width, height;
            if (xiaotai_video_resolve(profile, j, &width, &height) != XIAOTAI_OK) continue;
            if (profile->codec == XIAOTAI_VIDEO_CODEC_JPEG) {
                xiaotai_video_profile_t shape = *profile;
                const uint16_t widths[] = {width, profile->fallback_width}, heights[] = {height, profile->fallback_height};
                for (size_t k = 0; k < 2U; ++k) {
                    shape.width = widths[k];
                    shape.height = heights[k];
                    if (!shape.width || !shape.height || video_jpeg_can_direct(camera, &shape)) continue;
                    uint32_t format;
                    xiaotai_video_transform_config_t plan;
                    const xiaotai_video_transform_t *backend = video_jpeg_native_transform(camera, &shape, &format, &plan);
                    if (!backend) continue;
                    const size_t bytes = video_geometry_image_bytes(plan.out_pixel, plan.out_width, plan.out_height);
                    if (bytes > scaled) scaled = bytes;
                    if (backend->output_alignment > scale_align) scale_align = backend->output_alignment;
                }
                continue;
            }
            video_h264_plan_t plan;
            if (!video_h264_plan(camera, profile, width, height, &plan)) continue;
            const size_t bytes = plan.encoder->internal_bytes ? plan.encoder->internal_bytes(&plan.config) : 0;
            if (!reserve.idle.encoder || bytes > dma_max ||
                (bytes == dma_max && plan.config.width > reserve.idle.config.width)) {
                dma_max = bytes;
                reserve.idle = plan;
            }
            if (profile->max_frame_bytes > out) out = profile->max_frame_bytes;
            if (plan.encoder->output_alignment > output_align) output_align = plan.encoder->output_alignment;
            if (plan.transform && plan.transform->output_alignment > scale_align) scale_align = plan.transform->output_alignment;
            if (plan.transform_bytes > scaled) scaled = plan.transform_bytes;
        }
    }
    reserve.transform_capacity = aligned_size(scaled, scale_align);
    if (scaled && !reserve.transform_capacity) return XIAOTAI_ERR_INVALID_SIZE;
    if (scaled) {
        reserve.transform = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, scale_align, reserve.transform_capacity);
        if (!reserve.transform) return XIAOTAI_ERR_NO_MEM;
    }
    if (!reserve.idle.encoder) return XIAOTAI_OK;
    reserve.output_capacity = aligned_size(out, output_align);
    if (!reserve.output_capacity) return XIAOTAI_ERR_INVALID_SIZE;
    reserve.output = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, output_align, reserve.output_capacity);
    if (!reserve.output) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = arrange(reserve.idle.encoder, reserve.idle.config, xiaotai_time_us());
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(reserve.output);
        reserve.output = NULL;
        reserve.output_capacity = 0;
    }
    return err;
}

xiaotai_err_t video_h264_reserve_take(const video_h264_plan_t *plan, uint32_t bitrate, int64_t now,
                                     void **encoder, xiaotai_video_encoder_config_t *config)
{
    if (!plan || !encoder || !config) return XIAOTAI_ERR_INVALID_ARG;
    *encoder = NULL;
    if (reserve.active) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_video_encoder_config_t next = plan->config;
    if (bitrate) next.bitrate_bps = bitrate;
    const xiaotai_err_t err = arrange(plan->encoder, next, now);
    if (err != XIAOTAI_OK) return err;
    reserve.active = true;
    *encoder = reserve.handle;
    *config = reserve.config;
    return XIAOTAI_OK;
}

xiaotai_err_t video_h264_reserve_rate(uint32_t bitrate, int64_t now, void **encoder,
                                     xiaotai_video_encoder_config_t *config, bool *reopened)
{
    if (!reserve.active || !reserve.handle || !encoder || !config || !reopened) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_video_encoder_config_t next = reserve.config;
    if (bitrate) next.bitrate_bps = bitrate;
    const uint32_t epoch = reserve.encoder_epoch;
    const xiaotai_err_t err = arrange(reserve.encoder, next, now);
    *encoder = reserve.handle;
    *reopened = reserve.encoder_epoch != epoch;
    if (err == XIAOTAI_OK) *config = reserve.config;
    return err;
}

uint8_t *video_uplink_reserve_transform(size_t required, size_t alignment, size_t *capacity)
{
    if (!reserve.transform || !alignment || (uintptr_t)reserve.transform % alignment ||
        !capacity || required > reserve.transform_capacity) return NULL;
    *capacity = reserve.transform_capacity;
    return reserve.transform;
}

xiaotai_err_t video_h264_reserve_return(int64_t now)
{
    if (!reserve.active) return XIAOTAI_OK;
    reserve.active = false;
    return arrange(reserve.idle.encoder, reserve.idle.config, now);
}

void video_uplink_reserve_reset(void)
{
    close_encoder();
    xiaotai_mem_free(reserve.output);
    xiaotai_mem_free(reserve.transform);
    memset(&reserve, 0, sizeof(reserve));
}
