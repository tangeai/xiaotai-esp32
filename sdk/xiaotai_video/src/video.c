// SPDX-License-Identifier: MIT
/* Video engine core: lifecycle, requirement tables, capabilities, the shell of
 * xiaotai_video_resolve(), the status snapshot and the engine's tasks.
 *
 * Memory: the requirement tables are allocated from XIAOTAI_MEM_BULK by their
 * first declaration (in an init(), where allocating is allowed); downlink
 * descriptions use a fixed array of one entry per video codec. Declaring
 * nothing allocates nothing and creates no task. */
#include <string.h>
#include "internal.h"
#include "capture.h"
#include "convert.h"
#include "downlink.h"
#include "geometry.h"
#include "paint.h"
#include "present.h"
#include "uplink.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"

video_engine_t video_engine;

static void ensure_lock(void)
{
    if (video_engine.lock_ready) return;
    xiaotai_critical_init(&video_engine.lock);
    video_engine.lock_ready = true;
}

/* ---------------------------------------------------------------- profiles */

static bool both_or_neither(uint16_t a, uint16_t b) { return (a == 0U) == (b == 0U); }

static xiaotai_err_t normalize_h264(xiaotai_video_profile_t *p)
{
    /* Fields only JPEG uses must be 0 (the header's rule for unused fields). */
    if (p->fallback_width || p->fallback_height || p->quality || p->min_quality || p->pressure_limit ||
        p->refetch_stale) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (p->key_interval_s == 0U) return XIAOTAI_ERR_INVALID_ARG;
    if (p->width) {
        /* As the governor of the existing pipeline normalized: raise to the
         * minimum first, then drop an odd side to even. */
        if (p->width < VIDEO_H264_MIN_WIDTH) p->width = VIDEO_H264_MIN_WIDTH;
        if (p->height < VIDEO_H264_MIN_HEIGHT) p->height = VIDEO_H264_MIN_HEIGHT;
        p->width &= (uint16_t)~1U;
        p->height &= (uint16_t)~1U;
        p->max_pixels = 0U;   /* only a derived size uses it */
    }
    if (p->fps < VIDEO_H264_MIN_FPS) p->fps = VIDEO_H264_MIN_FPS;
    if (p->fps > VIDEO_H264_MAX_FPS) p->fps = VIDEO_H264_MAX_FPS;
    if (p->bitrate_bps < VIDEO_H264_MIN_BITRATE_BPS) p->bitrate_bps = VIDEO_H264_MIN_BITRATE_BPS;
    if (p->bitrate_bps > VIDEO_H264_MAX_BITRATE_BPS) p->bitrate_bps = VIDEO_H264_MAX_BITRATE_BPS;
    if (p->min_qp < VIDEO_H264_QP_LOWEST || p->min_qp > VIDEO_H264_QP_HIGHEST) p->min_qp = VIDEO_H264_DEFAULT_MIN_QP;
    if (p->max_qp < p->min_qp || p->max_qp > VIDEO_H264_QP_HIGHEST) {
        p->max_qp = VIDEO_H264_DEFAULT_MAX_QP;
        if (p->max_qp < p->min_qp) p->max_qp = p->min_qp;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t check_jpeg(xiaotai_video_profile_t *p)
{
    /* Fields only H.264 uses must be 0. */
    if (p->bitrate_bps || p->min_qp || p->max_qp || p->key_interval_s) return XIAOTAI_ERR_INVALID_ARG;
    if (p->fps < VIDEO_JPEG_MIN_FPS || p->fps > VIDEO_JPEG_MAX_FPS) return XIAOTAI_ERR_INVALID_ARG;
    if (p->quality == 0U || p->quality > VIDEO_JPEG_MAX_QUALITY) return XIAOTAI_ERR_INVALID_ARG;
    if (p->min_quality == 0U || p->min_quality > p->quality) return XIAOTAI_ERR_INVALID_ARG;
    if (p->fallback_width) {
        /* A fallback is a smaller fixed size. */
        if (!p->width || p->fallback_width > p->width || p->fallback_height > p->height ||
            (p->fallback_width == p->width && p->fallback_height == p->height)) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    if (p->width) p->max_pixels = 0U;
    return XIAOTAI_OK;
}

xiaotai_err_t video_profile_normalize(const xiaotai_video_profile_t *profile, xiaotai_video_profile_t *out)
{
    if (!profile || !out) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_video_profile_t p = *profile;
    if (!XIAOTAI_CODEC_IS_VIDEO(p.codec)) return XIAOTAI_ERR_INVALID_ARG;
    if (!both_or_neither(p.width, p.height) || !both_or_neither(p.fallback_width, p.fallback_height)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (p.max_frame_bytes == 0U) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = p.codec == XIAOTAI_VIDEO_CODEC_H264 ? normalize_h264(&p) : check_jpeg(&p);
    if (err != XIAOTAI_OK) return err;
    *out = p;
    return XIAOTAI_OK;
}

/* Field by field: the struct has padding, so memcmp would compare garbage. */
static bool profile_equal(const xiaotai_video_profile_t *a, const xiaotai_video_profile_t *b)
{
    return a->codec == b->codec && a->width == b->width && a->height == b->height && a->max_pixels == b->max_pixels &&
           a->fallback_width == b->fallback_width && a->fallback_height == b->fallback_height &&
           a->upright == b->upright && a->fps == b->fps && a->bitrate_bps == b->bitrate_bps &&
           a->min_qp == b->min_qp && a->max_qp == b->max_qp && a->key_interval_s == b->key_interval_s &&
           a->quality == b->quality && a->min_quality == b->min_quality &&
           a->max_frame_bytes == b->max_frame_bytes && a->backlog_limit_bytes == b->backlog_limit_bytes &&
           a->max_age_ms == b->max_age_ms && a->pressure_limit == b->pressure_limit &&
           a->refetch_stale == b->refetch_stale && a->retry_ms == b->retry_ms;
}

static const xiaotai_video_profile_t *find_normalized(const xiaotai_video_profile_t *normalized)
{
    const video_requirements_t *r = &video_engine.requirements;
    for (size_t i = 0; i < r->profile_count; ++i) {
        if (profile_equal(&r->profiles[i], normalized)) return &r->profiles[i];
    }
    return NULL;
}

const xiaotai_video_profile_t *video_require_find_uplink(const xiaotai_video_profile_t *profile)
{
    xiaotai_video_profile_t normalized;
    if (video_profile_normalize(profile, &normalized) != XIAOTAI_OK) return NULL;
    return find_normalized(&normalized);
}

xiaotai_err_t xiaotai_video_require_uplink(const xiaotai_video_profile_t *profile)
{
    if (video_started()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_video_profile_t normalized;
    const xiaotai_err_t err = video_profile_normalize(profile, &normalized);
    if (err != XIAOTAI_OK) return err;
    if (find_normalized(&normalized)) return XIAOTAI_OK;
    video_requirements_t *r = &video_engine.requirements;
    if (r->profile_count >= (size_t)XIAOTAI_VIDEO_PROFILES_MAX) return XIAOTAI_ERR_NO_MEM;
    if (!r->profiles) {
        r->profiles = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, (size_t)XIAOTAI_VIDEO_PROFILES_MAX, sizeof(*r->profiles));
        if (!r->profiles) return XIAOTAI_ERR_NO_MEM;
    }
    r->profiles[r->profile_count++] = normalized;
    return XIAOTAI_OK;
}

/* --------------------------------------------------------------- downlinks */

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

xiaotai_err_t xiaotai_video_require_downlink(const xiaotai_video_downlink_t *downlink)
{
    if (video_started()) return XIAOTAI_ERR_INVALID_STATE;
    if (!downlink || !XIAOTAI_CODEC_IS_VIDEO(downlink->codec)) return XIAOTAI_ERR_INVALID_ARG;
    if (!downlink->max_width || !downlink->max_height || !downlink->max_frame_bytes) return XIAOTAI_ERR_INVALID_ARG;
    /* Buffered playout paces frames at the sender's rate, so it needs one. */
    if (downlink->burst_ms && !downlink->fps) return XIAOTAI_ERR_INVALID_ARG;
    /* Only a codec with a reference chain rides out stalls by buffering;
     * every JPEG frame stands alone and only the newest is shown. */
    if (downlink->burst_ms && downlink->codec == XIAOTAI_VIDEO_CODEC_JPEG) return XIAOTAI_ERR_INVALID_ARG;

    video_requirements_t *r = &video_engine.requirements;
    xiaotai_video_downlink_t *slot = NULL;
    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS && !slot; ++i) {
        if (r->downlinks[i].codec == downlink->codec) slot = &r->downlinks[i];
    }
    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS && !slot; ++i) {
        if (r->downlinks[i].codec == XIAOTAI_VIDEO_CODEC_NONE) {
            slot = &r->downlinks[i];
            *slot = *downlink;
            return XIAOTAI_OK;
        }
    }
    if (!slot) return XIAOTAI_ERR_NO_MEM;   /* more video codecs than slots: tuning.h lags codec.h */
    /* One description per codec: the largest of each field, so every
     * declared stream fits the pools sized from it. */
    slot->max_width = (uint16_t)max_u32(slot->max_width, downlink->max_width);
    slot->max_height = (uint16_t)max_u32(slot->max_height, downlink->max_height);
    slot->fps = (uint8_t)max_u32(slot->fps, downlink->fps);
    slot->max_frame_bytes = max_u32(slot->max_frame_bytes, downlink->max_frame_bytes);
    slot->burst_ms = max_u32(slot->burst_ms, downlink->burst_ms);
    slot->key_refresh_ms = (uint16_t)max_u32(slot->key_refresh_ms, downlink->key_refresh_ms);
    slot->recover_h264_profile |= downlink->recover_h264_profile;
    slot->preserve_h264_parameter_chain |= downlink->preserve_h264_parameter_chain;
    return XIAOTAI_OK;
}

const xiaotai_video_downlink_t *video_require_downlink(xiaotai_video_codec_t codec)
{
    if (codec == XIAOTAI_VIDEO_CODEC_NONE) return NULL;
    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS; ++i) {
        if (video_engine.requirements.downlinks[i].codec == codec) return &video_engine.requirements.downlinks[i];
    }
    return NULL;
}

/* --------------------------------------------------------------- viewports */

static bool viewport_pixel(xiaotai_video_pixel_t pixel)
{
    return pixel == XIAOTAI_VIDEO_PIXEL_RGB565 || pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED ||
           pixel == XIAOTAI_VIDEO_PIXEL_RGB888;
}

xiaotai_err_t xiaotai_video_require_viewport(const xiaotai_video_viewport_t *viewport)
{
    if (video_started()) return XIAOTAI_ERR_INVALID_STATE;
    if (!viewport) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_display_t *display = xiaotai_hal_display(viewport->display);
    if (!display) return XIAOTAI_ERR_INVALID_ARG;
    if (!viewport->width || !viewport->height || viewport->width > display->desc.width ||
        viewport->height > display->desc.height) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!viewport_pixel(viewport->pixel)) return XIAOTAI_ERR_INVALID_ARG;
    if ((unsigned)viewport->fit > (unsigned)XIAOTAI_VIDEO_FIT_COVER) return XIAOTAI_ERR_INVALID_ARG;
    if ((unsigned)viewport->rotation > (unsigned)XIAOTAI_VIDEO_ROTATE_270) return XIAOTAI_ERR_INVALID_ARG;
    if (viewport->frames && viewport->frames < VIDEO_VIEWPORT_MIN_FRAMES) return XIAOTAI_ERR_INVALID_ARG;

    video_requirements_t *r = &video_engine.requirements;
    for (size_t i = 0; i < r->viewport_count; ++i) {
        video_viewport_entry_t *entry = &r->viewports[i];
        const xiaotai_video_viewport_t *v = &entry->viewport;
        if (v->display != viewport->display) continue;
        /* Same display: the same area, layout and placement, or nothing, so
         * the outcome never depends on the order modules initialize in. */
        if (v->width != viewport->width || v->height != viewport->height || v->pixel != viewport->pixel ||
            v->fit != viewport->fit || v->rotation != viewport->rotation ||
            v->match_orientation != viewport->match_orientation) {
            return XIAOTAI_ERR_INVALID_STATE;
        }
        if (viewport->frames > entry->viewport.frames) entry->viewport.frames = viewport->frames;
        if (!viewport->frames) entry->default_frames = true;
        return XIAOTAI_OK;
    }
    if (r->viewport_count >= (size_t)XIAOTAI_VIDEO_VIEWPORTS_MAX) return XIAOTAI_ERR_NO_MEM;
    if (!r->viewports) {
        r->viewports = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, (size_t)XIAOTAI_VIDEO_VIEWPORTS_MAX, sizeof(*r->viewports));
        if (!r->viewports) return XIAOTAI_ERR_NO_MEM;
    }
    video_viewport_entry_t *entry = &r->viewports[r->viewport_count++];
    entry->viewport = *viewport;
    entry->default_frames = viewport->frames == 0U;
    return XIAOTAI_OK;
}

