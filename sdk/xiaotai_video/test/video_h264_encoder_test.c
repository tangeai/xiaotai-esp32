// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_h264_enc_single_hw.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/video_backend.h"

extern const xiaotai_video_encoder_t video_esp_h264_encoder;

static esp_h264_enc_t vendor_handle;
static esp_h264_enc_param_hw_t vendor_param;
static esp_h264_enc_cfg_hw_t vendor_config;
static _Alignas(128) uint8_t output[512];
static _Alignas(128) uint8_t input[1024U * 608U * 3U / 2U];
static bool external = true;
static int allocations, events;
static char ops[64];
static char failing_op;
static bool suppress_idr, missing_pps, empty_output, overlong_output;
static bool requested_idr;
static uint32_t pts;
static size_t raw_size;

static void step(char op)
{
    assert(events + 1 < (int)sizeof(ops));
    ops[events++] = op;
    ops[events] = '\0';
}

static esp_h264_err_t action(char op)
{
    step(op);
    return failing_op == op ? ESP_H264_ERR_TIMEOUT : ESP_H264_ERR_OK;
}

bool xiaotai_mem_has_external(void) { return external; }
bool xiaotai_mem_is_external(const void *p) { return external && (p == input || p == output); }
void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    assert(kind == XIAOTAI_MEM_INTERNAL);
    step('a');
    if (failing_op == 'a') return NULL;
    ++allocations;
    return calloc(count, size);
}

void xiaotai_mem_free(void *ptr)
{
    assert(ptr);
    --allocations;
    step('z');
    free(ptr);
}

