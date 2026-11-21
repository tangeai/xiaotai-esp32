// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/port/mem.h"

#include <string.h>

#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
xiaotai_err_t xiaotai_ui_owner_frame_offer(xiaotai_ui_owner_frame_fn provide)
{
    if (!provide) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !g_ui.initialized || g_ui.started || g_ui.owner_frame_provider)
        return XIAOTAI_ERR_INVALID_STATE;
    if (g_ui.display_count != 1U || xiaotai_mem_has_external()) return XIAOTAI_ERR_NOT_SUPPORTED;
    g_ui.owner_frame_provider = provide;
    return XIAOTAI_OK;
}

void xiaotai_ui_owner_frame_forget(xiaotai_ui_owner_frame_fn provide)
{
    if (g_ui.owner_frame_provider == provide) g_ui.owner_frame_provider = NULL;
}

static ui_display_t *owner_frame_display(const xiaotai_ui_host_t *host)
{
    if (!host || !xiaotai_on_owner() || g_ui.display_count != 1U) return NULL;
    ui_display_t *display = ui_display_at(host->index);
    if (!display || host != &display->host || !display->driver.driven ||
        !display->driver.owner_timer || !display->theme ||
        display->theme->backend != XIAOTAI_UI_BACKEND_FRAME) return NULL;
    return display;
}

bool xiaotai_ui_host_prepare_owner_frame(const xiaotai_ui_host_t *host, size_t bytes)
{
    ui_display_t *d = owner_frame_display(host);
    if (!d || d->state == UI_DISPLAY_READY || !bytes || bytes != d->frame.bytes ||
        d->part->desc.render != XIAOTAI_RENDER_FULL ||
        (d->part->desc.flags & XIAOTAI_DISPLAY_DMA_BUFFER) ||
        !g_ui.owner_frame_provider) return false;
    const uint8_t *buffer = g_ui.owner_frame_provider(bytes);
    const uint32_t alignment = d->part->desc.buffer_align;
    if (!buffer || (alignment && (uintptr_t)buffer % alignment)) return false;
    d->frame.owner_workspace_bytes = bytes;
    return true;
}

uint8_t *xiaotai_ui_host_owner_frame(const xiaotai_ui_host_t *host)
{
    ui_display_t *d = owner_frame_display(host);
    return d ? d->frame.owner_workspace : NULL;
}

bool ui_render_scene(ui_display_t *d)
{
    const size_t bytes = d->frame.owner_workspace_bytes;
    if (bytes) {
        if (d->frame.owner_workspace || !owner_frame_display(&d->host) || !g_ui.owner_frame_provider)
            return false;
        d->frame.owner_workspace = g_ui.owner_frame_provider(bytes);
        if (!d->frame.owner_workspace) return false;
    }
    d->theme->render(d->theme_state, &d->scene);
    d->frame.owner_workspace = NULL;
    return true;
}
#endif

static uint64_t bytes_for(xiaotai_pixel_t pixel, uint16_t width, uint16_t height)
{
    switch (pixel) {
        case XIAOTAI_PIXEL_MONO1: return ((width + 7U) / 8U) * (uint64_t)height;
        case XIAOTAI_PIXEL_MONO1_VLSB: return ((height + 7U) / 8U) * (uint64_t)width;
        case XIAOTAI_PIXEL_RGB565: return 2U * (uint64_t)width * height;
        case XIAOTAI_PIXEL_RGB888: return 3U * (uint64_t)width * height;
        default: return 0;
    }
}

