// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/port/mem.h"

#include <stdatomic.h>
#include <string.h>

struct ui_diag_request {
    atomic_uint refs;
    atomic_bool cancelled;
    xiaotai_signal_t done;
    bool running;                  /* protected by the engine mutex */
    xiaotai_err_t result;
    size_t capacity, count;
#if XIAOTAI_UI_CAPTURE
    xiaotai_ui_capture_t capture;
    char view[32];
#endif
    xiaotai_ui_control_t *rows;
};

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

void xiaotai_ui_capture_free(xiaotai_ui_capture_t *capture)
{
    if (!capture) return;
    xiaotai_mem_free(capture->pixels);
    *capture = (xiaotai_ui_capture_t){0};
}

#if XIAOTAI_UI_CAPTURE
static atomic_bool activity;

static void activity_changed(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    atomic_store(&activity, xiaotai_activity_active());
}

bool ui_diag_activity(void)
{
    /* Reconcile starts still queued and ends whose event could not be queued. */
    const bool live = xiaotai_activity_active();
    if (atomic_load(&activity) != live) atomic_store(&activity, live);
    return live;
}
#endif

#if !XIAOTAI_UI_CAPTURE
bool ui_diag_activity(void) { return false; }
#endif

void ui_count_flush(ui_display_t *d, uint32_t pixels, size_t bytes, uint64_t wait_us)
{
    lock();
    ++d->flushes;
    if (wait_us) ++d->flush_waits;
    d->flush_pixels += pixels;
    d->flush_bytes += bytes;
    d->flush_us += wait_us;
    unlock();
}

static void span(ui_trace_t *t, ui_span_kind_t kind, int64_t us)
{
    ui_span_t *s = &t->span[kind];
    if (us < 0) us = 0;
    if (us > UINT32_MAX) us = UINT32_MAX;
    ++s->count;
    s->total_us += (uint64_t)us;
    if (us > s->max_us) s->max_us = (uint32_t)us;
}

void ui_trace_span(ui_display_t *d, ui_span_kind_t kind, int64_t us)
{
    lock();
    if (d->touch) span(&d->touch->trace, kind, us);
    unlock();
}

void ui_trace_poll(ui_display_t *d, int64_t began, int64_t ended)
{
    lock();
    if (d->touch) {
        ui_trace_t *t = &d->touch->trace;
        span(t, UI_SPAN_READ, ended - began);
        if (t->polled) span(t, UI_SPAN_POLL, began - t->polled);
        t->polled = began;
    }
    unlock();
}

void ui_trace_contact(ui_display_t *d, bool down, int64_t at)
{
    lock();
    if (d->touch) {
        ui_trace_t *t = &d->touch->trace;
        t->pressed = down ? at : 0;
        t->released = down ? 0 : at;
        t->posted = t->shown = 0;
    }
    unlock();
}

void ui_trace_posted(ui_display_t *d)
{
    lock();
    ui_trace_t *t = d->touch ? &d->touch->trace : NULL;
    if (t && t->released) {
        const int64_t now = xiaotai_time_us();
        span(t, UI_SPAN_ACTION, now - t->released);
        t->released = 0;
        t->posted = now;
    }
    unlock();
}

void ui_trace_shown(ui_display_t *d)
{
    lock();
    ui_trace_t *t = d->touch ? &d->touch->trace : NULL;
    if (t && t->posted) {
        const int64_t now = xiaotai_time_us();
        span(t, UI_SPAN_APP, now - t->posted);
        t->posted = 0;
        t->shown = now;
    }
    unlock();
}

/* A step painted; `scene`: with what the application last showed. */
void ui_trace_painted(ui_display_t *d, bool scene)
{
    lock();
    ui_trace_t *t = d->touch ? &d->touch->trace : NULL;
    if (t) {
        const int64_t now = xiaotai_time_us();
        if (t->pressed) span(t, UI_SPAN_PRESS, now - t->pressed);
        t->pressed = 0;
        if (t->shown && scene) {
            span(t, UI_SPAN_PAGE, now - t->shown);
            t->shown = 0;
        }
    }
    unlock();
}

xiaotai_err_t ui_diag_init(void)
{
#if XIAOTAI_UI_CAPTURE
    atomic_store(&activity, xiaotai_activity_active());
    return XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, activity_changed, NULL);
#else
    return XIAOTAI_OK;
#endif
}

static void release(ui_diag_request_t *r)
{
    if (!r || atomic_fetch_sub(&r->refs, 1U) != 1U) return;
#if XIAOTAI_UI_CAPTURE
    xiaotai_ui_capture_free(&r->capture);
#endif
    xiaotai_mem_free(r->rows);
    xiaotai_signal_deinit(&r->done);
    xiaotai_mem_free(r);
}

static void finish(ui_diag_request_t *r, xiaotai_err_t err)
{
    if (!r) return;
    r->result = err;
    xiaotai_signal_give(&r->done);
    release(r);
}

