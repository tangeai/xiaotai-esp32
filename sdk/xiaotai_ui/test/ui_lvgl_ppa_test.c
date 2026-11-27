// SPDX-License-Identifier: MIT
/* ui_lvgl_ppa: the pixel accelerator's draw unit (esp-idf/ui_lvgl_ppa.c) with
 * a PPA made of plain C. A display drawn in place renders the same scene with
 * and without the accelerator, and the frames are equal pixel for pixel:
 * fills, a picture, the copy that brings the hidden buffer up to date, small
 * and shaped things that stay with the software renderer, jobs the driver
 * refuses, a picture the accelerator cannot read, the switch, and buffers it
 * must decline. Run with "inexact" the stand-in returns a wrong pixel in its
 * first copy: the self-check then keeps pictures and copies in software. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"
#include "ui_lvgl_ppa.h"

enum { W = 320, H = 240, ALIGN = 64, PICTURE_W = 120, PICTURE_H = 80 };

/* ------------------------------------------------------- the stand-in PPA */

static struct {
    unsigned moves, fills, syncs;
    bool inexact, refuse;
    const uint8_t *synced;      /* the rows last written back from the cache */
    size_t synced_bytes;
    const void *flash, *flash_end;   /* what counts as not being RAM */
} fake;

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *out)
{
    static int clients[3];
    assert(config->max_pending_trans_num == 1);
    *out = (ppa_client_handle_t)&clients[config->oper_type];
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

/* What the driver insists on for an output picture. */
static void check_out(const ppa_out_pic_blk_config_t *out, uint32_t width, uint32_t height)
{
    assert(out->buffer && !((uintptr_t)out->buffer % ALIGN) && !(out->buffer_size % ALIGN));
    assert((uint64_t)out->pic_w * out->pic_h * 2U <= out->buffer_size);
    assert(width && height && out->block_offset_x + width <= out->pic_w && out->block_offset_y + height <= out->pic_h);
}

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *job)
{
    assert(client && job->mode == PPA_TRANS_MODE_BLOCKING && job->scale_x == 1.0f && job->scale_y == 1.0f);
    assert(job->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 && job->out.srm_cm == PPA_SRM_COLOR_MODE_RGB565);
    assert(job->in.buffer && job->in.block_offset_x + job->in.block_w <= job->in.pic_w &&
           job->in.block_offset_y + job->in.block_h <= job->in.pic_h);
    check_out(&job->out, job->in.block_w, job->in.block_h);
    /* The driver drops the cache over the rows it writes: they must have been written back just before. */
    const uint8_t *rows = (const uint8_t *)job->out.buffer + (size_t)job->out.block_offset_y * job->out.pic_w * 2U;
    assert(fake.synced && fake.synced <= rows &&
           fake.synced + fake.synced_bytes >= rows + (size_t)job->in.block_h * job->out.pic_w * 2U);
    fake.synced = NULL;
    if (fake.refuse) return ESP_FAIL;
    for (uint32_t row = 0; row < job->in.block_h; ++row)
        memcpy((uint8_t *)job->out.buffer + ((size_t)(job->out.block_offset_y + row) * job->out.pic_w + job->out.block_offset_x) * 2U,
               (const uint8_t *)job->in.buffer + ((size_t)(job->in.block_offset_y + row) * job->in.pic_w + job->in.block_offset_x) * 2U,
               (size_t)job->in.block_w * 2U);
    if (fake.inexact) {
        uint8_t *first = (uint8_t *)job->out.buffer + ((size_t)job->out.block_offset_y * job->out.pic_w + job->out.block_offset_x) * 2U;
        first[0] ^= 1U;
    }
    ++fake.moves;
    return ESP_OK;
}

esp_err_t ppa_do_fill(ppa_client_handle_t client, const ppa_fill_oper_config_t *job)
{
    assert(client && job->mode == PPA_TRANS_MODE_BLOCKING && job->out.fill_cm == PPA_FILL_COLOR_MODE_RGB565);
    check_out(&job->out, job->fill_block_w, job->fill_block_h);
    if (fake.refuse) return ESP_FAIL;
    const color_pixel_argb8888_data_t c = job->fill_argb_color;
    const uint16_t colour = (uint16_t)((c.r >> 3) << 11 | (c.g >> 2) << 5 | c.b >> 3);
    for (uint32_t row = 0; row < job->fill_block_h; ++row)
        for (uint32_t x = 0; x < job->fill_block_w; ++x)
            memcpy((uint8_t *)job->out.buffer + ((size_t)(job->out.block_offset_y + row) * job->out.pic_w + job->out.block_offset_x + x) * 2U,
                   &colour, 2);
    ++fake.fills;
    return ESP_OK;
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    assert(addr && size && (flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) && !(flags & ESP_CACHE_MSYNC_FLAG_DIR_M2C));
    fake.synced = addr;
    fake.synced_bytes = size;
    ++fake.syncs;
    return ESP_OK;
}