xiaotai_err_t ui_frame_open(ui_display_t *display)
{
    const xiaotai_display_desc_t *desc = &display->part->desc;
    if (!desc->width || !desc->height) return XIAOTAI_ERR_INVALID_SIZE;
    if (desc->rotation > 3U) return XIAOTAI_ERR_INVALID_ARG;
    const bool swapped = (desc->rotation & 1U) != 0U;
    const uint16_t w = swapped ? desc->height : desc->width;
    const uint16_t h = swapped ? desc->width : desc->height;
    const uint64_t bytes = bytes_for(desc->pixel, desc->width, desc->height);
    const uint64_t native = bytes_for(desc->pixel, w, h);
    if (!bytes) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (bytes > SIZE_MAX || native > SIZE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    display->frame.last = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)bytes);
    if (!display->frame.last) return XIAOTAI_ERR_NO_MEM;
    if (desc->rotation) {
        const xiaotai_mem_kind_t memory = (desc->flags & XIAOTAI_DISPLAY_DMA_BUFFER) &&
            desc->render == XIAOTAI_RENDER_PARTIAL ? XIAOTAI_MEM_DMA : XIAOTAI_MEM_BULK;
        display->frame.native = desc->buffer_align > _Alignof(void *)
            ? xiaotai_mem_aligned(memory, desc->buffer_align, (size_t)native)
            : xiaotai_mem_alloc(memory, (size_t)native);
        if (!display->frame.native) {
            ui_frame_close(display);
            return XIAOTAI_ERR_NO_MEM;
        }
        display->frame.native_bytes = (size_t)native;
    }
    display->frame.bytes = (size_t)bytes;
    display->frame.area = (xiaotai_rect_t){.width = w, .height = h};
    return XIAOTAI_OK;
}

void ui_frame_close(ui_display_t *display)
{
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    display->frame.owner_workspace_bytes = 0;
    display->frame.owner_workspace = NULL;
#endif
    xiaotai_mem_free(display->frame.native);
    display->frame.native = NULL;
    xiaotai_mem_free(display->frame.last);
    display->frame.last = NULL;
}

static size_t mono_offset(xiaotai_pixel_t pixel, size_t width, size_t x, size_t y, uint8_t *mask)
{
    if (pixel == XIAOTAI_PIXEL_MONO1_VLSB) {
        *mask = (uint8_t)(1U << (y % 8U));
        return (y / 8U) * width + x;
    }
    *mask = (uint8_t)(0x80U >> (x % 8U));
    return y * ((width + 7U) / 8U) + x / 8U;
}

static void rotate_frame(const xiaotai_display_desc_t *desc, ui_frame_t *state, const uint8_t *frame)
{
    const size_t width = desc->width, height = desc->height, native_width = state->area.width;
    const size_t pixel_bytes = desc->pixel == XIAOTAI_PIXEL_RGB565 ? 2U :
                               desc->pixel == XIAOTAI_PIXEL_RGB888 ? 3U : 0U;
    if (!pixel_bytes) memset(state->native, 0, state->native_bytes);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            size_t nx, ny;
            switch (desc->rotation) {
                case 1: nx = height - 1U - y; ny = x; break;
                case 2: nx = width - 1U - x; ny = height - 1U - y; break;
                default: nx = y; ny = width - 1U - x; break;
            }
            if (pixel_bytes) {
                memcpy(state->native + (ny * native_width + nx) * pixel_bytes,
                       frame + (y * width + x) * pixel_bytes, pixel_bytes);
            } else {
                uint8_t mask;
                const size_t source = mono_offset(desc->pixel, width, x, y, &mask);
                if (frame[source] & mask) {
                    const size_t target = mono_offset(desc->pixel, native_width, nx, ny, &mask);
                    state->native[target] |= mask;
                }
            }
        }
    }
}

void ui_frame_unlatch(ui_display_t *display)
{
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    display->frame.fault = false;
    xiaotai_mutex_unlock(&g_ui.lock);
    display->frame.valid = false;
}

