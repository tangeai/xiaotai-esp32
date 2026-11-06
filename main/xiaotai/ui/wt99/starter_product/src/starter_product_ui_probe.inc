/* UART samples enter the existing pointer driver; LVGL owns hit testing and
 * dispatch. A held maintenance press expires even if the host disappears. */
#include <inttypes.h>

static void (*s_ui_touch_read)(lv_indev_t *, lv_indev_data_t *);
static bool s_ui_pointer_held;
static lv_point_t s_ui_pointer_point;
static int64_t s_ui_pointer_until;

static void ui_probe_read(lv_indev_t *driver, lv_indev_data_t *data)
{
    unsigned sample = atomic_exchange(&s_ui_pointer, 0);
    if (sample != 0) {
        s_ui_pointer_point.x = sample & 0xfffU;
        s_ui_pointer_point.y = (sample >> 12) & 0xfffU;
        s_ui_pointer_held = (sample & 0x40000000U) != 0;
        s_ui_pointer_until = monotonic_ms() + 1000;
        data->point = s_ui_pointer_point;
        data->state = s_ui_pointer_held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    if (s_ui_pointer_held) {
        data->point = s_ui_pointer_point;
        if (monotonic_ms() >= s_ui_pointer_until) s_ui_pointer_held = false;
        data->state = s_ui_pointer_held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    s_ui_touch_read(driver, data);
}

static void ui_probe_objects(lv_obj_t *object, bool hidden)
{
    hidden |= lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN);
    if (hidden) return;
    if (lv_obj_check_type(object, &lv_button_class)) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        const char *text = "";
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
            lv_obj_t *child = lv_obj_get_child(object, i);
            if (lv_obj_check_type(child, &lv_label_class) && lv_label_get_text(child)[0]) {
                text = lv_label_get_text(child);
                break;
            }
        }
        printf("+UIBTN:x=%" PRId32 ",y=%" PRId32 ",w=%" PRId32 ",h=%" PRId32 ",enabled=%u,text=%s\r\n",
            area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area),
            (unsigned)!lv_obj_has_state(object, LV_STATE_DISABLED), text);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        ui_probe_objects(lv_obj_get_child(object, i), hidden);
}

static void ui_probe_tick(void)
{
    if (!atomic_exchange(&s_ui_inspect, false)) return;
    lv_obj_update_layout(lv_scr_act());
    flockfile(stdout);
    printf("+UI:page=%u,role=%s,setup=%u,awake=%u,door_modal=%u\r\n", (unsigned)s_page,
        s_preferences.door_sender ? "sender" : "viewer", (unsigned)s3_ui.setup_active,
        (unsigned)s_display_awake, (unsigned)door_ui.modal_kind);
    ui_probe_objects(lv_scr_act(), false);
    ui_probe_objects(lv_layer_top(), false);
    puts("+UI:END");
    fflush(stdout);
    funlockfile(stdout);
}
