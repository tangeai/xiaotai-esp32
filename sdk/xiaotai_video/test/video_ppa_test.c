// SPDX-License-Identifier: MIT
#include "driver/ppa.h"
#include "esp_cache.h"
#include "geometry.h"
#include "sdkconfig.h"
#include "xiaotai/port/mem.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

extern const xiaotai_video_transform_t video_esp_ppa_transform;
static const xiaotai_video_transform_t *const backend = &video_esp_ppa_transform;
enum { ALIGN = CONFIG_CACHE_L2_CACHE_LINE_SIZE };
struct ppa_client_t { bool active; ppa_operation_t kind; };
static struct ppa_client_t s_srm, s_fill;

static struct {
    unsigned allocs, live, registrations, releases, srm, fills, syncs, fail_alloc_at;
    bool srm_open_fail, fill_open_fail;
    esp_err_t srm_error, fill_error, sync_error;
    ppa_srm_oper_config_t op;
    ppa_fill_oper_config_t fill;
    const uint8_t *real_input;
    uint8_t packed[12];
    size_t used_bytes, packed_bytes;
} fake;

static size_t padded(size_t size) { return (size + ALIGN - 1U) & ~(size_t)(ALIGN - 1U); }

void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t n, size_t bytes)
{
    assert(kind == XIAOTAI_MEM_INTERNAL);
    ++fake.allocs;
    if (fake.allocs == fake.fail_alloc_at) return NULL;
    ++fake.live;
    return calloc(n, bytes);
}

void *xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t bytes)
{
    assert(kind == XIAOTAI_MEM_BULK && alignment == ALIGN && bytes % ALIGN == 0);
    ++fake.allocs;
    if (fake.allocs == fake.fail_alloc_at) return NULL;
    fake.packed_bytes = bytes;
    ++fake.live;
    return aligned_alloc(alignment, bytes);
}

void xiaotai_mem_free(void *p)
{
    if (!p) return;
    assert(fake.live > 0);
    --fake.live;
    free(p);
}

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *out)
{
    assert(config->max_pending_trans_num == 1 && config->data_burst_length == PPA_DATA_BURST_LENGTH_128);
    assert(config->oper_type == PPA_OPERATION_SRM || config->oper_type == PPA_OPERATION_FILL);
    struct ppa_client_t *client = config->oper_type == PPA_OPERATION_SRM ? &s_srm : &s_fill;
    assert(!client->active && !*out);
    ++fake.registrations;
    if (config->oper_type == PPA_OPERATION_SRM ? fake.srm_open_fail : fake.fill_open_fail) return ESP_ERR_NO_MEM;
    client->active = true;
    client->kind = config->oper_type;
    *out = client;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t client)
{
    assert(client->active);
    if (client == &s_srm) assert(!s_fill.active);
    client->active = false;
    ++fake.releases;
    return ESP_OK;
}

static void output_bytes(void *address, size_t size)
{
    assert((uintptr_t)address % ALIGN == 0 && size == padded(fake.used_bytes));
}

esp_err_t esp_cache_msync(void *address, size_t size, int flags)
{
    output_bytes(address, size);
    assert(flags == ESP_CACHE_MSYNC_FLAG_DIR_C2M && ((const uint8_t *)address)[0] == 0);
    ++fake.syncs;
    return fake.sync_error;
}

esp_err_t ppa_do_fill(ppa_client_handle_t client, const ppa_fill_oper_config_t *config)
{
    assert(client == &s_fill && client->active && config->mode == PPA_TRANS_MODE_BLOCKING);
    assert(config->fill_block_w == config->out.pic_w && config->fill_block_h == config->out.pic_h &&
           config->out.block_offset_x == 0 && config->out.block_offset_y == 0 && config->fill_color_val == 0);
    output_bytes(config->out.buffer, config->out.buffer_size);
    fake.fill = *config;
    ++fake.fills;
    if (fake.fill_error == ESP_OK) memset(config->out.buffer, 0, fake.used_bytes);
    return fake.fill_error;
}

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *config)
{
    assert(client == &s_srm && client->active && config->mode == PPA_TRANS_MODE_BLOCKING);
    output_bytes(config->out.buffer, config->out.buffer_size);
    assert(config->scale_x == config->scale_y);
    fake.op = *config;
    ++fake.srm;
    if (config->in.buffer != fake.real_input) memcpy(fake.packed, config->in.buffer, sizeof(fake.packed));
    if (fake.srm_error != ESP_OK || config->out.srm_cm != PPA_SRM_COLOR_MODE_RGB565) return fake.srm_error;
    const bool turn = config->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                      config->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
    const unsigned w = (unsigned)((turn ? config->in.block_h : config->in.block_w) * config->scale_x);
    const unsigned h = (unsigned)((turn ? config->in.block_w : config->in.block_h) * config->scale_y);
    uint8_t *out = config->out.buffer;
    for (unsigned y = config->out.block_offset_y; y < config->out.block_offset_y + h; ++y) {
        for (unsigned x = config->out.block_offset_x; x < config->out.block_offset_x + w; ++x) {
            out[((size_t)y * config->out.pic_w + x) * 2U] = 0x34;
            out[((size_t)y * config->out.pic_w + x) * 2U + 1] = 0x12;
        }
    }
    return ESP_OK;
}