esp_err_t esp_cache_msync(void *addr, size_t len, int flags)
{
    if (addr == input) {
        assert(flags == (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
        assert(len == raw_size);
        step('w');
        return failing_op == 'w' ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    }
    assert(addr == output && len == 128U);
    assert(flags == (ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
    step('i');
    return failing_op == 'i' ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

esp_h264_err_t esp_h264_enc_hw_new(const esp_h264_enc_cfg_hw_t *cfg, esp_h264_enc_handle_t *handle)
{
    vendor_config = *cfg;
    const esp_h264_err_t err = action('n');
    *handle = err == ESP_H264_ERR_OK ? &vendor_handle : NULL;
    return err;
}

esp_h264_err_t esp_h264_enc_hw_get_param_hd(esp_h264_enc_handle_t handle, esp_h264_enc_param_hw_handle_t *out)
{
    assert(handle == &vendor_handle);
    const esp_h264_err_t err = action('g');
    *out = err == ESP_H264_ERR_OK ? &vendor_param : NULL;
    return err;
}

esp_h264_err_t esp_h264_enc_open(esp_h264_enc_handle_t handle)
{
    assert(handle == &vendor_handle);
    return action('o');
}

esp_h264_err_t esp_h264_enc_close(esp_h264_enc_handle_t handle)
{
    assert(handle == &vendor_handle);
    return action('c');
}

esp_h264_err_t esp_h264_enc_del(esp_h264_enc_handle_t handle)
{
    assert(handle == &vendor_handle);
    return action('d');
}

esp_h264_err_t esp_h264_enc_set_bitrate(esp_h264_enc_param_handle_t param, uint32_t value)
{
    assert(param == &vendor_param.base && value == 900000);
    return action('b');
}

esp_h264_err_t esp_h264_enc_set_fps(esp_h264_enc_param_handle_t param, uint8_t value)
{
    assert(param == &vendor_param.base && (value == 12 || value == 15));
    return action('f');
}

esp_h264_err_t esp_h264_enc_set_gop(esp_h264_enc_param_handle_t param, uint8_t value)
{
    assert(param == &vendor_param.base && value == 36);
    return action('p');
}

esp_h264_err_t esp_h264_enc_force_idr(esp_h264_enc_param_handle_t param)
{
    assert(param == &vendor_param.base);
    const esp_h264_err_t err = action('k');
    requested_idr = err == ESP_H264_ERR_OK;
    return err;
}

esp_h264_err_t esp_h264_enc_process(esp_h264_enc_handle_t handle, esp_h264_enc_in_frame_t *in,
                                   esp_h264_enc_out_frame_t *out)
{
    assert(handle == &vendor_handle && in->pts == pts && in->raw_data.buffer == input && in->raw_data.len == raw_size);
    assert(out->raw_data.buffer == output && out->raw_data.len == sizeof(output));
    const esp_h264_err_t err = action('e');
    if (err != ESP_H264_ERR_OK) return err;
    const uint8_t key[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0, 0, 1, 0x68, 0x11, 0, 0, 1, 0x65, 0x55};
    const uint8_t delta[] = {0, 0, 1, 0x41, 0x22};
    const uint8_t no_pps[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0, 0, 1, 0x65, 0x55};
    if (overlong_output) out->length = out->raw_data.len + 1;
    else if (empty_output) out->length = 0;
    else if (requested_idr && !suppress_idr) {
        memcpy(output, missing_pps ? no_pps : key, missing_pps ? sizeof(no_pps) : sizeof(key));
        out->length = missing_pps ? sizeof(no_pps) : sizeof(key);
    } else {
        memcpy(output, delta, sizeof(delta));
        out->length = sizeof(delta);
    }
    requested_idr = false;
    return ESP_H264_ERR_OK;
}

int main(void)
{
    const xiaotai_video_encoder_t *v = &video_esp_h264_encoder;
    xiaotai_video_encoder_config_t c = {.codec = XIAOTAI_VIDEO_CODEC_H264, .pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED,
        .width = 720, .height = 960, .rows = 960, .fps = 15, .bitrate_bps = 1000000, .min_qp = 10, .max_qp = 40,
        .key_interval_frames = 30, .output = output, .output_capacity = sizeof(output)};
    assert(v->load == XIAOTAI_VIDEO_LOAD_HARDWARE && v->output_alignment == 128 && v->match(&c));
    assert(v->internal_bytes(&c) == 5888U);
    xiaotai_video_encoder_config_t padded = c;
    padded.width = 1024;
    padded.height = 600;
    padded.rows = 608;
    padded.full_range = true;
    assert(v->match(&padded) && v->internal_bytes(&padded) == 8320U && !allocations);
    xiaotai_video_encoder_config_t bad = padded;
    bad.rows = 600;
    assert(!v->match(&bad));
    external = false;
    assert(!v->match(&padded));
    external = true;
    const char *failures[] = {"a", "anz", "angdz", "angodz"};
    const char stages[] = "ango";
    void *state = NULL;
    for (size_t k = 0; k < sizeof(stages) - 1; ++k) {
        failing_op = stages[k];
        events = 0;
        state = &vendor_handle;
        assert(v->open(&c, &state) != XIAOTAI_OK && !state && !strcmp(ops, failures[k]) && !allocations);
    }
    failing_op = 0;
    events = 0;
    assert(v->open(&c, &state) == XIAOTAI_OK && !vendor_config.vui_color.present);
    assert(vendor_config.pic_type == ESP_H264_RAW_FMT_O_UYY_E_VYY && vendor_config.gop == 30 && vendor_config.fps == 15);
    assert(!strcmp(ops, "ango"));
    v->close(state);
    assert(!strcmp(ops, "angocdz") && !allocations);

    events = 0;
    assert(v->open(&padded, &state) == XIAOTAI_OK);
    assert(vendor_config.res.height == 600 && vendor_config.res.width == 1024 &&
           vendor_config.vui_color.present && vendor_config.vui_color.full_range &&
           vendor_config.vui_color.color_description_present && vendor_config.vui_color.matrix_coefficients == 6 &&
           vendor_config.vui_color.color_primaries == 2 && vendor_config.vui_color.transfer_characteristics == 2);
    xiaotai_video_image_t image = {.data = input, .size = sizeof(input), .pixel = padded.pixel,
        .width = 1024, .height = 600, .rows = 608, .stride = 1024, .full_range = true, .captured_us = 1234567};
    raw_size = sizeof(input);
    pts = 1234;
    xiaotai_video_packet_t packet;
    events = 0;
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_OK && !strcmp(ops, "wkei"));
    assert(packet.key_frame && packet.timestamp_ms == pts && packet.width == 1024 && packet.height == 600);
    assert(packet.data == output && packet.size == 17 && allocations == 1);
    events = 0;
    assert(v->encode(state, &image, false, &packet) == XIAOTAI_OK && !strcmp(ops, "wei") && !packet.key_frame);

    events = 0;
    image.size--;
    assert(v->encode(state, &image, false, &packet) == XIAOTAI_ERR_INVALID_SIZE && !events && !packet.data);
    ++image.size;
    image.stride++;
    assert(v->encode(state, &image, false, &packet) == XIAOTAI_ERR_INVALID_SIZE && !events);
    --image.stride;
    failing_op = 'w';
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_ERR_TIMEOUT && !strcmp(ops, "w") && !packet.data);
    failing_op = 'k';
    events = 0;
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_ERR_TIMEOUT && !strcmp(ops, "wk"));
    failing_op = 'i';
    events = 0;
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_ERR_TIMEOUT && !strcmp(ops, "wkei"));

    events = 0;
    failing_op = 0;
    overlong_output = true;
    assert(v->encode(state, &image, false, &packet) == XIAOTAI_ERR_INVALID_SIZE && !strcmp(ops, "we"));
    overlong_output = false;
    empty_output = true;
    events = 0;
    assert(v->encode(state, &image, false, &packet) == XIAOTAI_ERR_INVALID_SIZE && !strcmp(ops, "we"));
    empty_output = false;
    suppress_idr = true;
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_FAIL && !packet.data);
    suppress_idr = false;
    missing_pps = true;
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_FAIL && !packet.data);

    events = 0;
    bad = padded;
    bad.full_range = false;
    assert(v->reconfigure(state, &bad) == XIAOTAI_ERR_NOT_SUPPORTED && !events);
    padded.fps = 12;
    padded.bitrate_bps = 900000;
    padded.key_interval_frames = 36;
    assert(v->reconfigure(state, &padded) == XIAOTAI_OK && !strcmp(ops, "bfp"));
    events = 0;
    assert(v->reconfigure(state, &padded) == XIAOTAI_OK && !events);
    bad = padded;
    bad.fps = 15;
    failing_op = 'f';
    assert(v->reconfigure(state, &bad) == XIAOTAI_ERR_TIMEOUT && !strcmp(ops, "f"));
    assert(v->encode(state, &image, true, &packet) == XIAOTAI_ERR_INVALID_STATE && !packet.data);
    v->close(state);
    assert(!allocations);
}
