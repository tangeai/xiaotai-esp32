// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_camera_geometry.py
/* Requirements and lifecycle (xiaotai/video.h): uplink profiles normalized as
 * the existing pipeline's governor did and kept once each; downlinks merged
 * per codec; the viewport rules for a display declared twice; the three
 * capacities; tables allocated from BULK by their first declaration;
 * declarations refused after start; nothing declared means no task, no
 * buffer and no table; the engine tasks the declarations call for; the
 * capabilities and the resolve shell; the status snapshot. */
#include <string.h>
#include "fakes.h"
#include "internal.h"
#include "uplink_h264.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/video_profiles.h"
#include "xiaotai_mem_host.h"

static void setup(void)
{
    xiaotai_video_reset();
    fake_backends_reset();
    fake_hal_reset();
    xiaotai_mem_host_clear();
    fake_port_clear();
}

static unsigned allocations(void)
{
    return fake_mem_allocations(XIAOTAI_MEM_BULK) + fake_mem_allocations(XIAOTAI_MEM_INTERNAL) +
           fake_mem_allocations(XIAOTAI_MEM_DMA);
}

static bool logged(uint32_t after, const char *label)
{
    xiaotai_eventlog_entry_t entries[16];
    const size_t n = xiaotai_eventlog_read(after, entries, 16);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(entries[i].source, "video") == 0 && strcmp(entries[i].label, label) == 0) return true;
    }
    return false;
}

static const xiaotai_video_profile_t k_compact = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
static const xiaotai_video_profile_t k_sensor = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
static const xiaotai_video_profile_t k_stream = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
static const xiaotai_video_profile_t k_still = XIAOTAI_VIDEO_PROFILE_JPEG_STILL;
static const xiaotai_video_downlink_t k_h264_down = XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT;

static size_t profile_count(void) { return video_engine.requirements.profile_count; }
static const xiaotai_video_profile_t *profile_at(size_t i) { return &video_engine.requirements.profiles[i]; }

/* ------------------------------------------------------------- nothing */

static void test_nothing_declared(void)
{
    setup();
    xiaotai_video_status_t status;
    xiaotai_video_status(&status);
    CHECK(!status.started, "started before start");
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    xiaotai_engine_video_start();
    /* Declaring nothing costs nothing: no task, no buffer, no table. */
    CHECK(fake_task_count() == 0, "%u tasks created", fake_task_count());
    CHECK(allocations() == 0, "%u allocations", allocations());
    CHECK(video_engine.requirements.profiles == NULL && video_engine.requirements.viewports == NULL);
    xiaotai_video_status(&status);
    CHECK(status.started && status.requested == 0 && status.generation == 0 && status.ignored == 0);
    CHECK(status.uplink.phase == XIAOTAI_VIDEO_OFF && status.downlink.received == 0);
    CHECK(xiaotai_video_remote_generation() == 0);
    CHECK(!video_engine.uplink_available && !video_engine.downlink_available);
    xiaotai_video_reset();
    xiaotai_video_status(&status);
    CHECK(!status.started, "started after reset");
}

/* ----------------------------------------------------------------- uplink */