const video_viewport_entry_t *video_require_viewport_for(uint8_t display)
{
    const video_requirements_t *r = &video_engine.requirements;
    for (size_t i = 0; i < r->viewport_count; ++i) {
        if (r->viewports[i].viewport.display == display) return &r->viewports[i];
    }
    return NULL;
}

bool video_require_any(void)
{
    const video_requirements_t *r = &video_engine.requirements;
    if (r->profile_count || r->viewport_count) return true;
    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS; ++i) {
        if (r->downlinks[i].codec != XIAOTAI_VIDEO_CODEC_NONE) return true;
    }
    return false;
}

/* ------------------------------------------------------- uplink encoders */

/* Layouts in the order they are tried; the first ones are what cameras
 * deliver and encoders read. */
static const xiaotai_video_pixel_t k_layouts[] = {
    XIAOTAI_VIDEO_PIXEL_YUYV422,        XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, XIAOTAI_VIDEO_PIXEL_I420,
    XIAOTAI_VIDEO_PIXEL_RGB565,         XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, XIAOTAI_VIDEO_PIXEL_RGB888,
};
#define LAYOUTS (sizeof(k_layouts) / sizeof(k_layouts[0]))

xiaotai_video_pixel_t video_camera_pixel(uint32_t camera_format)
{
    switch (camera_format) {
    case XIAOTAI_CAMERA_YUV422: return XIAOTAI_VIDEO_PIXEL_YUYV422;
    case XIAOTAI_CAMERA_YUV420_PAIRED: return XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    case XIAOTAI_CAMERA_RGB565: return XIAOTAI_VIDEO_PIXEL_RGB565;
    default: return XIAOTAI_VIDEO_PIXEL_NONE;
    }
}

