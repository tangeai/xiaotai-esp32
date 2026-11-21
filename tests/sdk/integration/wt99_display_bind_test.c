// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_wt_display_cleanup.py
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "esp_lcd_mipi_dsi.h"
#include "freertos/semphr.h"
#include "lvgl/ui_lvgl_private.h"
#include "xiaotai/parts/display.h"
#include "wt99_display_bind_test.h"

static wt99_bind_mode_t mode;
static unsigned injected, attaches, detaches;
static bool registered;
static bool already_open;

static xiaotai_err_t duplicate_bind(void *ctx, const xiaotai_display_handles_t *handles, bool *released)
{
    (void)ctx;
    (void)handles;
    (void)released;
    assert(false && "recursive bind ran before committing the display");
    return XIAOTAI_ERR_INVALID_STATE;
}

static void check_opening(void)
{
    const xiaotai_display_t *part = xiaotai_hal_display(0);
    xiaotai_display_part_stats_t stats;
    xiaotai_display_part_stats(part, &stats);
    assert(stats.open == already_open && stats.opens == (unsigned)already_open);
    xiaotai_display_handles_t duplicate = {.panel = (void *)part, .touch = (void *)part};
    assert(part->open(part->self, &duplicate) == XIAOTAI_ERR_BUSY);
    assert(!duplicate.panel && !duplicate.touch && !duplicate.framebuffers[0] && !duplicate.framebuffers[1]);
    assert(part->open_bound(part->self, duplicate_bind, NULL) == XIAOTAI_ERR_BUSY);
    if (!already_open) {
        size_t count = 1;
        assert(part->read_touch(part->self, NULL, 0, &count) == XIAOTAI_ERR_INVALID_STATE && count == 0);
        assert(part->set_backlight(part->self, 50) == XIAOTAI_ERR_INVALID_STATE);
    }
}

static void *other_open(void *unused)
{
    (void)unused;
    check_opening();
    return NULL;
}

lv_display_t *__real_lv_display_create(int32_t width, int32_t height);
lv_display_t *__wrap_lv_display_create(int32_t width, int32_t height)
{
    if (width != 1024 || height != 600) return __real_lv_display_create(width, height);
    check_opening();
    if (mode == CREATE_FAIL && !already_open) {
        pthread_t contender;
        assert(!pthread_create(&contender, NULL, other_open, NULL));
        assert(!pthread_join(contender, NULL));
        fprintf(stderr, "WT bind: same-task and other-task Open/Bound rejected; stats uncommitted\n");
    }
    if (mode != CREATE_FAIL) return __real_lv_display_create(width, height);
    ++injected;
    return NULL;
}

lv_indev_t *__real_lv_indev_create(void);
lv_indev_t *__wrap_lv_indev_create(void)
{
    if (mode != POINTER_FAIL) return __real_lv_indev_create();
    assert(registered && attaches == 1 && !detaches);
    check_opening();
    ++injected;
    return NULL;
}

int esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
    const esp_lcd_dpi_panel_event_callbacks_t *callbacks, void *ctx)
{
    assert(panel && callbacks && mode != CREATE_FAIL);
    if (callbacks->on_refresh_done) {
        assert(ctx && !registered);
        ++attaches;
        registered = true;
    } else {
        assert(!ctx && registered);
        ++detaches;
        if (mode == DETACH_FAIL) return XIAOTAI_FAIL;
        registered = false;
    }
    return XIAOTAI_OK;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    assert(storage && mode != CREATE_FAIL);
    return storage;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{
    (void)sem;
    (void)woken;
    assert(false && "unexpected scanout callback");
    return pdFALSE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem && mode != CREATE_FAIL);
    return ticks && (mode == HEALTHY || mode == POINTER_FAIL) ? pdTRUE : pdFALSE;
}

static xiaotai_err_t theme_start(const xiaotai_ui_host_t *host, void **out)
{
    assert(mode == HEALTHY && host->root);
    *out = NULL;
    return XIAOTAI_OK;
}

void wt99_ui_bind_probe(wt99_bind_mode_t failure_mode, bool initially_open)
{
    static const xiaotai_theme_t theme = {
        .name = "wt99-ui-bind-probe", .backend = XIAOTAI_UI_BACKEND_LVGL, .start = theme_start,
    };
    mode = failure_mode;
    already_open = initially_open;
    assert(xiaotai_engine_ui_init() == XIAOTAI_OK);
    ui_display_t *d = ui_display_at(0);
    assert(d && d->part == xiaotai_hal_display(0));
    d->theme = &theme;
    const xiaotai_err_t result = ui_display_open(d);
    fprintf(stderr, "WT bind mode=%u ret=%d stage=%s injected=%u attach=%u detach=%u cleanup_error=%d\n",
            mode, result, d->driver.start_stage, injected, attaches, detaches, d->native_cleanup_error);
    if (mode == HEALTHY) {
        assert(result == XIAOTAI_OK && d->host.root && attaches == 1 && !detaches);
        ui_display_stop(d);
        assert(!registered && detaches == 1 && !d->lvgl);
    } else {
        const bool oom = mode == CREATE_FAIL || mode == POINTER_FAIL;
        assert(result == (oom ? XIAOTAI_ERR_NO_MEM : XIAOTAI_ERR_TIMEOUT));
        assert(!strcmp(d->driver.start_stage, "display.backend_failed") && !d->handles.panel);
        assert(!d->lvgl && !d->drawing && !d->host.root && !lv_is_initialized());
        assert(d->native_cleanup_error == (mode == DETACH_FAIL ? XIAOTAI_FAIL : XIAOTAI_OK));
        assert(registered == (mode == DETACH_FAIL));
        assert(injected == oom && attaches == (mode != CREATE_FAIL) && detaches == attaches);
    }
    xiaotai_ui_reset();
}
