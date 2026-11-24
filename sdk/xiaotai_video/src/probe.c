// SPDX-License-Identifier: MIT
#include "probe.h"

#include <inttypes.h>

#include "geometry.h"
#include "internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"

enum { FRAME_COUNT = 3, POLL_MS = 5, BORROW_LIMIT_MS = 3000, CRC_CHUNK = 64 * 1024 };

/* The worker retains the actual descriptor through a failed release. Never
 * lend the HAL a stack variable it might still hold after this job returns. */
static struct {
    const xiaotai_camera_t *camera;
    xiaotai_camera_mode_t mode;
    xiaotai_camera_frame_t frame;
    bool opening, powered, borrowed;
} owner;

static atomic_bool s_pending;
static atomic_uint_least32_t s_dropped;

static void record_report(xiaotai_err_t result)
{
    if (result != XIAOTAI_OK) (void)atomic_fetch_add(&s_dropped, 1U);
}

uint32_t video_probe_dropped_reports(void) { return atomic_load(&s_dropped); }

static const char *format_name(uint32_t format)
{
    switch (format) {
    case XIAOTAI_CAMERA_YUV422: return "yuv422";
    case XIAOTAI_CAMERA_JPEG: return "jpeg";
    case XIAOTAI_CAMERA_RGB565: return "rgb565";
    case XIAOTAI_CAMERA_YUV420_PAIRED: return "yuv420_paired";
    default: return "unknown"; /* A successful HAL borrow normally cannot do this. */
    }
}

static void report_result(unsigned frames, xiaotai_err_t result)
{
    char scratch[24];
    record_report(xiaotai_at_notifyf("+VIDEO-PROBE: result=%s frames=%u error=%s",
        result == XIAOTAI_OK ? "passed" : "failed", frames,
        result == XIAOTAI_OK ? "NONE" : xiaotai_at_error_name(result, scratch, sizeof(scratch))));
}

/* Reflected CRC-32 (zlib/IEEE). Yield between chunks of native camera data. */
static uint32_t crc32(const uint8_t *data, size_t size)
{
    static const uint32_t nibble[] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t crc = UINT32_MAX;
    while (size) {
        const size_t chunk = size > CRC_CHUNK ? CRC_CHUNK : size;
        for (size_t i = 0; i < chunk; ++i) {
            crc ^= *data++;
            crc = (crc >> 4) ^ nibble[crc & 15U];
            crc = (crc >> 4) ^ nibble[crc & 15U];
        }
        size -= chunk;
        if (size) xiaotai_delay_ms(1);
    }
    return ~crc;
}

static bool frame_matches(const xiaotai_camera_frame_t *f)
{
    if (!f->data || f->width != owner.mode.width || f->height != owner.mode.height ||
        f->format != owner.mode.format) return false;
    const uint8_t alignment = owner.camera->desc.row_align ? owner.camera->desc.row_align : 1;
    const uint32_t padded = ((uint32_t)f->height + alignment - 1U) / alignment * alignment;
    if (f->rows != f->height && f->rows != padded) return false;
    if (f->format == XIAOTAI_CAMERA_JPEG)
        return f->size >= 4U && f->data[0] == 0xff && f->data[1] == 0xd8 &&
               f->data[f->size - 2U] == 0xff && f->data[f->size - 1U] == 0xd9;
    const size_t bytes = video_geometry_image_bytes(video_camera_pixel(f->format), f->width, f->rows);
    /* The HAL allows a device tail beyond the required storage rows. */
    return bytes && f->size >= bytes;
}

static xiaotai_err_t release_frame(void)
{
    if (!owner.borrowed) return XIAOTAI_OK;
    const bool had_data = owner.frame.data != NULL;
    const xiaotai_err_t error = owner.camera->release(owner.camera->self, &owner.frame);
    /* Camera parts may return a requeue error after consuming the loan. */
    if (error == XIAOTAI_OK || (had_data && !owner.frame.data)) owner.borrowed = false;
    return error;
}

static xiaotai_err_t close_camera(void)
{
    if (!owner.opening) return XIAOTAI_OK;
    const xiaotai_err_t error = owner.camera->close(owner.camera->self);
    if (error == XIAOTAI_OK) owner.opening = false;
    return error;
}

static xiaotai_err_t cleanup(void)
{
    if (!owner.camera) return XIAOTAI_OK;
    const xiaotai_err_t release = release_frame();
    if (owner.borrowed) return release;
    const xiaotai_err_t closed = close_camera();
    if (closed != XIAOTAI_OK) return release == XIAOTAI_OK ? closed : release;
    if (owner.powered) {
        const xiaotai_err_t power = owner.camera->set_power(owner.camera->self, false);
        if (power != XIAOTAI_OK) return release == XIAOTAI_OK ? power : release;
    }
    owner.camera = NULL;
    owner.powered = false;
    return release;
}

bool video_probe_blocks_camera(void) { return owner.camera != NULL; }