static xiaotai_err_t make_request(size_t capacity, ui_diag_request_t **request)
{
    if (capacity > SIZE_MAX / sizeof(xiaotai_ui_control_t)) return XIAOTAI_ERR_INVALID_SIZE;
    /* The request owns a semaphore; copy buffers and frame pixels use BULK. */
    ui_diag_request_t *r = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*r));
    if (!r) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xiaotai_signal_init(&r->done);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(r);
        return err;
    }
    atomic_init(&r->refs, 1U);
    atomic_init(&r->cancelled, false);
    if (capacity && !(r->rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, capacity, sizeof(*r->rows)))) {
        release(r);
        return XIAOTAI_ERR_NO_MEM;
    }
    r->capacity = capacity;
    *request = r;
    return XIAOTAI_OK;
}

static bool abandoned(const ui_diag_request_t *r)
{
    return r && !r->running && atomic_load(&r->cancelled);
}

static xiaotai_err_t queue(ui_display_t *d, ui_diag_request_t *r, bool capture)
{
    lock();
    if (d->state != UI_DISPLAY_READY || d->driver.stop) {
        unlock();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    ui_diag_request_t *old = NULL;
    ui_diag_request_t **slot = &d->controls_request;
#if XIAOTAI_UI_CAPTURE
    if (capture) {
        ui_display_t *previous = g_ui.capture_display;
        if (previous && abandoned(previous->capture_request)) {
            old = previous->capture_request;
            previous->capture_request = NULL;
            g_ui.capture_display = NULL;
        }
        if (g_ui.capture_display) {
            unlock();
            return XIAOTAI_ERR_INVALID_STATE;
        }
        slot = &d->capture_request;
    }
#else
    (void)capture;
#endif
    if (abandoned(*slot)) {
        old = *slot;
        *slot = NULL;
    }
    if (*slot) {
        unlock();
        return capture ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_TIMEOUT;
    }
    atomic_fetch_add(&r->refs, 1U);
    *slot = r;
#if XIAOTAI_UI_CAPTURE
    if (capture) g_ui.capture_display = d;
#endif
    const xiaotai_err_t err = ui_driver_post_locked(d);
    if (err != XIAOTAI_OK) {
        *slot = NULL;
#if XIAOTAI_UI_CAPTURE
        if (capture) g_ui.capture_display = NULL;
#endif
    }
    unlock();
    release(old);
    if (err != XIAOTAI_OK) release(r);
    return err;
}

static xiaotai_err_t wait(ui_display_t *d, ui_diag_request_t *r, bool capture, uint32_t timeout_ms)
{
    xiaotai_err_t err = queue(d, r, capture);
    if (err == XIAOTAI_OK) err = xiaotai_signal_take(&r->done, timeout_ms) ? r->result : XIAOTAI_ERR_TIMEOUT;
    if (err != XIAOTAI_OK) atomic_store(&r->cancelled, true);
    return err;
}

static void string(char *out, size_t capacity, const char *in)
{
    const size_t length = in ? ui_store_utf8_prefix(in, strlen(in), capacity - 1U) : 0;
    if (length) memcpy(out, in, length);
    out[length] = '\0';
}

static void control_values(ui_display_t *d, xiaotai_ui_control_t *row)
{
    if (row->element == XIAOTAI_UI_ELEMENT_OTHER) {
        row->text[sizeof(row->text) - 1U] = '\0';
        row->intent[0] = '\0';
        return;
    }
    const xiaotai_ui_layer_t *layer = ui_scene_layer(d, (xiaotai_view_layer_t)row->layer);
    const xiaotai_view_t *view = layer && layer->present ? &layer->view : NULL;
    const char *label = NULL, *intent = NULL, *icon = NULL;
    uint8_t state = XIAOTAI_STATE_DISABLED;
    const size_t i = row->index;
    if (view && row->element == XIAOTAI_UI_ELEMENT_ITEM && i < view->item_count) {
        const xiaotai_view_item_t *item = &view->items[i];
        label = item->label;
        icon = item->icon;
        intent = item->intent;
        state = item->state;
    } else if (view && row->element == XIAOTAI_UI_ELEMENT_ACTION && i < view->action_count) {
        const xiaotai_view_action_t *action = &view->actions[i];
        label = action->label;
        icon = action->icon;
        intent = action->intent;
        state = action->state;
    } else if (view && row->element == XIAOTAI_UI_ELEMENT_FIELD && i < view->field_count) {
        const xiaotai_view_field_t *field = &view->fields[i];
        label = field->label;
        intent = field->intent;
        state = field->state;
    } else if (view && row->element == XIAOTAI_UI_ELEMENT_INDICATOR && i < view->indicator_count) {
        const xiaotai_view_indicator_t *indicator = &view->indicators[i];
        icon = indicator->icon;
        intent = indicator->intent;
        state = 0;
    }
    /* A logical field can have controls with different availability. */
    row->state |= state;
    string(row->text, sizeof(row->text), label && *label ? label : icon);
    string(row->intent, sizeof(row->intent), intent);
}

static xiaotai_err_t collect_controls(ui_display_t *d, ui_diag_request_t *r)
{
    if (!d->theme->controls) return XIAOTAI_ERR_NOT_SUPPORTED;
    r->count = d->theme->controls(d->theme_state, r->capacity ? r->rows : NULL, r->capacity);
    const size_t copied = r->count < r->capacity ? r->count : r->capacity;
    for (size_t i = 0; i < copied; ++i) control_values(d, &r->rows[i]);
    return XIAOTAI_OK;
}

static void service(ui_display_t *d, ui_diag_request_t **slot, bool capture)
{
    lock();
    ui_diag_request_t *r = *slot;
    if (r) r->running = true;
    unlock();
    if (!r) return;

    xiaotai_err_t err = XIAOTAI_ERR_TIMEOUT;
    if (!atomic_load(&r->cancelled)) {
#if XIAOTAI_UI_CAPTURE
        if (capture) {
            if (ui_diag_activity()) err = XIAOTAI_ERR_BUSY;
            else {
                ui_view_name(d->scene.page.present ? &d->scene.page.view : NULL, r->view, sizeof(r->view));
                if (d->theme->backend == XIAOTAI_UI_BACKEND_FRAME) {
                    err = ui_frame_capture(d, &r->capture);
                } else {
#if XIAOTAI_UI_LVGL
                    err = ui_lvgl_capture(d, &r->capture);
#else
                    err = XIAOTAI_ERR_NOT_SUPPORTED;
#endif
                }
            }
        } else
#else
        (void)capture;
#endif
        {
            err = collect_controls(d, r);
        }
    }
    lock();
    *slot = NULL;
#if XIAOTAI_UI_CAPTURE
    if (capture) g_ui.capture_display = NULL;
#endif
    unlock();
    finish(r, err);
}

void ui_diag_step(ui_display_t *d)
{
    service(d, &d->controls_request, false);
#if XIAOTAI_UI_CAPTURE
    service(d, &d->capture_request, true);
#endif
}

bool ui_diag_pending(ui_display_t *d)
{
    lock();
    bool pending = d->controls_request && !abandoned(d->controls_request);
#if XIAOTAI_UI_CAPTURE
    pending |= d->capture_request && !abandoned(d->capture_request);
#endif
    unlock();
    return pending;
}

void ui_diag_shutdown(ui_display_t *d)
{
    lock();
    ui_diag_request_t *controls = d->controls_request;
    d->controls_request = NULL;
#if XIAOTAI_UI_CAPTURE
    ui_diag_request_t *capture = d->capture_request;
    d->capture_request = NULL;
    if (g_ui.capture_display == d) g_ui.capture_display = NULL;
#endif
    unlock();
    finish(controls, XIAOTAI_ERR_INVALID_STATE);
#if XIAOTAI_UI_CAPTURE
    finish(capture, XIAOTAI_ERR_INVALID_STATE);
#endif
}

xiaotai_err_t xiaotai_ui_controls(uint8_t display, xiaotai_ui_control_t *out, size_t capacity,
                                  size_t *count, uint32_t timeout_ms)
{
    if (!count) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    if (capacity && !out) return XIAOTAI_ERR_INVALID_ARG;
    ui_display_t *d = ui_display_at(display);
    const xiaotai_err_t guard = ui_driver_wait_guard(d);
    if (guard != XIAOTAI_OK) return guard;
    ui_diag_request_t *r;
    xiaotai_err_t err = make_request(capacity, &r);
    if (err != XIAOTAI_OK) return err;
    err = wait(d, r, false, timeout_ms);
    if (err == XIAOTAI_OK) {
        *count = r->count;
        const size_t copied = r->count < capacity ? r->count : capacity;
        if (copied) memcpy(out, r->rows, copied * sizeof(*out));
    }
    release(r);
    return err;
}

xiaotai_err_t ui_capture_named(uint8_t display, xiaotai_ui_capture_t *out, char view[32], uint32_t timeout_ms)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_ui_capture_t){0};
    if (view) view[0] = '\0';
#if XIAOTAI_UI_CAPTURE
    ui_display_t *d = ui_display_at(display);
    const xiaotai_err_t guard = ui_driver_wait_guard(d);
    if (guard != XIAOTAI_OK) return guard;
    if (ui_diag_activity()) return XIAOTAI_ERR_BUSY;
    ui_diag_request_t *r;
    xiaotai_err_t err = make_request(0, &r);
    if (err != XIAOTAI_OK) return err;
    err = wait(d, r, true, timeout_ms);
    if (err == XIAOTAI_OK) {
        *out = r->capture;
        if (view) memcpy(view, r->view, sizeof(r->view));
        r->capture = (xiaotai_ui_capture_t){0};
    }
    release(r);
    return err;
#else
    (void)display;
    (void)timeout_ms;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

xiaotai_err_t xiaotai_ui_capture(uint8_t display, xiaotai_ui_capture_t *out, uint32_t timeout_ms)
{
    return ui_capture_named(display, out, NULL, timeout_ms);
}