static bool camera_delivers(const xiaotai_camera_desc_t *camera, xiaotai_video_pixel_t pixel)
{
    for (uint32_t bit = 1U; bit && bit <= camera->formats; bit <<= 1) {
        if ((camera->formats & bit) && video_camera_pixel(bit) == pixel) return true;
    }
    return false;
}

static bool plan_transform(const xiaotai_video_transform_t *backend, const xiaotai_camera_desc_t *camera,
                           xiaotai_video_pixel_t from, xiaotai_video_pixel_t to,
                           const xiaotai_video_profile_t *profile, xiaotai_video_transform_config_t *config)
{
    const xiaotai_video_image_t source = {
        .pixel = from, .width = camera->max_width, .height = camera->max_height,
        .full_range = (camera->flags & XIAOTAI_CAMERA_FULL_RANGE) != 0,
    };
    xiaotai_video_scale_limits_t limits;
    return backend->limits(from, to, &limits) &&
           video_geometry_uplink(&source, profile, (xiaotai_video_rotation_t)(camera->rotation & 3U),
                                 to, &limits, config) && backend->match(config) > 0;
}

static bool transform_reaches(const xiaotai_camera_desc_t *camera, xiaotai_video_pixel_t pixel,
                              const xiaotai_video_profile_t *profile, uint16_t width, uint16_t height)
{
    const xiaotai_video_profile_t exact = {.width = width, .height = height};
    const xiaotai_video_profile_t *shape = profile ? profile : &exact;
    for (uint32_t bit = 1U; bit && bit <= camera->formats; bit <<= 1) {
        const xiaotai_video_pixel_t from = (camera->formats & bit) ? video_camera_pixel(bit) : XIAOTAI_VIDEO_PIXEL_NONE;
        if (from == XIAOTAI_VIDEO_PIXEL_NONE) continue;
        for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
            xiaotai_video_transform_config_t config;
            if (plan_transform(video_transform_at(i), camera, from, pixel, shape, &config) &&
                config.out_width == width && config.out_height == height) return true;
        }
    }
    return false;
}

