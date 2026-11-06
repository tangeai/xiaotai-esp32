#include <assert.h>
#include <stdio.h>
#include "lvgl.h"

enum { WIDTH = 160, HEIGHT = 80, S3_ROOM_KEY_BACKSPACE = 10, ROOM_KEY_BACKSPACE = 10 };
static uint16_t pixels[WIDTH * HEIGHT];
static unsigned labels, backspaces, flushes;
#include "key_draw.inc"

static void inspect(lv_event_t *event)
{
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(lv_event_get_draw_task(event));
    if (!label || label->base.part != LV_PART_ITEMS) return;
    ++labels;
    if (label->base.id1 == ROOM_KEY_BACKSPACE) {
        assert(label->font == &lv_font_montserrat_14);
        ++backspaces;
    }
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area; (void)data;
    ++flushes;
    lv_display_flush_ready(display);
}
int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    const char *keys[] = {"1", "2", "3", "4", "\n", "5", "6", "7", "8", "\n",
                          "9", "0", LV_SYMBOL_BACKSPACE, "OK", ""};
    lv_event_cb_t callbacks[] = {profile_0, profile_1, profile_2, profile_3};
    for (unsigned profile = 0; profile < 4; ++profile) {
        lv_obj_t *keyboard = lv_buttonmatrix_create(lv_screen_active());
        lv_buttonmatrix_set_map(keyboard, keys);
        lv_obj_set_size(keyboard, WIDTH, HEIGHT);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(keyboard, callbacks[profile], LV_EVENT_DRAW_TASK_ADDED, NULL);
        lv_obj_add_event_cb(keyboard, inspect, LV_EVENT_DRAW_TASK_ADDED, NULL);
        labels = backspaces = flushes = 0;
        lv_obj_invalidate(keyboard);
        lv_refr_now(display);
        assert(labels == 12 && backspaces == 1 && flushes > 0);
        lv_obj_delete(keyboard);
    }
    lv_display_delete(display);
    lv_deinit();
    puts("Four product keyboard callbacks render all keys and select the backspace symbol font on LVGL 9");
    return 0;
}