static void test_uplink_normalize(void)
{
    setup();
    /* Below the minimum and odd: raised, then made even; rate, bitrate and
     * QP clamped; QP bounds out of range take the defaults 30 and 45. */
    xiaotai_video_profile_t p = k_compact;
    p.width = 300;
    p.height = 201;
    p.fps = 40;
    p.bitrate_bps = 100000;
    p.min_qp = 5;
    p.max_qp = 60;
    p.max_pixels = 1234;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK);
    CHECK(profile_count() == 1);
    const xiaotai_video_profile_t *n = profile_at(0);
    CHECK(n->width == 320 && n->height == 240, "%ux%u", n->width, n->height);
    CHECK(n->fps == 30 && n->bitrate_bps == 200000 && n->min_qp == 30 && n->max_qp == 45, "fps %u bps %u qp %u..%u",
          n->fps, (unsigned)n->bitrate_bps, n->min_qp, n->max_qp);
    CHECK(n->max_pixels == 0, "a fixed size keeps no pixel limit");

    /* Above: odd sides drop to even; rate and bitrate clamped from the other
     * side; a max QP below the min takes 45. */
    p = k_compact;
    p.width = 641;
    p.height = 481;
    p.fps = 3;
    p.bitrate_bps = 20000000;
    p.min_qp = 40;
    p.max_qp = 35;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK);
    n = profile_at(1);
    CHECK(n->width == 640 && n->height == 480 && n->fps == 5 && n->bitrate_bps == 12000000 && n->min_qp == 40 &&
          n->max_qp == 45);
    /* A min QP above the default max: max follows the min. */
    p = k_compact;
    p.min_qp = 50;
    p.max_qp = 20;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK);
    n = profile_at(2);
    CHECK(n->min_qp == 50 && n->max_qp == 50);
    CHECK(profile_count() == 3);

    /* The tuned profiles are already normal: kept as they are. */
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_sensor) == XIAOTAI_OK);
    n = video_require_find_uplink(&k_compact);
    CHECK(n && n->width == 384 && n->height == 256 && n->fps == 12 && n->bitrate_bps == 256000 && n->min_qp == 34 &&
          n->max_qp == 51 && n->key_interval_s == 16);
    n = video_require_find_uplink(&k_sensor);
    CHECK(n && n->width == 0 && n->height == 0 && n->max_pixels == 720u * 960u && n->upright);

    /* Equal after normalization is the same profile: no second entry, and
     * the raw profile finds the normalized one. */
    p = k_compact;
    p.fps = 31;   /* normalizes to 30 */
    xiaotai_video_profile_t q = k_compact;
    q.fps = 30;
    const size_t before = profile_count();
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&q) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    CHECK(profile_count() == before + 1, "%zu profiles", profile_count());
    CHECK(video_require_find_uplink(&p) == video_require_find_uplink(&q));

    /* JPEG is checked, not normalized: 240 x 176 and 1 fps stay. */
    CHECK(xiaotai_video_require_uplink(&k_stream) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_OK);
    n = video_require_find_uplink(&k_stream);
    CHECK(n && n->width == 240 && n->height == 176 && n->fallback_width == 160 && n->quality == 45);
    n = video_require_find_uplink(&k_still);
    CHECK(n && n->fps == 1 && n->refetch_stale);
    /* An undeclared profile is not found. */
    xiaotai_video_profile_t other = k_still;
    other.max_age_ms = 500;
    CHECK(video_require_find_uplink(&other) == NULL);
}

static void test_uplink_invalid(void)
{
    setup();
    CHECK(xiaotai_video_require_uplink(NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_video_profile_t p = k_compact;
    p.codec = XIAOTAI_VIDEO_CODEC_NONE;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p.codec = XIAOTAI_CODEC_OPUS;   /* audio */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.height = 0;     /* one side only */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.max_frame_bytes = 0;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.key_interval_s = 0;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.quality = 45;   /* a JPEG field on H.264 */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.fallback_width = 320, p.fallback_height = 240;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);

    p = k_stream, p.fps = 0;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.fps = 31;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.quality = 0;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.quality = 101;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.min_quality = 50;   /* above the start quality */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.bitrate_bps = 1000;  /* an H.264 field on JPEG */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.fallback_width = 320;   /* larger than the size */
    p.fallback_height = 240;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.fallback_height = 0;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.fallback_width = p.width, p.fallback_height = p.height;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);
    p = k_stream, p.width = 0, p.height = 0;   /* a fallback needs a fixed size */
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_INVALID_ARG);

    CHECK(profile_count() == 0 && allocations() == 0, "a refusal stored or allocated");
}

static void test_uplink_table(void)
{
    setup();
    CHECK(allocations() == 0);
    xiaotai_video_profile_t p = k_stream;
    for (unsigned i = 0; i < XIAOTAI_VIDEO_PROFILES_MAX; ++i) {
        p.fps = (uint8_t)(1U + i);
        CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK, "profile %u", i);
        /* The table is one BULK block, taken by the first declaration. */
        CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1, "after %u: %u BULK allocations", i,
              fake_mem_allocations(XIAOTAI_MEM_BULK));
    }
    CHECK(fake_mem_allocations(XIAOTAI_MEM_INTERNAL) == 0 && fake_mem_allocations(XIAOTAI_MEM_DMA) == 0);
    p.fps = 30;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_ERR_NO_MEM, "a profile beyond the capacity was taken");
    p.fps = 1;
    CHECK(xiaotai_video_require_uplink(&p) == XIAOTAI_OK, "an equal profile in a full table is no new entry");
    CHECK(profile_count() == XIAOTAI_VIDEO_PROFILES_MAX);

    /* A table that cannot be allocated: NO_MEM, nothing kept, and the next
     * declaration tries again. */
    setup();
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_ERR_NO_MEM);
    CHECK(profile_count() == 0 && video_engine.requirements.profiles == NULL);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_OK && profile_count() == 1);
    xiaotai_video_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0, "reset left %lld", (long long)xiaotai_mem_host_outstanding());
}

/* --------------------------------------------------------------- downlink */