size_t esp_cache_get_line_size_by_addr(const void *addr)
{
    assert(addr);
    return ALIGN;
}

bool esp_ptr_external_ram(const void *p) { return p && !(p >= fake.flash && p < fake.flash_end); }
bool esp_ptr_internal(const void *p) { (void)p; return false; }

/* ------------------------------------------------------------- the scene */

static lv_display_t *display;
static lv_draw_buf_t buffers[2];
static const uint8_t *shown;         /* the frame buffer the last refresh ended in */
static uint32_t clock_ms;
static lv_obj_t *picture, *panel;
static uint16_t picture_pixels[PICTURE_W * PICTURE_H];
static lv_image_dsc_t picture_dsc = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
    .w = PICTURE_W, .h = PICTURE_H, .stride = PICTURE_W * 2}, .data_size = sizeof(picture_pixels),
    .data = (const uint8_t *)picture_pixels};

static uint32_t tick(void) { return clock_ms += 5U; }

static void flush(lv_display_t *root, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    if (lv_display_flush_is_last(root)) shown = pixels;
    lv_display_flush_ready(root);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t colour, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static void build(void)
{
    for (unsigned i = 0; i < PICTURE_W * PICTURE_H; ++i) picture_pixels[i] = (uint16_t)(i * 40503U >> 3);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x16304A), 0);     /* a fill as large as the display */
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    panel = box(screen, 180, 120, 110, 100, 0xE04070, 0);             /* a fill large enough for a job */
    (void)box(screen, 200, 20, 80, 60, 0x40C0F0, 12);                  /* rounded: software */
    (void)box(screen, 10, 200, 24, 24, 0xFFD060, 0);                   /* too small for a job: software */
    lv_obj_t *veil = box(screen, 20, 20, 90, 60, 0x000000, 0);         /* translucent: software */
    lv_obj_set_style_bg_opa(veil, LV_OPA_50, 0);
    picture = lv_image_create(screen);
    lv_image_set_src(picture, &picture_dsc);
    lv_obj_set_pos(picture, 30, 40);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "PPA");
    lv_obj_set_pos(label, 150, 60);
}

/* Draws what changed and returns the frame as it is shown. */
static const uint8_t *render(void)
{
    shown = NULL;
    lv_refr_now(display);
    assert(shown == buffers[0].data || shown == buffers[1].data);
    return shown;
}

static uint8_t *snapshot(const uint8_t *frame)
{
    uint8_t *copy = malloc((size_t)W * H * 2U);
    assert(copy);
    memcpy(copy, frame, (size_t)W * H * 2U);
    return copy;
}

static void same(const uint8_t *frame, const uint8_t *expected, const char *what)
{
    for (unsigned i = 0; i < W * H; ++i)
        if (memcmp(frame + 2U * i, expected + 2U * i, 2) != 0) {
            fprintf(stderr, "%s: pixel (%u, %u) differs\n", what, i % W, i / W);
            abort();
        }
}

static unsigned field(const char *report, const char *name)
{
    const char *at = strstr(report, name);
    assert(at);
    return (unsigned)strtoul(at + strlen(name), NULL, 10);
}