static uint16_t padded_rows(uint16_t height, uint8_t align)
{
    if (align <= 1U) return height;
    return (uint16_t)(((uint32_t)height + align - 1U) / align * align);
}

static void probe_config(const xiaotai_video_profile_t *profile, xiaotai_video_codec_t codec,
                         const xiaotai_camera_desc_t *camera, uint16_t width, uint16_t height, uint16_t rows,
                         xiaotai_video_pixel_t pixel, xiaotai_video_encoder_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->codec = codec;
    config->pixel = pixel;
    config->width = width;
    config->height = height;
    config->rows = rows;
    config->full_range = (camera->flags & XIAOTAI_CAMERA_FULL_RANGE) != 0U;
    if (profile) {
        config->fps = profile->fps;
        config->bitrate_bps = profile->bitrate_bps;
        config->min_qp = profile->min_qp;
        config->max_qp = profile->max_qp;
        if (codec == XIAOTAI_VIDEO_CODEC_H264) {
            const uint32_t frames = (uint32_t)profile->fps * profile->key_interval_s;
            config->key_interval_frames = (uint16_t)(frames > 255U ? 255U : (frames ? frames : 1U));
        }
        config->quality = profile->quality;
    } else if (codec == XIAOTAI_VIDEO_CODEC_H264) {
        config->fps = 15U;
        config->bitrate_bps = 1000000U;
        config->min_qp = VIDEO_H264_QP_LOWEST;
        config->max_qp = VIDEO_H264_QP_HIGHEST;
        config->key_interval_frames = 15U;
    } else {
        config->fps = 15U;
        config->quality = 80U;
    }
}

const xiaotai_video_encoder_t *video_uplink_encoder(const xiaotai_video_profile_t *profile, xiaotai_video_codec_t codec,
                                                    const xiaotai_camera_desc_t *camera, uint16_t width,
                                                    uint16_t height, bool direct_allowed, xiaotai_video_pixel_t *pixel)
{
    if (pixel) *pixel = XIAOTAI_VIDEO_PIXEL_NONE;
    if (!camera || !width || !height) return NULL;
    const xiaotai_video_encoder_t *best = NULL;
    int best_score = 0;
    xiaotai_video_pixel_t best_pixel = XIAOTAI_VIDEO_PIXEL_NONE;
    /* Pass 0: what the camera delivers, as it delivers it (rows padded as the
     * camera pads them). Pass 1: what a transform makes of it. */
    for (int pass = direct_allowed ? 0 : 1; pass < 2; ++pass) {
        for (size_t i = 0; i < LAYOUTS; ++i) {
            const xiaotai_video_pixel_t layout = k_layouts[i];
            const bool direct = pass == 0;
            if (direct ? !camera_delivers(camera, layout) : !transform_reaches(camera, layout, profile, width, height))
                continue;
            const uint16_t rows = direct ? padded_rows(height, camera->row_align) : height;
            if (rows < height) continue;
            xiaotai_video_encoder_config_t config;
            probe_config(profile, codec, camera, width, height, rows, layout, &config);
            const xiaotai_video_encoder_t *encoder = xiaotai_video_encoder_find(&config);
            if (!encoder) continue;
            const int score = encoder->match(&config);
            if (score > best_score) {
                best = encoder;
                best_score = score;
                best_pixel = layout;
            }
        }
    }
    if (pixel) *pixel = best_pixel;
    return best;
}