static void test_downlink(void)
{
    setup();
    CHECK(video_require_downlink(XIAOTAI_VIDEO_CODEC_H264) == NULL);
    CHECK(xiaotai_video_require_downlink(&k_h264_down) == XIAOTAI_OK);
    const xiaotai_video_downlink_t larger = {.codec = XIAOTAI_VIDEO_CODEC_H264,
                                             .max_width = 640,
                                             .max_height = 200,
                                             .fps = 15,
                                             .max_frame_bytes = 512u * 1024u,
                                             .burst_ms = 0,
                                             .key_refresh_ms = 0};
    CHECK(xiaotai_video_require_downlink(&larger) == XIAOTAI_OK);
    /* One description per codec, the largest of each field. */
    const xiaotai_video_downlink_t *d = video_require_downlink(XIAOTAI_VIDEO_CODEC_H264);
    CHECK(d && d->max_width == 640 && d->max_height == 256 && d->fps == 15 && d->max_frame_bytes == 512u * 1024u &&
          d->burst_ms == 2000u && d->key_refresh_ms == 1000);

    const xiaotai_video_downlink_t jpeg_a = XIAOTAI_VIDEO_DOWNLINK_JPEG(640, 480);
    const xiaotai_video_downlink_t jpeg_b = XIAOTAI_VIDEO_DOWNLINK_JPEG(1024, 608);
    CHECK(xiaotai_video_require_downlink(&jpeg_a) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_downlink(&jpeg_b) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_downlink(&jpeg_a) == XIAOTAI_OK);
    d = video_require_downlink(XIAOTAI_VIDEO_CODEC_JPEG);
    CHECK(d && d->max_width == 1024 && d->max_height == 608 && d->burst_ms == 0 && d->key_refresh_ms == 0);
    /* The H.264 entry is untouched by JPEG declarations. */
    CHECK(video_require_downlink(XIAOTAI_VIDEO_CODEC_H264)->max_width == 640);
    /* Downlinks need no table: nothing was allocated. */
    CHECK(allocations() == 0, "%u allocations", allocations());

    xiaotai_video_downlink_t bad = k_h264_down;
    CHECK(xiaotai_video_require_downlink(NULL) == XIAOTAI_ERR_INVALID_ARG);
    bad.codec = XIAOTAI_CODEC_PCM16;
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = k_h264_down, bad.max_width = 0;
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = k_h264_down, bad.max_height = 0;
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = k_h264_down, bad.max_frame_bytes = 0;
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = k_h264_down, bad.fps = 0;   /* buffered playout needs the rate */
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = jpeg_a, bad.fps = 12, bad.burst_ms = 1000;   /* JPEG is never buffered */
    CHECK(xiaotai_video_require_downlink(&bad) == XIAOTAI_ERR_INVALID_ARG);
    d = video_require_downlink(XIAOTAI_VIDEO_CODEC_H264);
    CHECK(d->max_width == 640 && d->burst_ms == 2000u, "a refused declaration changed the entry");
}

/* -------------------------------------------------------------- viewports */

static xiaotai_video_viewport_t plain_viewport(uint8_t display, uint16_t width, uint16_t height,
                                               xiaotai_video_pixel_t pixel)
{
    xiaotai_video_viewport_t v;
    memset(&v, 0, sizeof(v));
    v.display = display;
    v.width = width;
    v.height = height;
    v.pixel = pixel;
    v.fit = XIAOTAI_VIDEO_FIT_CONTAIN;
    v.match_orientation = false;
    v.rotation = XIAOTAI_VIDEO_ROTATE_0;
    v.frames = 0;
    return v;
}

