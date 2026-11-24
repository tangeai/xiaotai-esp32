// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/theme_mono128.h"
#include "xiaotai_mem_host.h"

#include <string.h>

enum { BYTES = XIAOTAI_THEME_MONO128_FRAME_BYTES };
static uint8_t panels[2][BYTES];
static unsigned writes[2];
static uint32_t now_ms;

static uint32_t clock_ms(void) { return now_ms; }

static xiaotai_err_t present(size_t index, const xiaotai_rect_t *area, const void *frame)
{
    if (area->x || area->y || area->width != 128 || area->height != 64 || !frame)
        return XIAOTAI_ERR_INVALID_ARG;
    memcpy(panels[index], frame, BYTES);
    ++writes[index];
    return XIAOTAI_OK;
}

static xiaotai_err_t page_major(void *self, const xiaotai_rect_t *area, const void *frame)
{
    (void)self;
    return present(0, area, frame);
}

static xiaotai_err_t row_major(void *self, const xiaotai_rect_t *area, const void *frame)
{
    (void)self;
    return present(1, area, frame);
}

static bool lit(const uint8_t *frame, unsigned x, unsigned y)
{
    return (frame[y / 8U * 128U + x] & (1U << (y % 8U))) != 0U;
}

static bool has_pixels(const uint8_t *frame, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            if (lit(frame, x, y)) return true;
    return false;
}

static void step(uint32_t next)
{
    now_ms = next;
    ui_driver_step(ui_display_at(0), next);
    ui_driver_step(ui_display_at(1), next);
}

static void show(xiaotai_view_t view)
{
    view.display = 0;
    ui_view_accept(&view);
    view.display = 1;
    ui_view_accept(&view);
    step(now_ms);
}

static int match_rule(void)
{
    const xiaotai_theme_t *theme = &xiaotai_theme_mono128;
    CHECK(!strcmp(theme->name, "mono128") && theme->backend == XIAOTAI_UI_BACKEND_FRAME);
    CHECK(theme->stack_bytes == XIAOTAI_UI_OWNER_THEME_STACK_BYTES);
    xiaotai_display_desc_t display = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    CHECK(theme->match(&display) == 100 && theme->match(NULL) == 0);
    display.pixel = XIAOTAI_PIXEL_MONO1;
    CHECK(theme->match(&display) == 100);
    display.flags = XIAOTAI_DISPLAY_SLOW_REFRESH;
    CHECK(theme->match(&display) == 0);
    display.flags = 0;
    --display.width;
    CHECK(theme->match(&display) == 0);
    ++display.width;
    --display.height;
    CHECK(theme->match(&display) == 0);
    ++display.height;
    display.pixel = XIAOTAI_PIXEL_RGB565;
    CHECK(theme->match(&display) == 0);

    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_theme_mono128_init() == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_at(0) == theme && xiaotai_ui_theme_count() == 1);
    CHECK(xiaotai_engine_theme_mono128_init() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_ui_theme_select(&display));
    display.pixel = XIAOTAI_PIXEL_MONO1_VLSB;
    CHECK(xiaotai_ui_theme_select(&display) == theme);
    xiaotai_ui_reset();
    return 0;
}