xiaotai_err_t xiaotai_ui_host_present(const xiaotai_ui_host_t *host, const uint8_t *frame, size_t size)
{
    ui_display_t *display = host ? ui_display_at(host->index) : NULL;
    if (!display || host != &display->host || !frame) return XIAOTAI_ERR_INVALID_ARG;
    if (display->theme->backend != XIAOTAI_UI_BACKEND_FRAME) return XIAOTAI_ERR_NOT_SUPPORTED;
    ui_frame_t *state = &display->frame;
    if (size != state->bytes) return XIAOTAI_ERR_INVALID_SIZE;
    if (!state->last) return XIAOTAI_ERR_INVALID_STATE;
#if XIAOTAI_UI_CAPTURE
    if (state->capture) {
        memcpy(state->capture->pixels, frame, size);
        state->capture->size = size;
        return XIAOTAI_OK;
    }
#endif
    if (state->fault || (state->valid && !memcmp(frame, state->last, size))) return XIAOTAI_OK;
    if (state->native) rotate_frame(&display->part->desc, state, frame);
    const int64_t began = xiaotai_time_us();
    const xiaotai_err_t err = display->part->present(display->part->self, &state->area,
                                                    state->native ? state->native : frame);
    const int64_t elapsed = xiaotai_time_us() - began;
    ui_count_flush(display, (uint32_t)state->area.width * state->area.height,
                   state->native ? state->native_bytes : size, elapsed > 0 ? (uint64_t)elapsed : 0U);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
        state->fault = true;
        ++state->errors;
        xiaotai_mutex_unlock(&g_ui.lock);
        return err;
    }
    memcpy(state->last, frame, size);
    state->valid = true;
    return XIAOTAI_OK;
}

#if XIAOTAI_UI_CAPTURE
xiaotai_err_t ui_frame_capture(ui_display_t *d, xiaotai_ui_capture_t *capture)
{
    const xiaotai_display_desc_t *desc = &d->part->desc;
    if (desc->pixel != XIAOTAI_PIXEL_MONO1 && desc->pixel != XIAOTAI_PIXEL_MONO1_VLSB &&
        desc->pixel != XIAOTAI_PIXEL_RGB565) return XIAOTAI_ERR_NOT_SUPPORTED;
    uint8_t *pixels = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, d->frame.bytes);
    if (!pixels) return XIAOTAI_ERR_NO_MEM;
    *capture = (xiaotai_ui_capture_t){.width = desc->width, .height = desc->height,
                                      .pixel = desc->pixel, .pixels = pixels};

    /* Render into the capture, leaving the panel and last sent frame alone. */
    uint32_t changed[UI_LAYER_COUNT];
    const bool clock_changed = d->scene.clock_changed;
    for (size_t i = 0; i < UI_LAYER_COUNT; ++i) {
        xiaotai_ui_layer_t *layer = ui_scene_layer(d, (xiaotai_view_layer_t)i);
        changed[i] = layer->changed;
        if (layer->present) layer->changed |= UI_CHANGED_ALL & ~XIAOTAI_CHANGED_IDENTITY;
    }
    d->scene.clock_changed = true;
    d->frame.capture = capture;
    const bool rendered = ui_render_scene(d);
    d->frame.capture = NULL;
    d->scene.clock_changed = clock_changed;
    for (size_t i = 0; i < UI_LAYER_COUNT; ++i) {
        ui_scene_layer(d, (xiaotai_view_layer_t)i)->changed = changed[i];
    }
    if (!rendered || !capture->size) {
        xiaotai_ui_capture_free(capture);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (desc->pixel == XIAOTAI_PIXEL_RGB565 && (desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES)) {
        for (size_t i = 0; i < capture->size; i += 2U) {
            const uint8_t high = pixels[i];
            pixels[i] = pixels[i + 1U];
            pixels[i + 1U] = high;
        }
    }
    return XIAOTAI_OK;
}
#endif

void xiaotai_ui_host_invalidate(const xiaotai_ui_host_t *host)
{
    ui_display_t *display = host ? ui_display_at(host->index) : NULL;
    if (!display || host != &display->host) return;
#if XIAOTAI_UI_CAPTURE
    if (display->frame.capture) return;
#endif
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    display->power.redraw = true;
    xiaotai_mutex_unlock(&g_ui.lock);
}

xiaotai_err_t xiaotai_ui_refresh(uint8_t index)
{
    ui_display_t *display = ui_display_at(index);
    if (!display) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->state != UI_DISPLAY_READY) {
        xiaotai_mutex_unlock(&g_ui.lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    display->power.force_frame = true;
    display->power.redraw = true;
    xiaotai_mutex_unlock(&g_ui.lock);
    return ui_driver_kick(display);
}