static void reset(void)
{
    assert(!fake.live && !s_srm.active && !s_fill.active);
    memset(&fake, 0, sizeof(fake));
}

static void geometry(void)
{
    xiaotai_video_scale_limits_t limits;
    assert(backend->output_alignment == ALIGN && backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE);
    assert(backend->limits(XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, &limits));
    assert(limits.denominator == 16 && limits.step == 2 && limits.max == 255);
    assert(backend->limits(XIAOTAI_VIDEO_PIXEL_I420, XIAOTAI_VIDEO_PIXEL_RGB888, &limits) && limits.step == 1);
    assert(!backend->limits(XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, XIAOTAI_VIDEO_PIXEL_YUYV422, &limits));
    assert(!backend->limits(XIAOTAI_VIDEO_PIXEL_RGB565, XIAOTAI_VIDEO_PIXEL_I420, &limits));
    assert(!backend->limits(XIAOTAI_VIDEO_PIXEL_NONE, XIAOTAI_VIDEO_PIXEL_RGB565, &limits));
    assert(!backend->limits(XIAOTAI_VIDEO_PIXEL_RGB565, XIAOTAI_VIDEO_PIXEL_RGB888, NULL));
    assert(!backend->match(NULL));
    xiaotai_video_transform_config_t c = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .in_width = 32, .in_height = 38, .in_stride = 34,
        .crop = {0, 0, 32, 38}, .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .out_width = 12, .out_height = 14,
        .place = {0, 0, 12, 14},
    };
    assert(backend->match(&c) > 1); /* floor(38 * 6/16) == 14 */
    c.place.height = 12;
    assert(!backend->match(&c)); /* representable on neither common scale */
    c.place.height = 14;
    c.in_stride = 0x4000;
    assert(!backend->match(&c) && fake.allocs == 0);
    c.in_stride = 34;
    c.rotation = (xiaotai_video_rotation_t)7;
    assert(!backend->match(&c));

    c = (xiaotai_video_transform_config_t){
        .in_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .in_width = 32, .in_height = 32, .in_stride = 32,
        .crop = {0, 0, 32, 32}, .out_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .out_width = 24, .out_height = 24,
        .place = {0, 0, 14, 14},
    };
    assert(!backend->match(&c)); /* 7/16 would be silently truncated by hardware. */
    c.place.width = c.place.height = 12;
    assert(backend->match(&c) > 1);
    c.fill = true;
    assert(!backend->match(&c)); /* Paired YUV has no native fill. */
    c.fill = false;
    c.in_stride = 34;
    assert(!backend->match(&c)); /* Planes of the public 4:2:0 layouts are tight. */

    assert(backend->limits(XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, &limits));
    xiaotai_video_image_t sensor = {
        .pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .width = 1024, .height = 600, .rows = 608, .stride = 1024,
    };
    xiaotai_video_profile_t profile = {.width = 384, .height = 256};
    assert(video_geometry_uplink(&sensor, &profile, XIAOTAI_VIDEO_ROTATE_0, sensor.pixel, &limits, &c));
    assert(c.in_height == 600 && c.crop.x == 128 && c.crop.y == 44 && c.crop.width == 768 && c.crop.height == 512);
    assert(c.place.width == 384 && c.place.height == 256 && backend->match(&c) > 1);
    sensor.width = sensor.stride = 1280;
    sensor.height = sensor.rows = 960;
    profile.width = 720;
    profile.height = 960;
    profile.upright = true;
    assert(video_geometry_uplink(&sensor, &profile, XIAOTAI_VIDEO_ROTATE_270, sensor.pixel, &limits, &c));
    assert(c.crop.width == 1280 && c.crop.height == 960 && c.rotation == XIAOTAI_VIDEO_ROTATE_270 &&
           c.place.width == 720 && c.place.height == 960 && backend->match(&c) > 1);
}