static void test_viewport(void)
{
    setup();
    fake_display_add(480, 320, XIAOTAI_PIXEL_RGB565, XIAOTAI_DISPLAY_SWAP_BYTES);
    fake_display_add(1024, 600, XIAOTAI_PIXEL_RGB565, 0);
    const xiaotai_video_viewport_t base = plain_viewport(0, 480, 320, XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED);
    CHECK(xiaotai_video_require_viewport(&base) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1 && fake_mem_allocations(XIAOTAI_MEM_INTERNAL) == 0);
    const video_viewport_entry_t *entry = video_require_viewport_for(0);
    CHECK(entry && entry->default_frames && entry->viewport.frames == 0);

    /* The same viewport again: accepted, the larger frame count kept, and
     * the declaration that left the count to the engine is remembered. */
    xiaotai_video_viewport_t v = base;
    v.frames = 5;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_OK);
    v.frames = 20;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_OK);
    v.frames = 4;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_OK);
    entry = video_require_viewport_for(0);
    CHECK(entry->viewport.frames == 20 && entry->default_frames);
    CHECK(video_engine.requirements.viewport_count == 1 && fake_mem_allocations(XIAOTAI_MEM_BULK) == 1);

    /* Any difference in area, pixel or placement is refused. */
    v = base, v.width = 240;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    v = base, v.height = 160;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    v = base, v.pixel = XIAOTAI_VIDEO_PIXEL_RGB565;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    v = base, v.fit = XIAOTAI_VIDEO_FIT_COVER;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    v = base, v.rotation = XIAOTAI_VIDEO_ROTATE_90;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    v = base, v.match_orientation = true;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    entry = video_require_viewport_for(0);
    CHECK(entry->viewport.frames == 20 && entry->viewport.fit == XIAOTAI_VIDEO_FIT_CONTAIN &&
          entry->viewport.rotation == XIAOTAI_VIDEO_ROTATE_0 && entry->viewport.width == 480);

    /* Out of range: no such display, an empty or too large area, a layout
     * a presenter cannot hold, fewer than 3 frames, unknown fit or turn. */
    v = base, v.display = 2;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.width = 0;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.width = 481;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.pixel = XIAOTAI_VIDEO_PIXEL_YUYV422;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.frames = 2;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.fit = (xiaotai_video_fit_t)2;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    v = base, v.rotation = (xiaotai_video_rotation_t)4;
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_video_require_viewport(NULL) == XIAOTAI_ERR_INVALID_ARG);

    /* The outcome does not depend on the order: on display 1, whichever of
     * two different viewports comes second is refused. */
    xiaotai_video_viewport_t cover = plain_viewport(1, 1024, 600, XIAOTAI_VIDEO_PIXEL_RGB565);
    cover.fit = XIAOTAI_VIDEO_FIT_COVER;
    const xiaotai_video_viewport_t contain = plain_viewport(1, 1024, 600, XIAOTAI_VIDEO_PIXEL_RGB565);
    CHECK(xiaotai_video_require_viewport(&cover) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_viewport(&contain) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(video_require_viewport_for(1)->viewport.fit == XIAOTAI_VIDEO_FIT_COVER);
    xiaotai_video_reset();
    CHECK(xiaotai_video_require_viewport(&contain) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_viewport(&cover) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(video_require_viewport_for(1)->viewport.fit == XIAOTAI_VIDEO_FIT_CONTAIN);
    CHECK(video_require_viewport_for(0) == NULL);
}

static void test_viewport_table(void)
{
    setup();
    for (unsigned i = 0; i <= XIAOTAI_VIDEO_VIEWPORTS_MAX; ++i) fake_display_add(320, 240, XIAOTAI_PIXEL_RGB565, 0);
    for (unsigned i = 0; i < XIAOTAI_VIDEO_VIEWPORTS_MAX; ++i) {
        const xiaotai_video_viewport_t v = plain_viewport((uint8_t)i, 320, 240, XIAOTAI_VIDEO_PIXEL_RGB565);
        CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_OK, "viewport %u", i);
    }
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1);
    const xiaotai_video_viewport_t extra =
        plain_viewport((uint8_t)XIAOTAI_VIDEO_VIEWPORTS_MAX, 320, 240, XIAOTAI_VIDEO_PIXEL_RGB565);
    CHECK(xiaotai_video_require_viewport(&extra) == XIAOTAI_ERR_NO_MEM);
    /* A repeat for a display already in the full table is still accepted. */
    const xiaotai_video_viewport_t repeat = plain_viewport(0, 320, 240, XIAOTAI_VIDEO_PIXEL_RGB565);
    CHECK(xiaotai_video_require_viewport(&repeat) == XIAOTAI_OK);

    setup();
    fake_display_add(320, 240, XIAOTAI_PIXEL_RGB565, 0);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_video_require_viewport(&repeat) == XIAOTAI_ERR_NO_MEM);
    CHECK(video_engine.requirements.viewports == NULL && video_require_viewport_for(0) == NULL);
    CHECK(xiaotai_video_require_viewport(&repeat) == XIAOTAI_OK);
}

/* ---------------------------------------------------------- after start */

static void test_after_start(void)
{
    setup();
    fake_display_add(480, 320, XIAOTAI_PIXEL_RGB565, 0);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    xiaotai_engine_video_start();
    const xiaotai_video_viewport_t v = plain_viewport(0, 480, 320, XIAOTAI_VIDEO_PIXEL_RGB565);
    CHECK(xiaotai_video_require_uplink(&k_stream) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_video_require_downlink(&k_h264_down) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(profile_count() == 1 && video_require_downlink(XIAOTAI_VIDEO_CODEC_H264) == NULL &&
          video_require_viewport_for(0) == NULL);
    /* A second start changes nothing. */
    const unsigned tasks = fake_task_count();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == tasks);
    xiaotai_video_reset();
    CHECK(xiaotai_video_require_uplink(&k_stream) == XIAOTAI_OK);
}

/* ------------------------------------------------------------------- tasks */

static void check_task(const char *name, unsigned priority, uint32_t stack_bytes, xiaotai_mem_kind_t stack, int core)
{
    const fake_task_record_t *t = fake_task_named(name);
    CHECK(t, "no task %s", name);
    CHECK(t->config.priority == priority, "%s priority %u", name, t->config.priority);
    CHECK(t->config.stack_bytes == stack_bytes, "%s stack %u", name, (unsigned)t->config.stack_bytes);
    CHECK(t->config.stack == stack, "%s stack kind %d", name, (int)t->config.stack);
    CHECK(t->config.core == core, "%s core %d", name, t->config.core);
    CHECK(t->config.entry != NULL && t->config.arg != NULL);
}