static int list_window_and_toast(void)
{
    const xiaotai_display_desc_t descs[] = {
        ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB), ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1),
    };
    ui_fake_board_set_displays(descs, 2);
    ((xiaotai_display_t *)ui_fake_display(0))->present = page_major;
    ((xiaotai_display_t *)ui_fake_display(1))->present = row_major;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_theme_mono128_init() == XIAOTAI_OK);
    ui_driver_manual(true);
    ui_core_set_clock(clock_ms);
    xiaotai_engine_ui_start();
    const xiaotai_theme_t *theme = &xiaotai_theme_mono128;
    CHECK(ui_display_at(0)->theme == theme && ui_display_at(1)->theme == theme &&
          ui_display_at(0)->theme_state != ui_display_at(1)->theme_state);
    const int64_t before_alloc = xiaotai_mem_host_outstanding();
    xiaotai_mem_host_fail_nth(1);
    void *unstarted = &now_ms;
    CHECK(theme->start(&ui_display_at(1)->host, &unstarted) == XIAOTAI_ERR_NO_MEM);
    CHECK(!unstarted && xiaotai_mem_host_outstanding() == before_alloc);

    static const xiaotai_view_item_t items[] = {
        {.label = "A", .intent = "pick.a"},
        {.label = "HIDDEN", .intent = "pick.hidden", .state = XIAOTAI_STATE_HIDDEN},
        {.label = "abcdefghijklmnopqrstuvwxyz", .intent = "pick.b"},
        {.label = "ABCDEFGHIJKLMNOPQRSTUVWXYZ", .detail = "42", .intent = "pick.c"},
        {.label = "D", .intent = "pick.d"},
        {.label = "E", .intent = "pick.e"},
    };
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_LIST, .id = "list", .title = "TITLE",
                          .focus = 4, .items = items, .item_count = 6};
    show(page);
    CHECK(writes[0] && writes[1]);
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 128; ++x)
            CHECK(lit(panels[0], x, y) ==
                  ((panels[1][y * 16 + x / 8] & (0x80U >> (x % 8))) != 0U));
    for (unsigned x = 0; x < 128; ++x) CHECK(lit(panels[0], x, 13));
    CHECK(has_pixels(panels[0], 112, 0, 128, 12)); /* focused detail in header */
    CHECK(!lit(panels[0], 0, 30) && lit(panels[0], 0, 31) && lit(panels[0], 123, 45));
    CHECK(!lit(panels[0], 0, 46) && !lit(panels[0], 124, 33));
    CHECK(!lit(panels[0], 126, 23) && lit(panels[0], 126, 24)); /* 27-pixel thumb */
    CHECK(lit(panels[0], 127, 50) && !lit(panels[0], 127, 51));

    xiaotai_ui_control_t controls[3];
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 3) == 3);
    CHECK(controls[0].index == 2 && controls[1].index == 3 && controls[2].index == 4);
    CHECK(controls[1].layer == XIAOTAI_LAYER_PAGE && controls[1].element == XIAOTAI_UI_ELEMENT_ITEM);
    CHECK(controls[1].y == 31 && controls[1].width == 124 && controls[1].height == 15);

    uint8_t first[BYTES];
    memcpy(first, panels[0], BYTES);
    const unsigned before_scroll = writes[0];
    step(100);
    step(200);
    step(300);
    CHECK(writes[0] == before_scroll);
    step(400);
    step(401); /* apply the redraw enqueued by tick */
    CHECK(writes[0] == before_scroll + 1 && memcmp(first, panels[0], BYTES));
    for (unsigned y = 0; y < 64; ++y) {
        if (y >= 33 && y < 45) continue;
        for (unsigned x = 0; x < 128; ++x) CHECK(lit(first, x, y) == lit(panels[0], x, y));
    }

    memcpy(first, panels[0], BYTES);
    xiaotai_view_t toast = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST,
                           .text = "SAVED", .duration_ms = 1500};
    show(toast);
    for (unsigned y = 0; y < 46; ++y)
        for (unsigned x = 0; x < 128; ++x) CHECK(lit(first, x, y) == lit(panels[0], x, y));
    CHECK(!has_pixels(panels[0], 0, 46, 128, 47) && lit(panels[0], 0, 47));
    CHECK(lit(panels[0], 127, 63) && memcmp(first, panels[0], BYTES));
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 3) == 2);
    step(1900);
    CHECK(lit(panels[0], 127, 63));
    step(1901);
    CHECK(!lit(panels[0], 127, 63));
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 3) == 3);

    page.focus = 6; /* the last visible item, shorter than the viewport */
    page.status = "OK";
    show(page);
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 3) == 3);
    CHECK(controls[0].index == 3 && controls[1].index == 4 && controls[2].index == 5);
    const unsigned settled = writes[0];
    const unsigned frames = ui_display_at(0)->frames;
    step(2001);
    step(2101);
    step(2201);
    CHECK(writes[0] == settled && ui_display_at(0)->frames == frames && !ui_display_at(0)->driver.tick);

    static const xiaotai_view_action_t actions[] = {
        {.role = XIAOTAI_ROLE_BACK, .label = "BACK", .intent = "back"},
        {.role = XIAOTAI_ROLE_PRIMARY, .label = "DO", .intent = "go"},
        {.role = XIAOTAI_ROLE_AUX, .label = "NEXT", .intent = "next"},
    };
    xiaotai_view_t dialog = {.kind = XIAOTAI_VIEW_LIST, .id = "empty", .title = "EMPTY",
                            .text = "NO ITEMS", .actions = actions, .action_count = 3,
                            .layer = XIAOTAI_LAYER_OVERLAY};
    show(dialog);
    CHECK(has_pixels(panels[0], 36, 32, 92, 44) && has_pixels(panels[0], 16, 51, 112, 63));
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 2) == 3);
    CHECK(controls[0].index == 1 && controls[1].index == 2 && controls[0].x < controls[1].x);
    CHECK(controls[0].element == XIAOTAI_UI_ELEMENT_ACTION && controls[0].layer == XIAOTAI_LAYER_OVERLAY);

    const xiaotai_view_t settings = {.kind = XIAOTAI_VIEW_NOTICE, .id = "settings", .title = "设置", .text = "音量"};
    step(UINT32_MAX - 100U);
    show(settings);
    memcpy(first, panels[0], BYTES);
    const xiaotai_view_t rejected = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST,
                                    .text = "操作未执行", .duration_ms = 1500};
    show(rejected);
    step(1398U);
    CHECK(memcmp(first, panels[0], BYTES) && !memcmp(first, panels[0], 5U * 128U));
    step(1399U);
    CHECK(!memcmp(first, panels[0], BYTES));

    xiaotai_ui_reset();
    ui_driver_manual(false);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    return 0;
}

int main(void)
{
    if (!strcmp(MONO_TEST_CASE, "match")) RUN(match_rule);
    else RUN(list_window_and_toast);
    return 0;
}