static void paired(void)
{
    reset();
    const xiaotai_video_transform_config_t c = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .in_width = 32, .in_height = 18, .in_stride = 32,
        .in_full_range = true, .crop = {4, 2, 24, 16}, .rotation = XIAOTAI_VIDEO_ROTATE_270,
        .out_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .out_width = 8, .out_height = 12, .place = {0, 0, 8, 12},
    };
    uint8_t input[32U * 20U * 3U / 2U] = {0};  /* Captured rows beyond visible height are ignored. */
    _Alignas(ALIGN) uint8_t output[16 * ALIGN];
    memset(output, 0xde, sizeof(output));
    fake.real_input = input;
    fake.used_bytes = 8U * 12U * 3U / 2U;
    void *state = NULL;
    assert(backend->open(&c, &state) == XIAOTAI_OK && state && fake.allocs == 1 && !s_fill.active);
    assert(backend->run(state, input, 32 * 18 * 3 / 2 - 1, output, sizeof(output)) == XIAOTAI_ERR_INVALID_SIZE);
    assert(backend->run(state, input, sizeof(input), output, fake.used_bytes) == XIAOTAI_ERR_INVALID_SIZE);
    assert(backend->run(state, input, sizeof(input), output, sizeof(output) - 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(backend->run(state, input, sizeof(input), output + 1, sizeof(output)) == XIAOTAI_ERR_INVALID_ARG);
    assert(backend->run(state, output, sizeof(output), output, sizeof(output)) == XIAOTAI_ERR_INVALID_ARG);
    assert(!fake.srm && !fake.syncs && !fake.fills);
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.op.in.buffer == input && fake.op.in.pic_w == 32 && fake.op.in.pic_h == 18 &&
           fake.op.in.block_w == 24 && fake.op.in.block_h == 16 && fake.op.in.block_offset_x == 4 &&
           fake.op.in.block_offset_y == 2 && fake.op.in.srm_cm == PPA_SRM_COLOR_MODE_YUV420 &&
           fake.op.out.srm_cm == PPA_SRM_COLOR_MODE_YUV420 && fake.op.rotation_angle == PPA_SRM_ROTATION_ANGLE_90 &&
           fake.op.in.yuv_range == PPA_COLOR_RANGE_FULL && fake.op.out.yuv_range == PPA_COLOR_RANGE_FULL &&
           fake.op.in.yuv_std == PPA_COLOR_CONV_STD_RGB_YUV_BT601 && fake.op.scale_x == 0.5f);
    assert(fake.allocs == 1 && fake.srm == 1 && !fake.syncs && !fake.fills && output[fake.used_bytes] == 0xde);
    backend->close(state);
    assert(fake.releases == 1 && !fake.live);
}