/* A parallel camera that delivers YUYV pictures in a range of sizes. */
static const xiaotai_camera_desc_t k_parallel_camera = {
    .formats = XIAOTAI_CAMERA_YUV422, .max_width = 640, .max_height = 480, .min_width = 0, .min_height = 0,
    .max_fps = 15, .rotation = 0, .row_align = 0, .flags = 0};
/* A CSI sensor with one size, mounted a quarter turn off, full range, rows
 * padded to 16. */
static const xiaotai_camera_desc_t k_csi_camera = {
    .formats = XIAOTAI_CAMERA_YUV420_PAIRED, .max_width = 1280, .max_height = 960, .min_width = 1280,
    .min_height = 960, .max_fps = 30, .rotation = 3, .row_align = 16, .flags = XIAOTAI_CAMERA_FULL_RANGE};

static void software_jpeg_encoder(size_t i)
{
    fake_encoders[i].load = XIAOTAI_VIDEO_LOAD_SOFTWARE;
    fake_encoder_ctl[i].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_encoder_ctl[i].pixel = XIAOTAI_VIDEO_PIXEL_YUYV422;
    fake_encoder_ctl[i].score = 10;
    CHECK(xiaotai_video_encoder_register(&fake_encoders[i]) == XIAOTAI_OK);
}

static void hardware_h264_encoder(size_t i)
{
    fake_encoders[i].load = XIAOTAI_VIDEO_LOAD_HARDWARE;
    fake_encoder_ctl[i].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_encoder_ctl[i].pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_encoder_ctl[i].score = 10;
    CHECK(xiaotai_video_encoder_register(&fake_encoders[i]) == XIAOTAI_OK);
}

static void test_tasks_jpeg_uplink(void)
{
    setup();
    fake_camera_t *camera = fake_camera_add(&k_parallel_camera, false);
    software_jpeg_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_stream) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_OK);
    xiaotai_mem_host_clear();
    fake_port_clear();
    xiaotai_engine_video_start();
    /* A JPEG uplink: the session task and the software camera task, both
     * stacks in BULK, and nothing else. */
    CHECK(fake_task_count() == 2, "%u tasks", fake_task_count());
    check_task("video_session", 5, 8192, XIAOTAI_MEM_BULK, 1);
    check_task("video_uplink", 3, 16384, XIAOTAI_MEM_BULK, XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE);
    /* Start takes no camera, encoder or frame memory for a software encoder:
     * no allocation, no encoder opened, no camera call. */
    CHECK(allocations() == 0, "%u allocations at start", allocations());
    CHECK(fake_encoder_ctl[0].open_calls == 0 && camera->log_count == 0);
    CHECK(video_engine.uplink_available && !video_engine.downlink_available);
    /* The tasks are stopped by reset. */
    xiaotai_video_reset();
    CHECK(!video_engine.tasks[VIDEO_TASK_SESSION].created && !video_engine.tasks[VIDEO_TASK_UPLINK].created);
}