/* ------------------------------------------------------------ capabilities */

void xiaotai_video_capabilities(xiaotai_video_capabilities_t *capabilities)
{
    if (!capabilities) return;
    memset(capabilities, 0, sizeof(*capabilities));
    if (!video_engine.initialized) return;
    static const xiaotai_video_codec_t codecs[] = {XIAOTAI_VIDEO_CODEC_JPEG, XIAOTAI_VIDEO_CODEC_H264};
    for (size_t c = 0; c < sizeof(codecs) / sizeof(codecs[0]); ++c) {
        const xiaotai_video_codec_t codec = codecs[c];
        /* An encoder takes a camera's pictures when it accepts them at the
         * camera's native size, straight or through a transform. */
        for (size_t index = 0; xiaotai_hal_camera(index); ++index) {
            const xiaotai_camera_desc_t *desc = &xiaotai_hal_camera(index)->desc;
            if (video_uplink_encoder(NULL, codec, desc, desc->max_width, desc->max_height, true, NULL)) {
                capabilities->uplink_codecs |= XIAOTAI_VIDEO_CODEC_BIT(codec);
                break;
            }
        }
        /* A decoder probed at its own limit ({codec, 0, 0}). */
        const xiaotai_video_decoder_config_t probe = {.codec = codec, .max_width = 0, .max_height = 0};
        if (xiaotai_video_decoder_find(&probe)) capabilities->downlink_codecs |= XIAOTAI_VIDEO_CODEC_BIT(codec);
    }
    const xiaotai_camera_t *camera = xiaotai_hal_camera(0);
    if (camera) {
        capabilities->camera_width = camera->desc.max_width;
        capabilities->camera_height = camera->desc.max_height;
        capabilities->camera_rotation = (xiaotai_video_rotation_t)(camera->desc.rotation & 3U);
        capabilities->camera_full_range = (camera->desc.flags & XIAOTAI_CAMERA_FULL_RANGE) != 0U;
    }
}

/* ----------------------------------------------------------------- resolve */

static const xiaotai_video_encoder_t *resolve_stepped(const xiaotai_video_profile_t *profile,
                                                      const xiaotai_camera_desc_t *camera, uint16_t *w, uint16_t *h)
{
    const xiaotai_video_encoder_t *best = NULL;
    uint32_t area = 0;
    for (size_t from = 0; from < LAYOUTS; ++from) {
        if (!camera_delivers(camera, k_layouts[from])) continue;
        for (size_t to = 0; to < LAYOUTS; ++to) {
            for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
                xiaotai_video_transform_config_t plan;
                if (!plan_transform(video_transform_at(i), camera, k_layouts[from], k_layouts[to], profile, &plan))
                    continue;
                const uint32_t candidate = (uint32_t)plan.out_width * plan.out_height;
                if (candidate <= area) continue;
                const xiaotai_video_encoder_t *encoder =
                    video_uplink_encoder(profile, profile->codec, camera, plan.out_width, plan.out_height, false, NULL);
                if (!encoder) continue;
                best = encoder;
                area = candidate;
                *w = plan.out_width;
                *h = plan.out_height;
            }
        }
    }
    return best;
}

static const xiaotai_video_encoder_t *resolve_encoder(const xiaotai_video_profile_t *p,
                                                      const xiaotai_camera_desc_t *desc,
                                                      uint16_t *width, uint16_t *height)
{
    /* The picture the camera offers, turned as the profile asks. */
    const unsigned turn = p->upright ? (desc->rotation & 3U) : 0U;
    const uint16_t source_width = (turn & 1U) ? desc->max_height : desc->max_width;
    const uint16_t source_height = (turn & 1U) ? desc->max_width : desc->max_height;
    const bool derived = p->width == 0U;
    uint16_t w = p->width, h = p->height;
    const xiaotai_video_scale_limits_t unrestricted = {0};
    if (derived && !video_geometry_derive(source_width, source_height, p->max_pixels, &unrestricted, &w, &h))
        return NULL;
    if (w > source_width || h > source_height) return NULL;
    /* A camera mode of exactly this size is used as it is (a camera that
     * states no minimum may have one); any other picture, and a turned one,
     * passes through a transform first. */
    const bool camera_size = turn == 0U && (derived ? (w == source_width && h == source_height) :
        (!desc->min_width || !desc->min_height || (w >= desc->min_width && h >= desc->min_height)));
    const xiaotai_video_encoder_t *encoder = video_uplink_encoder(p, p->codec, desc, w, h, camera_size, NULL);
    /* If the exact unconstrained size has no route, a stepped backend may
     * keep the whole picture at a smaller (still exact) size. */
    if (!encoder && derived) encoder = resolve_stepped(p, desc, &w, &h);
    if (encoder) {
        if (width) *width = w;
        if (height) *height = h;
    }
    return encoder;
}