static void planar(void)
{
    reset();
    const xiaotai_video_transform_config_t c = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_I420, .in_width = 8, .in_height = 6, .in_stride = 8,
        .crop = {2, 2, 4, 2}, .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .out_width = 8, .out_height = 4,
        .place = {2, 0, 4, 2}, .fill = true,
    };
    uint8_t input[72];
    for (unsigned i = 0; i < 48; ++i) input[i] = (uint8_t)i;
    for (unsigned i = 0; i < 12; ++i) { input[48 + i] = (uint8_t)(100 + i); input[60 + i] = (uint8_t)(170 + i); }
    _Alignas(ALIGN) uint8_t output[2 * ALIGN];
    memset(output, 0xde, sizeof(output));
    fake.real_input = input;
    fake.used_bytes = 8 * 4 * 2;
    void *state = NULL;
    assert(backend->open(&c, &state) == XIAOTAI_OK && fake.allocs == 2 && fake.live == 2 && !s_fill.active);
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == XIAOTAI_OK);
    static const uint8_t expected[] = {105, 18, 19, 106, 20, 21, 175, 26, 27, 176, 28, 29};
    assert(!memcmp(fake.packed, expected, sizeof(expected)) && fake.op.in.buffer != input &&
           fake.op.in.pic_w == 4 && fake.op.in.pic_h == 2 && !fake.op.in.block_offset_x && !fake.op.in.block_offset_y);
    assert(fake.op.in.srm_cm == PPA_SRM_COLOR_MODE_YUV420 && fake.op.in.yuv_range == PPA_COLOR_RANGE_LIMIT &&
           fake.op.out.srm_cm == PPA_SRM_COLOR_MODE_RGB565 && !fake.op.byte_swap);
    assert(fake.allocs == 2 && fake.syncs == 1 && fake.srm == 1 && !fake.fills);
    assert(output[0] == 0 && output[3] == 0 && output[4] == 0x12 && output[5] == 0x34 &&
           output[11] == 0x34 && output[12] == 0 && output[32] == 0 && output[64] == 0xde);
    fake.sync_error = ESP_FAIL;
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == ESP_FAIL && fake.srm == 1);
    fake.sync_error = ESP_OK;
    fake.srm_error = ESP_ERR_TIMEOUT;
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == ESP_ERR_TIMEOUT && fake.srm == 2);
    backend->close(state);
    assert(fake.releases == 1 && !fake.live);

    fake.srm_open_fail = true;
    assert(backend->open(&c, &state) == XIAOTAI_ERR_NO_MEM && state == NULL && !fake.live);
    fake.srm_open_fail = false;
    fake.fail_alloc_at = fake.allocs + 2;
    assert(backend->open(&c, &state) == XIAOTAI_ERR_NO_MEM && state == NULL && !fake.live && !s_srm.active);
    fake.fail_alloc_at = fake.allocs + 1;
    assert(backend->open(&c, &state) == XIAOTAI_ERR_NO_MEM && state == NULL && !fake.live);
}

static void rgb(void)
{
    reset();
    xiaotai_video_transform_config_t c = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .in_width = 8, .in_height = 4, .in_stride = 10,
        .crop = {2, 0, 4, 4}, .rotation = XIAOTAI_VIDEO_ROTATE_90,
        .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .out_width = 8, .out_height = 4, .place = {2, 0, 4, 4}, .fill = true,
    };
    uint8_t input[80] = {0};
    _Alignas(ALIGN) uint8_t output[2 * ALIGN];
    memset(output, 0xde, sizeof(output));
    fake.real_input = input;
    fake.used_bytes = 8 * 4 * 2;
    void *state = NULL;
    assert(backend->open(&c, &state) == XIAOTAI_OK && s_fill.active);
    fake.fill_error = ESP_ERR_TIMEOUT;
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == ESP_ERR_TIMEOUT && !fake.srm);
    fake.fill_error = ESP_OK;
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.fills == 2 && !fake.syncs && fake.srm == 1 && fake.fill.out.fill_cm == PPA_FILL_COLOR_MODE_RGB565);
    assert(fake.op.in.pic_w == 10 && fake.op.in.pic_h == 4 && fake.op.in.block_offset_x == 2 && fake.op.byte_swap &&
           fake.op.rotation_angle == PPA_SRM_ROTATION_ANGLE_270);
    assert(output[0] == 0 && output[4] == 0x34 && output[5] == 0x12 && output[64] == 0xde);
    backend->close(state);
    assert(fake.releases == 2 && !fake.live);

    fake.fill_open_fail = true;
    c.out_pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
    fake.used_bytes = 8 * 4 * 3;
    assert(backend->open(&c, &state) == XIAOTAI_OK && !s_fill.active);
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.op.out.srm_cm == PPA_SRM_COLOR_MODE_RGB888 && fake.fills == 2 && fake.syncs == 1);
    backend->close(state);
    assert(!fake.live);

    c.out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED;
    c.fill = false;
    fake.used_bytes = 8 * 4 * 2;
    memset(output, 0xde, sizeof(output));
    assert(backend->open(&c, &state) == XIAOTAI_OK && !s_fill.active);
    assert(backend->run(state, input, sizeof(input), output, sizeof(output)) == XIAOTAI_OK);
    assert(output[0] == 0xde && output[4] == 0x12 && output[5] == 0x34 && output[12] == 0xde);
    backend->close(state);
    assert(!fake.live);
}

