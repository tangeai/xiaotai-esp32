/* P4 home only. A fixed RGB565 image preserves LVGL's clipped image blits;
 * no gradient calculation runs in the face animation or audio tasks. */
#define P4_HOME_CENTER_COLOR 0x1C1F22
#define P4_HOME_EDGE_COLOR 0x101315

static EXT_RAM_BSS_ATTR struct {
    lv_img_dsc_t image;
    lv_color_t *pixels;
    bool active;
} s_p4_home_background;

static uint8_t p4_home_background_weight(int x, int y)
{
    int32_t nx = (2 * x - (LCD_H_RES - 1)) * 256 / (LCD_H_RES - 1);
    int32_t ny = (2 * y - (LCD_V_RES - 1)) * 256 / (LCD_V_RES - 1);
    uint32_t radius2 = (uint32_t)(nx * nx + ny * ny);
    /* The full zoomed face canvas fits inside the flat centre, including
     * its sampling margin. This avoids changing the face rasterizer. */
    if (radius2 <= 40000U) return 255;
    if (radius2 >= 65536U) return 0;
    uint32_t t = (radius2 - 40000U) * 256U / (65536U - 40000U);
    uint32_t smooth = (t * t * (768U - 2U * t) + 32768U) / 65536U;
    return (uint8_t)((256U - smooth) * 255U / 256U);
}

static void p4_home_background_reset(lv_obj_t *screen)
{
    s_p4_home_background.active = false;
    lv_obj_set_style_bg_img_src(screen, NULL, 0);
}

static void p4_home_background_show(lv_obj_t *screen)
{
    if (s_p4_home_background.pixels == NULL) {
        const size_t bytes = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);
        s_p4_home_background.pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_p4_home_background.pixels == NULL) {
            ESP_LOGE(TAG, "home background allocation failed: PSRAM bytes=%u", (unsigned)bytes);
            return;
        }
        const lv_color_t centre = lv_color_hex(P4_HOME_CENTER_COLOR);
        const lv_color_t edge = lv_color_hex(P4_HOME_EDGE_COLOR);
        for (int y = 0; y < LCD_V_RES; ++y) {
            for (int x = 0; x < LCD_H_RES; ++x) {
                s_p4_home_background.pixels[y * LCD_H_RES + x] =
                    lv_color_mix(centre, edge, p4_home_background_weight(x, y));
            }
        }
        s_p4_home_background.image = (lv_img_dsc_t) {
            .header.cf = LV_IMG_CF_TRUE_COLOR,
            .header.w = LCD_H_RES,
            .header.h = LCD_V_RES,
            .data_size = bytes,
            .data = (const uint8_t *)s_p4_home_background.pixels,
        };
    }
    /* Retain one 300 KiB PSRAM bitmap across page changes, never a copy per page. */
    s_p4_home_background.active = true;
    lv_obj_set_style_bg_img_src(screen, &s_p4_home_background.image, 0);
    lv_obj_set_style_bg_img_opa(screen, LV_OPA_COVER, 0);
}