xiaotai_err_t xiaotai_video_resolve(const xiaotai_video_profile_t *profile, size_t camera, uint16_t *width,
                                    uint16_t *height)
{
    if (!width || !height) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_video_profile_t p;
    const xiaotai_err_t err = video_profile_normalize(profile, &p);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_camera_t *part = xiaotai_hal_camera(camera);
    if (!part) return XIAOTAI_ERR_NOT_FOUND;
    return resolve_encoder(&p, &part->desc, width, height) ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ tasks */

static void set_task(video_task_plan_t *plan, video_task_kind_t kind, const char *name, uint32_t stack_bytes,
                     xiaotai_mem_kind_t stack, unsigned priority, int core)
{
    /* Two downlinks decoded by backends of the same load share one task: it
     * gets the larger stack, internal when either asks for it. */
    video_task_plan_entry_t *entry = &plan->task[kind];
    if (!entry->exists || stack_bytes > entry->config.stack_bytes) entry->config.stack_bytes = stack_bytes;
    if (!entry->exists || stack == XIAOTAI_MEM_INTERNAL) entry->config.stack = stack;
    entry->exists = true;
    entry->config.name = name;
    entry->config.priority = priority;
    entry->config.core = core;
}

void video_task_plan(video_task_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    for (size_t i = 0; i < VIDEO_TASK_KINDS; ++i) plan->task[i].config.stack = XIAOTAI_MEM_BULK;
    const bool has_session = video_require_any();
    const bool has_probe = video_engine.probe_enabled && xiaotai_hal_camera(0);
    if (!has_session && !has_probe) return;
    if (has_session)
        set_task(plan, VIDEO_TASK_SESSION, "video_session", VIDEO_SESSION_STACK_BYTES, XIAOTAI_MEM_BULK,
                 VIDEO_SESSION_PRIORITY, VIDEO_SESSION_CORE);

    /* One camera task owns every call of the camera parts, whatever encoder a
     * profile uses. Its scheduling follows the load of the encoders the
     * declared profiles match: a software encoder keeps it below relay and
     * convert (and pinned away from audio processing), even next to a
     * hardware one, so no software burst runs at the hardware priority. */
    bool hardware = false;
    bool software = false;
    const video_requirements_t *r = &video_engine.requirements;
    for (size_t i = 0; i < r->profile_count; ++i) {
        const xiaotai_video_profile_t *profile = &r->profiles[i];
        for (size_t index = 0; xiaotai_hal_camera(index); ++index) {
            const xiaotai_camera_desc_t *desc = &xiaotai_hal_camera(index)->desc;
            const xiaotai_video_encoder_t *encoder = resolve_encoder(profile, desc, NULL, NULL);
            if (!encoder) continue;
            if (encoder->load == XIAOTAI_VIDEO_LOAD_HARDWARE) hardware = true;
            else software = true;
        }
    }
    plan->uplink_encodable = software || hardware;
    bool prepare_only = has_probe && !plan->uplink_encodable;
    if (r->profile_count && !plan->uplink_encodable) {
        /* An unencodable declaration still calls any boot prepare hooks on
         * their designated owner, without advertising an available uplink. */
        for (size_t i = 0; xiaotai_hal_camera(i); ++i) {
            if (xiaotai_hal_camera(i)->prepare) {
                prepare_only = true;
                break;
            }
        }
    }
    if (software || prepare_only) {
        set_task(plan, VIDEO_TASK_UPLINK, "video_uplink", VIDEO_UPLINK_STACK_BYTES, XIAOTAI_MEM_BULK,
                 VIDEO_SOFTWARE_UPLINK_PRIORITY, XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE);
    } else if (hardware) {
        set_task(plan, VIDEO_TASK_UPLINK, "video_uplink", VIDEO_UPLINK_STACK_BYTES, XIAOTAI_MEM_BULK,
                 XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY, XIAOTAI_CORE_ANY);
    }

    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS; ++i) {
        const xiaotai_video_downlink_t *downlink = &r->downlinks[i];
        if (downlink->codec == XIAOTAI_VIDEO_CODEC_NONE) continue;
        const xiaotai_video_decoder_config_t config = {
            .codec = downlink->codec, .max_width = downlink->max_width, .max_height = downlink->max_height};
        const xiaotai_video_decoder_t *decoder = xiaotai_video_decoder_find(&config);
        if (!decoder) continue;
        const xiaotai_mem_kind_t stack = decoder->stack_internal ? XIAOTAI_MEM_INTERNAL : XIAOTAI_MEM_BULK;
        if (decoder->load == XIAOTAI_VIDEO_LOAD_SOFTWARE) {
            set_task(plan, VIDEO_TASK_DECODE_SOFTWARE, "video_decode_sw",
                     decoder->stack_bytes ? decoder->stack_bytes : VIDEO_SOFTWARE_DECODE_STACK_BYTES, stack,
                     VIDEO_SOFTWARE_DECODE_PRIORITY, XIAOTAI_CORE_ANY);
            set_task(plan, VIDEO_TASK_RELAY, "video_relay", VIDEO_RELAY_STACK_BYTES, XIAOTAI_MEM_BULK,
                     VIDEO_RELAY_PRIORITY, XIAOTAI_CORE_ANY);
            set_task(plan, VIDEO_TASK_CONVERT, "video_convert", VIDEO_CONVERT_STACK_BYTES, XIAOTAI_MEM_BULK,
                     VIDEO_CONVERT_PRIORITY, VIDEO_CONVERT_CORE);
        } else {
            set_task(plan, VIDEO_TASK_DECODE_HARDWARE, "video_decode_hw",
                     decoder->stack_bytes ? decoder->stack_bytes : VIDEO_HARDWARE_DECODE_STACK_BYTES, stack,
                     XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY, XIAOTAI_CORE_ANY);
        }
    }
}