static void test_tasks_full(void)
{
    setup();
    fake_camera_add(&k_csi_camera, true);
    fake_display_add(480, 320, XIAOTAI_PIXEL_RGB565, XIAOTAI_DISPLAY_SWAP_BYTES);
    hardware_h264_encoder(0);
    /* The compact profile needs a transform from the single-size sensor. */
    fake_transform_ctl[0].from = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_transform_ctl[0].score = 5;
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    /* A software H.264 decoder and a hardware JPEG decoder held from init,
     * whose task stack must be internal. */
    fake_decoders[0].load = XIAOTAI_VIDEO_LOAD_SOFTWARE;
    fake_decoders[0].flags = XIAOTAI_VIDEO_DECODER_BASELINE_ONLY;
    fake_decoders[0].stack_bytes = 16384;
    fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_decoder_ctl[0].score = 5;
    fake_decoders[1].load = XIAOTAI_VIDEO_LOAD_HARDWARE;
    fake_decoders[1].flags = XIAOTAI_VIDEO_DECODER_PERSISTENT;
    fake_decoders[1].stack_bytes = 8192;
    fake_decoders[1].stack_internal = true;
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_decoder_ctl[1].score = 5;
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);

    const xiaotai_video_downlink_t jpeg = XIAOTAI_VIDEO_DOWNLINK_JPEG(640, 480);
    xiaotai_video_viewport_t v = plain_viewport(0, 480, 320, XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED);
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_sensor) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_downlink(&k_h264_down) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_downlink(&jpeg) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_viewport(&v) == XIAOTAI_OK);
    fake_port_clear();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 6, "%u tasks", fake_task_count());
    check_task("video_session", 5, 8192, XIAOTAI_MEM_BULK, 1);
    check_task("video_uplink", XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY, 16384, XIAOTAI_MEM_BULK, XIAOTAI_CORE_ANY);
    check_task("video_decode_sw", 5, 16384, XIAOTAI_MEM_BULK, XIAOTAI_CORE_ANY);
    check_task("video_relay", 4, 3072, XIAOTAI_MEM_BULK, XIAOTAI_CORE_ANY);
    check_task("video_convert", 4, 8192, XIAOTAI_MEM_BULK, XIAOTAI_CORE_ANY);
    check_task("video_decode_hw", XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY, 8192, XIAOTAI_MEM_INTERNAL, XIAOTAI_CORE_ANY);
    /* Priority order the product relies on: hardware codec tasks above relay
     * and convert, which are above software encoding. */
    CHECK(XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY > 4);
    CHECK(video_engine.uplink_available && video_engine.downlink_available);
    xiaotai_video_reset();
    for (size_t kind = 0; kind < VIDEO_TASK_KINDS; ++kind) CHECK(!video_engine.tasks[kind].created);

    /* Only a JPEG downlink with a hardware decoder: no relay, no convert. */
    setup();
    fake_display_add(480, 320, XIAOTAI_PIXEL_RGB565, 0);
    fake_decoders[1].load = XIAOTAI_VIDEO_LOAD_HARDWARE;
    fake_decoders[1].stack_bytes = 0;   /* the engine's default */
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_decoder_ctl[1].score = 5;
    CHECK(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_downlink(&jpeg) == XIAOTAI_OK);
    const xiaotai_video_viewport_t jpeg_view = plain_viewport(0, 480, 320, XIAOTAI_VIDEO_PIXEL_RGB565);
    CHECK(xiaotai_video_require_viewport(&jpeg_view) == XIAOTAI_OK);
    fake_port_clear();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 2, "%u tasks", fake_task_count());
    check_task("video_session", 5, 8192, XIAOTAI_MEM_BULK, 1);
    check_task("video_decode_hw", XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY, 8192, XIAOTAI_MEM_BULK, XIAOTAI_CORE_ANY);
    CHECK(!video_engine.uplink_available && video_engine.downlink_available);
    xiaotai_video_reset();
}

static void test_tasks_mixed_and_failures(void)
{
    /* A software and a hardware encoder on one camera: one camera task, at
     * the software encoder's place (below relay and convert). */
    setup();
    const xiaotai_camera_desc_t both = {.formats = XIAOTAI_CAMERA_YUV422 | XIAOTAI_CAMERA_YUV420_PAIRED,
                                        .max_width = 1280,
                                        .max_height = 960};
    fake_camera_add(&both, false);
    software_jpeg_encoder(0);
    hardware_h264_encoder(1);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_still) == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    fake_port_clear();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 2);
    check_task("video_uplink", 3, 16384, XIAOTAI_MEM_BULK, XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE);
    xiaotai_video_reset();

    /* An uplink no encoder takes: no camera task, the uplink unavailable. */
    setup();
    xiaotai_camera_desc_t rgb_only = k_parallel_camera;
    rgb_only.formats = XIAOTAI_CAMERA_RGB565;
    fake_camera_add(&rgb_only, false);
    hardware_h264_encoder(0);   /* no RGB-to-YUV transform is registered */
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    fake_port_clear();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 1 && fake_task_named("video_session"));
    CHECK(!video_engine.uplink_available);
    xiaotai_video_reset();

    /* A declared profile still visits a camera that must reserve its own
     * boot resources, even with no compatible encoder in this build. */
    setup();
    fake_camera_t *prepare_only = fake_camera_add(&rgb_only, true);
    hardware_h264_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_compact) == XIAOTAI_OK);
    fake_port_clear();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 2 && fake_task_named("video_uplink"));
    CHECK(prepare_only->log_count == 1 && prepare_only->log[0] == FAKE_CAMERA_PREPARE);
    CHECK(!video_engine.uplink_available);
    xiaotai_video_reset();

    /* The camera task cannot be created: the engine still starts, the
     * uplink is unavailable and the failure is in the event log. */
    setup();
    fake_camera_add(&k_parallel_camera, false);
    software_jpeg_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_require_uplink(&k_stream) == XIAOTAI_OK);
    fake_port_clear();
    fake_task_fail_nth(2, XIAOTAI_ERR_NO_MEM);
    const uint32_t before = xiaotai_eventlog_latest();
    xiaotai_engine_video_start();
    CHECK(fake_task_count() == 2);
    xiaotai_video_status_t status;
    xiaotai_video_status(&status);
    CHECK(status.started && video_started());
    CHECK(video_engine.tasks[VIDEO_TASK_SESSION].created && !video_engine.tasks[VIDEO_TASK_UPLINK].created);
    CHECK(!video_engine.uplink_available);
    CHECK(logged(before, "task.create_failed"));
    xiaotai_video_reset();
}