static xiaotai_err_t open_mode(bool (*cancelled)(void))
{
    const xiaotai_camera_desc_t *desc = &owner.camera->desc;
    const uint32_t formats[] = {XIAOTAI_CAMERA_YUV422, XIAOTAI_CAMERA_YUV420_PAIRED,
        XIAOTAI_CAMERA_RGB565, XIAOTAI_CAMERA_JPEG};
    const uint16_t width[] = {desc->max_width, desc->min_width};
    const uint16_t height[] = {desc->max_height, desc->min_height};
    if (!(desc->formats & (XIAOTAI_CAMERA_YUV422 | XIAOTAI_CAMERA_YUV420_PAIRED |
                         XIAOTAI_CAMERA_RGB565 | XIAOTAI_CAMERA_JPEG)) || !width[0] || !height[0])
        return XIAOTAI_ERR_NOT_SUPPORTED;
    if (cancelled && cancelled()) return XIAOTAI_ERR_BUSY;
    owner.powered = true; /* a failed power-on still needs a matching power-off */
    xiaotai_err_t error = owner.camera->set_power(owner.camera->self, true);
    if (error != XIAOTAI_OK) return error;

    for (size_t size = 0; size < 2U; ++size) {
        if (!width[size] || !height[size] || (size && width[0] == width[1] && height[0] == height[1])) continue;
        for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
            if (!(desc->formats & formats[i])) continue;
            if (cancelled && cancelled()) return XIAOTAI_ERR_BUSY;
            owner.mode = (xiaotai_camera_mode_t){.width = width[size], .height = height[size], .format = formats[i]};
            owner.opening = true;
            error = owner.camera->open(owner.camera->self, &owner.mode);
            if (error != XIAOTAI_ERR_NOT_SUPPORTED) return error;
            error = close_camera();
            if (error != XIAOTAI_OK) return XIAOTAI_ERR_NOT_SUPPORTED;
        }
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t borrow(bool (*cancelled)(void))
{
    const int64_t start = xiaotai_time_us();
    for (unsigned waited = 0; ; waited += POLL_MS) {
        if (cancelled && cancelled()) return XIAOTAI_ERR_BUSY;
        owner.frame = (xiaotai_camera_frame_t){0};
        const xiaotai_err_t error = owner.camera->borrow(owner.camera->self, &owner.frame);
        if (error != XIAOTAI_ERR_NOT_FOUND) return error;
        const int64_t now = xiaotai_time_us();
        if (waited >= BORROW_LIMIT_MS || (now >= start &&
            (uint64_t)now - (uint64_t)start >= BORROW_LIMIT_MS * UINT64_C(1000))) return XIAOTAI_ERR_TIMEOUT;
        xiaotai_delay_ms(POLL_MS);
    }
}

static xiaotai_err_t probe(unsigned *frames, video_luma_t *luma, bool (*cancelled)(void))
{
    owner.camera = xiaotai_hal_camera(0);
    if (!owner.camera) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_err_t result = open_mode(cancelled);
    while (result == XIAOTAI_OK && *frames < FRAME_COUNT) {
        result = borrow(cancelled);
        if (result != XIAOTAI_OK) break;
        owner.borrowed = true;
        const xiaotai_camera_frame_t frame = owner.frame;
        const bool valid = frame_matches(&frame);
        if (!*frames) *luma = valid ? video_luma_sample(&frame) : (video_luma_t){.state = VIDEO_LUMA_INVALID};
        const uint32_t crc = frame.data ? crc32(frame.data, frame.size) : 0;
        const xiaotai_err_t released = release_frame();
        record_report(xiaotai_at_notifyf("+VIDEO-PROBE-FRAME: index=%u width=%u height=%u format=%s bytes=%zu"
            " crc32=%08" PRIx32 " ok=%u", (*frames)++, (unsigned)frame.width, (unsigned)frame.height,
            format_name(frame.format), frame.size, crc, (unsigned)valid));
        if (!valid) result = XIAOTAI_ERR_INVALID_SIZE;
        else if (released != XIAOTAI_OK) result = released;
        if (owner.borrowed) return result; /* retain the failed loan, even during close */
    }
    const xiaotai_err_t closed = cleanup();
    return result == XIAOTAI_OK ? closed : result;
}

xiaotai_err_t video_probe_request(void)
{
    video_task_t *task = &video_engine.tasks[VIDEO_TASK_UPLINK];
    if (!video_started()) return XIAOTAI_ERR_INVALID_STATE;
    if (!xiaotai_hal_camera(0)) return XIAOTAI_ERR_NOT_FOUND;
    if (!task->created || atomic_load(&task->stop)) return XIAOTAI_ERR_NOT_SUPPORTED;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_pending, &idle, true)) return XIAOTAI_ERR_BUSY;
    xiaotai_task_notify(&task->handle);
    return XIAOTAI_OK;
}

bool video_probe_work(bool video_busy, bool (*cancelled)(void))
{
    if (!atomic_load(&s_pending)) return false;
    unsigned frames = 0;
    const bool blocked = video_busy || owner.camera;
    video_luma_t luma = {0};
    const xiaotai_err_t result = blocked ? XIAOTAI_ERR_BUSY : probe(&frames, &luma, cancelled);
    if (!blocked) record_report(xiaotai_at_notifyf("+VIDEO-PROBE-LUMA: state=%s y_mean=%d samples=%u",
        video_luma_name(luma.state), video_luma_value(luma), (unsigned)luma.samples));
    report_result(frames, result);
    atomic_store(&s_pending, false);
    return true;
}

void video_probe_shutdown(void)
{
    const xiaotai_err_t result = cleanup();
    if (result != XIAOTAI_OK) xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "probe.cleanup", 0, result);
    atomic_store(&s_pending, false);
}

void video_probe_reset(void)
{
    atomic_store(&s_pending, false);
    atomic_store(&s_dropped, 0);
}