int main(int argc, char **argv)
{
    const bool inexact = argc > 1 && !strcmp(argv[1], "inexact");
    char report[192];
    lv_init();
    lv_tick_set_cb(tick);
    for (int i = 0; i < 2; ++i) {
        void *memory = aligned_alloc(ALIGN, (size_t)W * H * 2U);
        assert(memory);
        memset(memory, 0, (size_t)W * H * 2U);
        assert(lv_draw_buf_init(&buffers[i], W, H, LV_COLOR_FORMAT_RGB565, W * 2U, memory, W * H * 2U) == LV_RESULT_OK);
    }
    display = lv_display_create(W, H);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_draw_buffers(display, &buffers[0], &buffers[1]);
    lv_display_set_flush_cb(display, flush);
    build();

    /* Nothing attached: nothing to report, nothing to switch. */
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(!report[0] && ui_lvgl_ppa_enable(false) == XIAOTAI_ERR_NOT_SUPPORTED);
    uint8_t *software = snapshot(render());

    /* Buffers the accelerator could not write are declined, and nothing changes for them. */
    lv_draw_buf_t odd = buffers[0];
    odd.data_size -= 2U;
    fake.inexact = inexact;
    assert(ui_lvgl_ppa_attach(&odd, 1) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(ui_lvgl_ppa_attach(NULL, 2) == XIAOTAI_ERR_INVALID_ARG);
    assert(ui_lvgl_ppa_attach(buffers, 2) == XIAOTAI_OK);
    /* Attached, it is idle: it runs no job, not even its self-check, and the frame is the software one. */
    assert(fake.moves == 0 && fake.fills == 0);
    lv_obj_invalidate(lv_display_get_screen_active(display));
    same(render(), software, "frame with the accelerator idle");
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(fake.moves == 0 && fake.fills == 0 && field(report, "on=") == 0);
    /* Turned on, it checks itself once. */
    assert(ui_lvgl_ppa_enable(true) == XIAOTAI_OK);
    fake.inexact = false;   /* only the self-check's copy was wrong */
    const unsigned checks = fake.moves;
    assert(checks == 1);
    assert(ui_lvgl_ppa_enable(true) == XIAOTAI_OK && fake.moves == checks);

    /* The whole scene again: the same frame, with the fills and the picture done as jobs. */
    lv_obj_invalidate(lv_display_get_screen_active(display));
    same(render(), software, "accelerated frame");
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(field(report, "on=") == 1 && field(report, "exact=") == (inexact ? 0U : 1U));
    assert(fake.fills == 2 && field(report, "fill=") == 2);          /* the background and the panel */
    assert(fake.moves - checks == (inexact ? 0U : 1U) && field(report, "image=") == (inexact ? 0U : 1U));
    assert(field(report, "refused=") == 0 && field(report, "software=") == 0);

    /* Two partial frames: the second brings the hidden buffer up to date with what the first changed. */
    lv_obj_set_pos(picture, 60, 90);
    (void)render();
    const unsigned before_copy = fake.moves;
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x30B060), 0);
    uint8_t *partial = snapshot(render());
    ui_lvgl_ppa_report(report, sizeof(report));
    if (!inexact) assert(fake.moves > before_copy && field(report, "copy=") >= 1);
    else assert(fake.moves == before_copy && field(report, "copy=") == 0);

    /* Switched off, the software renderer draws the same state from scratch: the frames agree. */
    assert(ui_lvgl_ppa_enable(false) == XIAOTAI_OK);
    const unsigned moves = fake.moves, fills = fake.fills;
    lv_obj_invalidate(lv_display_get_screen_active(display));
    uint8_t *expected = snapshot(render());
    same(partial, expected, "frame after partial updates");
    assert(fake.moves == moves && fake.fills == fills);
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(field(report, "on=") == 0);

    /* A driver that refuses every job: the software renderer draws it, and it is counted. */
    assert(ui_lvgl_ppa_enable(true) == XIAOTAI_OK);
    fake.refuse = true;
    lv_obj_invalidate(lv_display_get_screen_active(display));
    same(render(), expected, "frame with refused jobs");
    fake.refuse = false;
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(field(report, "refused=") >= 2 && field(report, "software=") >= 2 && fake.moves == moves && fake.fills == fills);

    /* A picture that is not in RAM is not handed over. The first of the two frames also brings the
     * hidden buffer up to date; the second redraws the picture and nothing else. */
    fake.flash = picture_pixels;
    fake.flash_end = picture_pixels + PICTURE_W * PICTURE_H;
    lv_obj_invalidate(picture);
    (void)render();
    const unsigned settled = fake.moves;
    lv_obj_invalidate(picture);
    same(render(), expected, "frame with a picture in flash");
    assert(fake.moves == settled);
    fake.flash = fake.flash_end = NULL;

    /* Detached, the buffers are LVGL's own again and nothing more reaches the accelerator. */
    ui_lvgl_ppa_detach(buffers, 2);
    assert(buffers[0].handlers == lv_draw_buf_get_handlers() && buffers[1].handlers == lv_draw_buf_get_handlers());
    const unsigned last_moves = fake.moves, last_fills = fake.fills;
    lv_obj_invalidate(lv_display_get_screen_active(display));
    same(render(), expected, "frame after detaching");
    assert(fake.fills == last_fills && fake.moves == last_moves);
    lv_display_delete(display);
    lv_deinit();
    ui_lvgl_ppa_reset();
    ui_lvgl_ppa_report(report, sizeof(report));
    assert(!report[0]);

    free(software);
    free(partial);
    free(expected);
    free(buffers[0].data);
    free(buffers[1].data);
    printf("PASS: ui_lvgl_ppa%s\n", inexact ? " (inexact)" : "");
    return 0;
}