/* ------------------------------------------------------------ capabilities */

static void test_capabilities(void)
{
    setup();
    xiaotai_video_capabilities_t caps;
    memset(&caps, 0xA5, sizeof(caps));
    xiaotai_video_capabilities(&caps);
    CHECK(caps.uplink_codecs == 0 && caps.downlink_codecs == 0 && caps.camera_width == 0, "not zero before init");

    /* No camera: no uplink codec, whatever encoders exist. */
    software_jpeg_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    xiaotai_video_capabilities(&caps);
    CHECK(caps.uplink_codecs == 0 && caps.camera_width == 0 && caps.camera_height == 0);
    xiaotai_video_reset();

    /* The CSI sensor: its size, mounting and range; H.264 straight from its
     * pictures; JPEG via the built-in transform into YUYV. */
    setup();
    fake_camera_add(&k_csi_camera, true);
    hardware_h264_encoder(0);
    software_jpeg_encoder(1);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    xiaotai_video_capabilities(&caps);
    CHECK(caps.camera_width == 1280 && caps.camera_height == 960 && caps.camera_rotation == XIAOTAI_VIDEO_ROTATE_270 &&
          caps.camera_full_range);
    CHECK(caps.uplink_codecs ==
          (XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264) | XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG)),
          "uplink 0x%x",
          (unsigned)caps.uplink_codecs);
    /* The probe the H.264 encoder saw: the camera's layout, size, padded rows
     * and range. */
    const xiaotai_video_encoder_config_t *seen = &fake_encoder_ctl[0].matched_encoder;
    CHECK(seen->width == 1280 && seen->height == 960 && seen->rows == 960 && seen->full_range &&
          seen->pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED);
    CHECK(caps.downlink_codecs == 0);
    xiaotai_video_reset();

    setup();
    fake_camera_add(&k_csi_camera, true);
    hardware_h264_encoder(0);
    software_jpeg_encoder(1);
    fake_transform_ctl[0].from = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_transform_ctl[0].pixel = XIAOTAI_VIDEO_PIXEL_YUYV422;
    fake_transform_ctl[0].score = 1;
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_decoder_ctl[0].score = 1;
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    xiaotai_video_capabilities(&caps);
    CHECK(caps.uplink_codecs ==
          (XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264) | XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG)));
    CHECK(caps.downlink_codecs == XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264));
    CHECK(fake_decoder_ctl[0].last_decoder.max_width == 0, "decoders are probed at their own limit");
    xiaotai_video_reset();
}

/* ----------------------------------------------------------------- resolve */

static bool resolve_steps(xiaotai_video_pixel_t in, xiaotai_video_pixel_t out, xiaotai_video_scale_limits_t *limits)
{
    if (in != XIAOTAI_VIDEO_PIXEL_RGB565 || out != XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) return false;
    *limits = (xiaotai_video_scale_limits_t){.denominator = 16, .step = 2, .max = 16};
    return true;
}

