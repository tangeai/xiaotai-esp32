#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t, lv_disp_t, lv_indev_t;
typedef struct { void *user_data; } esp_lcd_panel_t;
typedef struct { int reset_gpio_num; esp_err_t (*del)(esp_lcd_panel_t *); } jd9165_panel_t;
typedef struct { int lvgl_port_cfg; } bsp_display_cfg_t;
typedef struct {
    void *io_handle, *panel_handle;
    int buffer_size, hres, vres, color_format;
    bool double_buffer;
    struct { bool buff_spiram, direct_mode; } flags;
} lvgl_port_display_cfg_t;
typedef struct { struct { bool avoid_tearing; } flags; } lvgl_port_display_dsi_cfg_t;
typedef struct { void *disp, *handle; } lvgl_port_touch_cfg_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t code = (expr); if (code != ESP_OK) return code; } while (0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define BSP_LCD_H_RES 1024
#define BSP_LCD_V_RES 600
#define BSP_LCD_ENABLE 1
#define LV_COLOR_FORMAT_RGB565 1
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_1 1

static void *s_panel, *s_panel_io, *s_dsi, *s_touch, *s_touch_io, *s_phy_ldo;
static lv_disp_t *s_display;
static lv_indev_t *s_indev;
static bool s_backlight_ready, s_lvgl_ready, s_cleanup_failed;
static int marker, live_display, panel_live, locks, starts, cleanup_step, cleanup_fail;
static int fail_input, fail_display, fail_lock, fail_dpi, frees;
static void counted_free(void *p) { ++frees; free(p); }
#define free(p) counted_free(p)
static int gpio_reset_pin(int pin) { return 0; }
static esp_err_t fake_dpi_del(esp_lcd_panel_t *panel) { return fail_dpi ? ESP_FAIL : ESP_OK; }
static int step(void) { return ++cleanup_step == cleanup_fail ? ESP_FAIL : ESP_OK; }
static int lvgl_port_remove_touch(void *p) { assert(locks && p); return step(); }
static int esp_lcd_panel_del(void *p)
{
    assert(panel_live && p);
    if (s_display) assert(locks && live_display); /* Callback still has valid context. */
    int result = step();
    if (result == ESP_OK) panel_live = 0;
    return result;
}
static int lvgl_port_remove_disp(void *p)
{
    assert(locks && live_display && !panel_live && p);
    int result = step();
    if (result == ESP_OK) live_display = 0;
    return result;
}
static int esp_lcd_panel_io_del(void *p) { assert(p); return step(); }
static int esp_lcd_del_dsi_bus(void *p) { assert(p && !s_panel && !s_panel_io); return step(); }
static int esp_lcd_touch_del(void *p) { assert(p && !s_indev); return step(); }
static int esp_ldo_release_channel(void *p) { assert(p && !s_dsi); return step(); }
static int ledc_stop(int mode, int channel, int duty) { return step(); }
static int gpio_set_level(int pin, int level) { return step(); }
static int display_power_init(void) { ++starts; s_phy_ldo = &marker; s_backlight_ready = true; return ESP_OK; }
static int touch_init(void) { s_touch = s_touch_io = &marker; return ESP_OK; }
static int panel_init(void) { s_panel = s_panel_io = s_dsi = &marker; panel_live = 1; return ESP_OK; }
static int lvgl_port_init(const void *cfg) { return ESP_OK; }
static bool lvgl_port_lock(int timeout) { if (fail_lock) return false; ++locks; return true; }
static void lvgl_port_unlock(void) { assert(locks > 0); --locks; }
static lv_disp_t *lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *cfg, const lvgl_port_display_dsi_cfg_t *dsi)
{
    assert(locks && cfg->flags.direct_mode && dsi->flags.avoid_tearing);
    if (fail_display) return NULL;
    live_display = 1; return &marker;
}
static lv_indev_t *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *cfg)
{
    assert(locks && cfg->disp && cfg->handle);
    return fail_input ? NULL : &marker;
}