static void reuse_open_state(void)
{
    reset();
    const xiaotai_video_transform_config_t initial = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_I420, .in_width = 16, .in_height = 12, .in_stride = 16,
        .crop = {0, 0, 4, 4}, .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .out_width = 4, .out_height = 4,
        .place = {0, 0, 4, 4},
    };
    uint8_t planes[16U * 12U * 3U / 2U] = {0};
    uint8_t rgb_input[16U * 8U * 2U] = {0};
    _Alignas(ALIGN) uint8_t output[16 * ALIGN];
    void *state = NULL;
    assert(backend->reconfigure(NULL, &initial) == XIAOTAI_ERR_INVALID_ARG);
    assert(backend->open(&initial, &state) == XIAOTAI_OK);
    assert(fake.allocs == 2 && fake.packed_bytes == padded(sizeof(planes)) && fake.registrations == 1);
    assert(backend->reconfigure(state, NULL) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_video_transform_config_t larger = initial;
    larger.crop.width = larger.out_width = larger.place.width = 16;
    larger.crop.height = larger.out_height = larger.place.height = 12;
    assert(backend->reconfigure(state, &larger) == XIAOTAI_OK);
    fake.real_input = planes;
    fake.used_bytes = 16U * 12U * 2U;
    assert(backend->run(state, planes, sizeof(planes), output, sizeof(output)) == XIAOTAI_OK);
    const uint8_t *const packed = fake.op.in.buffer;
    assert(packed != planes && fake.op.in.pic_w == 16 && fake.op.in.pic_h == 12);

    const xiaotai_video_transform_config_t ragged = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .in_width = 14, .in_height = 8, .in_stride = 16,
        .crop = {2, 0, 12, 8}, .rotation = XIAOTAI_VIDEO_ROTATE_90,
        .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .out_width = 12, .out_height = 16,
        .place = {2, 2, 8, 12}, .fill = true,
    };
    assert(backend->reconfigure(state, &ragged) == XIAOTAI_OK);
    fake.real_input = rgb_input;
    fake.used_bytes = 12U * 16U * 2U;
    memset(output, 0xde, sizeof(output));
    assert(backend->run(state, rgb_input, sizeof(rgb_input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.op.in.buffer == rgb_input && fake.op.in.pic_w == 16 && fake.op.in.pic_h == 8 &&
           fake.op.in.block_offset_x == 2 && fake.op.byte_swap && fake.op.rotation_angle == PPA_SRM_ROTATION_ANGLE_270);
    assert(!fake.fills && fake.syncs == 1 && output[0] == 0 && output[(12 + 1) * 4] == 0x12);

    xiaotai_video_transform_config_t oversized = larger;
    oversized.in_stride = oversized.in_width = oversized.in_height = 20;
    oversized.crop.width = oversized.out_width = oversized.place.width = 20;
    oversized.crop.height = oversized.out_height = oversized.place.height = 20;
    assert(backend->match(&oversized) > 1);
    assert(backend->reconfigure(state, &oversized) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(backend->run(state, rgb_input, sizeof(rgb_input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.op.in.buffer == rgb_input && fake.op.out.block_offset_x == 2 && fake.op.out.block_offset_y == 2);

    assert(backend->reconfigure(state, &initial) == XIAOTAI_OK);
    fake.real_input = planes;
    fake.used_bytes = 4U * 4U * 2U;
    assert(backend->run(state, planes, sizeof(planes), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.op.in.buffer == packed && fake.op.in.pic_w == 4 && fake.op.in.pic_h == 4 && !fake.op.byte_swap);
    assert(fake.allocs == 2 && fake.registrations == 1 && fake.releases == 0 && fake.srm == 4);
    backend->close(state);
    assert(fake.releases == 1 && !fake.live);

    reset();
    xiaotai_video_transform_config_t boot_rgb = ragged;
    boot_rgb.fill = false;
    assert(backend->open(&boot_rgb, &state) == XIAOTAI_OK && !s_fill.active && fake.packed_bytes == 0);
    assert(backend->reconfigure(state, &initial) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(backend->reconfigure(state, &ragged) == XIAOTAI_OK);
    fake.real_input = rgb_input;
    fake.used_bytes = 12U * 16U * 2U;
    assert(backend->run(state, rgb_input, sizeof(rgb_input), output, sizeof(output)) == XIAOTAI_OK);
    assert(fake.allocs == 1 && fake.registrations == 1 && fake.syncs == 1 && !fake.fills);
    backend->close(state);
    assert(!fake.live && fake.releases == 1);
}

int main(void)
{
    geometry();
    paired();
    planar();
    rgb();
    reuse_open_state();
    return 0;
}