static const xiaotai_task_fn_t k_task_entry[VIDEO_TASK_KINDS] = {
    [VIDEO_TASK_SESSION] = video_session_task,      [VIDEO_TASK_UPLINK] = video_uplink_task,
    [VIDEO_TASK_DECODE_SOFTWARE] = video_downlink_decode_sw_task, [VIDEO_TASK_RELAY] = video_downlink_relay_task,
    [VIDEO_TASK_CONVERT] = video_convert_task,      [VIDEO_TASK_DECODE_HARDWARE] = video_downlink_decode_hw_task,
};

static bool create_task(video_task_kind_t kind, const video_task_plan_entry_t *entry)
{
    video_task_t *task = &video_engine.tasks[kind];
    if (xiaotai_signal_init(&task->exited) != XIAOTAI_OK) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "task.create_failed", 0, XIAOTAI_ERR_NO_MEM);
        return false;
    }
    atomic_store(&task->stop, false);
    xiaotai_task_config_t config = entry->config;
    config.entry = k_task_entry[kind];
    config.arg = task;
    const xiaotai_err_t err = xiaotai_task_create(&config, &task->handle);
    if (err != XIAOTAI_OK) {
        xiaotai_signal_deinit(&task->exited);
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "task.create_failed", 0, err);
        return false;
    }
    task->created = true;
    return true;
}