static void test_resolve(void)
{
    setup();
    fake_camera_add(&k_csi_camera, true);
    hardware_h264_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    uint16_t w = 0, h = 0;
    /* Single-size sensor, no hardware scaler: the known software fallback
     * can supply the compact and mounted native-size profiles. */
    CHECK(xiaotai_video_resolve(&k_compact, 0, &w, &h) == XIAOTAI_OK && w == 384 && h == 256);
    const xiaotai_video_profile_t sensor = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
    CHECK(xiaotai_video_resolve(&sensor, 0, &w, &h) == XIAOTAI_OK && w == 720 && h == 960);
    CHECK(xiaotai_video_resolve(&k_compact, 1, &w, &h) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_video_reset();

    setup();
    fake_camera_add(&k_csi_camera, true);
    hardware_h264_encoder(0);
    fake_transform_ctl[0].from = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_transform_ctl[0].pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_transform_ctl[0].score = 5;
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_resolve(&k_compact, 0, &w, &h) == XIAOTAI_OK && w == 384 && h == 256, "%ux%u", w, h);
    /* The encoder was asked for the transformed picture, tightly packed. */
    CHECK(fake_encoder_ctl[0].matched_encoder.width == 384 && fake_encoder_ctl[0].matched_encoder.rows == 256 &&
          fake_encoder_ctl[0].matched_encoder.fps == 12 &&
          fake_encoder_ctl[0].matched_encoder.key_interval_frames == 192);
    /* Larger than the camera's picture. */
    xiaotai_video_profile_t p = k_compact;
    p.width = 1280;
    p.height = 1024;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_ERR_NOT_SUPPORTED);
    /* Upright on a sensor mounted a quarter turn off: the turned picture is
     * 960 x 1280, so 960 x 1280 fits and 1000 x 1200 does not. */
    p.upright = true;
    p.width = 960;
    p.height = 1280;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_OK && w == 960 && h == 1280);
    p.width = 1000;
    p.height = 1200;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_ERR_NOT_SUPPORTED);
    /* Normalized before resolving: 300 x 200 is 320 x 240. */
    p = k_compact;
    p.width = 300;
    p.height = 200;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_OK && w == 320 && h == 240);
    /* A codec no encoder takes. */
    CHECK(xiaotai_video_resolve(&k_still, 0, &w, &h) == XIAOTAI_ERR_NOT_SUPPORTED);
    /* Arguments. */
    CHECK(xiaotai_video_resolve(NULL, 0, &w, &h) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_video_resolve(&k_compact, 0, NULL, &h) == XIAOTAI_ERR_INVALID_ARG);
    p = k_compact, p.key_interval_s = 0;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_video_reset();

    /* A stepped transform is the only route from this layout to the encoder.
     * Pick its largest whole-sensor scale under the cap rather than refusing
     * the unattainable unconstrained size or cropping the picture. */
    setup();
    xiaotai_camera_desc_t rgb_source = k_csi_camera;
    rgb_source.formats = XIAOTAI_CAMERA_RGB565;
    fake_camera_add(&rgb_source, true);
    hardware_h264_encoder(0);
    fake_transform_ctl[0].from = XIAOTAI_VIDEO_PIXEL_RGB565;
    fake_transform_ctl[0].pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    fake_transform_ctl[0].score = 5;
    fake_transforms[0].limits = resolve_steps;
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    p = sensor;
    p.max_pixels = 500000;
    CHECK(xiaotai_video_resolve(&p, 0, &w, &h) == XIAOTAI_OK && w == 600 && h == 800);
    xiaotai_video_reset();

    /* A parallel camera opens the profile's size itself: no transform needed. */
    setup();
    fake_camera_add(&k_parallel_camera, false);
    software_jpeg_encoder(0);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_resolve(&k_stream, 0, &w, &h) == XIAOTAI_OK && w == 240 && h == 176);
    CHECK(xiaotai_video_resolve(&k_still, 0, &w, &h) == XIAOTAI_OK && w == 320 && h == 240);
    CHECK(fake_encoder_ctl[0].matched_encoder.quality == 45 && fake_encoder_ctl[0].matched_encoder.fps == 1);
    xiaotai_video_reset();
}

static void test_phone_scene_geometry(void)
{
    const struct {
        uint16_t sensor_width, sensor_height, width, height, rows;
        uint8_t rotation, row_align;
    } scenes[] = {
        {1280, 960, 720, 960, 960, 3, 0},
        {1024, 600, 1024, 600, 608, 0, 16},
        /* A smaller sensor must still work, without upscaling. */
        {640, 480, 480, 640, 640, 3, 0},
        {640, 480, 640, 480, 480, 0, 16},
    };
    for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
        setup();
        xiaotai_camera_desc_t camera = k_csi_camera;
        camera.min_width = camera.max_width = scenes[i].sensor_width;
        camera.min_height = camera.max_height = scenes[i].sensor_height;
        camera.rotation = scenes[i].rotation;
        camera.row_align = scenes[i].row_align;
        fake_camera_add(&camera, true);
        hardware_h264_encoder(0);
        CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
        uint16_t width, height;
        const xiaotai_video_profile_t profile = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
        CHECK(xiaotai_video_resolve(&profile, 0, &width, &height) == XIAOTAI_OK);
        CHECK(width == scenes[i].width && height == scenes[i].height);
        video_h264_plan_t plan;
        CHECK(video_h264_plan(&camera, &profile, width, height, &plan));
        CHECK(plan.mode.width == scenes[i].sensor_width && plan.mode.height == scenes[i].sensor_height);
        CHECK(plan.config.width == width && plan.config.height == height && plan.config.fps == 15);
        CHECK(plan.config.rows == scenes[i].rows);
        CHECK(xiaotai_video_resolve(&k_compact, 0, &width, &height) == XIAOTAI_OK);
        CHECK(width == 384 && height == 256);
        CHECK(video_h264_plan(&camera, &k_compact, width, height, &plan));
        CHECK(plan.mode.width == scenes[i].sensor_width && plan.mode.height == scenes[i].sensor_height);
        CHECK(plan.config.fps == 12);
        xiaotai_video_reset();
    }
}

int main(void)
{
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    test_nothing_declared();
    test_uplink_normalize();
    test_uplink_invalid();
    test_uplink_table();
    test_downlink();
    test_viewport();
    test_viewport_table();
    test_after_start();
    test_tasks_jpeg_uplink();
    test_tasks_full();
    test_tasks_mixed_and_failures();
    test_capabilities();
    test_resolve();
    test_phone_scene_geometry();
    xiaotai_video_reset();
    return 0;
}