/* -------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_video_configure(const xiaotai_video_config_t *config)
{
    if (video_started() || video_engine.tasks[VIDEO_TASK_SESSION].created) return XIAOTAI_ERR_INVALID_STATE;
    video_engine.restore_initial_quality_on_resolution_reopen =
        config && config->restore_initial_quality_on_resolution_reopen;
    video_engine.buffer_independent_frames = config && config->buffer_independent_frames;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_engine_video_init(void)
{
    if (video_engine.initialized) return XIAOTAI_OK;
    ensure_lock();
    const xiaotai_err_t err = xiaotai_video_port_init();
    /* Only a full or unallocatable registry fails the start; any other
     * failure of the platform leaves the engine without those backends. */
    if (err == XIAOTAI_ERR_NO_MEM) return err;
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "port.init_failed", 0, err);
    /* The fallback is last so a platform backend wins equal scores. */
    const xiaotai_err_t fallback = video_registry_install_builtin();
    if (fallback != XIAOTAI_OK) return fallback;
    if (xiaotai_module_present("at") && xiaotai_module_present("eventlog")) {
        const xiaotai_err_t diagnostics = video_at_register();
        if (diagnostics != XIAOTAI_OK) return diagnostics;
        video_engine.probe_enabled = true;
    }
    video_registry_claim_persistent();
    video_engine.initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_engine_video_start(void)
{
    if (video_started()) return;
    for (size_t i = 0; i < VIDEO_TASK_KINDS; ++i) if (video_engine.tasks[i].created) return;
    ensure_lock();
    /* Persistent decoders registered after init (host tests) are claimed now,
     * still before the first link. */
    video_registry_claim_persistent();
    video_task_plan_t plan;
    video_task_plan(&plan);
    const bool downlink = plan.task[VIDEO_TASK_DECODE_SOFTWARE].exists || plan.task[VIDEO_TASK_DECODE_HARDWARE].exists;
    bool created[VIDEO_TASK_KINDS] = {false};
    for (size_t kind = 0; kind < VIDEO_TASK_KINDS; ++kind) {
        if (plan.task[kind].exists) created[kind] = create_task((video_task_kind_t)kind, &plan.task[kind]);
    }
    /* Reserve contiguous camera/DMA resources before the presentation pools,
     * keeping both reservations ahead of the first network session. A
     * missing direction never prevents the engine itself starting. */
    const bool session = created[VIDEO_TASK_SESSION];
    const bool camera_prepared = created[VIDEO_TASK_UPLINK] && video_uplink_prepare_boot();
    video_engine.uplink_available = session && camera_prepared && plan.uplink_encodable;
    const bool software = !plan.task[VIDEO_TASK_DECODE_SOFTWARE].exists ||
                          (created[VIDEO_TASK_DECODE_SOFTWARE] && created[VIDEO_TASK_RELAY] && created[VIDEO_TASK_CONVERT]);
    const bool hardware = !plan.task[VIDEO_TASK_DECODE_HARDWARE].exists || created[VIDEO_TASK_DECODE_HARDWARE];
    xiaotai_err_t downlink_result = XIAOTAI_OK;
    if (session && downlink && software && hardware) {
        const bool external_receiver = video_session_has_receiver();
        if (!external_receiver) downlink_result = video_present_prepare();
        if (downlink_result == XIAOTAI_OK) downlink_result = video_downlink_prepare();
        if (downlink_result == XIAOTAI_OK) {
            if (!external_receiver) video_convert_prepare();
        } else {
            (void)video_present_reset();
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "downlink.reserve_failed", 0, downlink_result);
        }
    }
    video_engine.downlink_available = session && downlink && downlink_result == XIAOTAI_OK && software && hardware;
    xiaotai_critical_enter(&video_engine.lock);
    video_engine.status.started = true;
    xiaotai_critical_exit(&video_engine.lock);
    atomic_store(&video_engine.started, true);
}

static bool stop_tasks(void)
{
    for (size_t kind = 0; kind < VIDEO_TASK_KINDS; ++kind) {
        video_task_t *task = &video_engine.tasks[kind];
        if (!task->created) continue;
        if (!atomic_exchange(&task->stop, true)) xiaotai_task_notify(&task->handle);
        if (!xiaotai_signal_take(&task->exited, VIDEO_TASK_EXIT_WAIT_MS)) {
            /* Its signal and arguments must outlive it: leak rather than free
             * what a running task still reads. */
            xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "task.exit_timeout", 0, XIAOTAI_ERR_TIMEOUT);
            return false;
        }
        xiaotai_signal_deinit(&task->exited);
        memset(task, 0, sizeof(*task));
    }
    return true;
}

void xiaotai_video_reset(void)
{
    atomic_store(&video_engine.started, false);
    if (!stop_tasks()) return;
    video_session_reset();
    if (!video_downlink_reset()) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "downlink.reset_pinned", 0, XIAOTAI_ERR_BUSY);
        return;
    }
    if (!video_present_reset()) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "present.reset_pinned", 0, XIAOTAI_ERR_BUSY);
        return;
    }
    if (!video_capture_reset()) {
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "capture.reset_pinned", 0, XIAOTAI_ERR_BUSY);
        return;
    }
    video_convert_reset();
    video_uplink_reset();
    video_paint_pin(NULL);
    video_registry_reset();
    video_requirements_t *r = &video_engine.requirements;
    xiaotai_mem_free(r->profiles);
    xiaotai_mem_free(r->viewports);
    memset(r, 0, sizeof(*r));
    video_engine.initialized = false;
    video_engine.restore_initial_quality_on_resolution_reopen = false;
    video_engine.buffer_independent_frames = false;
    video_engine.probe_enabled = false;
    video_engine.h264_unavailable = false;
    video_engine.uplink_available = false;
    video_engine.downlink_available = false;
    atomic_store(&video_engine.remote_generation, 0U);
    if (video_engine.lock_ready) {
        xiaotai_critical_enter(&video_engine.lock);
        memset(&video_engine.status, 0, sizeof(video_engine.status));
        xiaotai_critical_exit(&video_engine.lock);
    }
}

/* ------------------------------------------------------------ diagnostics */

void video_status_with_luma(xiaotai_video_status_t *status, video_luma_t *luma)
{
    if (!status) return;
    if (luma) *luma = (video_luma_t){0};
    if (!video_engine.lock_ready) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xiaotai_critical_enter(&video_engine.lock);
    *status = video_engine.status;
    if (luma && status->generation) *luma = video_engine.uplink_luma;
    xiaotai_critical_exit(&video_engine.lock);
}

void xiaotai_video_status(xiaotai_video_status_t *status) { video_status_with_luma(status, NULL); }

uint32_t xiaotai_video_remote_generation(void) { return atomic_load(&video_engine.remote_generation); }
