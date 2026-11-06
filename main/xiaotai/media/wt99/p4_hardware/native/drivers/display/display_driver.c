#include "display_driver.h"
#include "xiaotai_lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"

#include "bsp/wt99p461_s1.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <string.h>

#include "app_task_affinity.h"
#include "hardware_board.h"

static const char *TAG = "display_driver";

#define DISPLAY_DRIVER_LANDSCAPE_ROTATION LV_DISPLAY_ROTATION_0
#define DISPLAY_DRIVER_PORTRAIT_ROTATION  LV_DISPLAY_ROTATION_90
#define DISPLAY_DRIVER_FRAME_PIXELS (BSP_LCD_H_RES * BSP_LCD_V_RES)
#define DISPLAY_DRIVER_LVGL_TASK_PRIORITY APP_TASK_PRIORITY_UI
#define DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX  18
#define DISPLAY_DRIVER_TOUCH_SCROLL_THROW     0
#define DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX 45

static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_initialized;
static display_driver_orientation_t s_orientation = DISPLAY_DRIVER_ORIENTATION_LANDSCAPE;

_Static_assert(LV_COLOR_DEPTH == 16, "direct LCD video requires RGB565 LVGL color depth");

#if CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
#include "display_ppa_blit.inc"
#endif

void display_driver_log_ppa_diagnostics(void)
{
#if CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
    display_ppa_state_t state;
    taskENTER_CRITICAL(&s_display_ppa_lock);
    state=s_display_ppa_state;
    taskEXIT_CRITICAL(&s_display_ppa_lock);
    ESP_LOGI(TAG, "LCD PPA state: engine=blend sequence=%lu stage=%lu age_ms=%lu size=%lux%lu clip=%d,%d-%d,%d core=%lu task=%p src=%p dst=%p completed=%lu",
             (unsigned long)state.sequence, (unsigned long)state.stage,
             (unsigned long)((uint32_t)(esp_timer_get_time()/1000)-state.at_ms),
             (unsigned long)state.width, (unsigned long)state.height,
             state.clip.x1,state.clip.y1,state.clip.x2,state.clip.y2,
             (unsigned long)state.core,(void *)state.task,(void *)state.source,
             (void *)state.destination,(unsigned long)atomic_load(&s_display_ppa_frames));
#else
    ESP_LOGI(TAG, "LCD PPA diagnostics disabled");
#endif
}

#if CONFIG_APP_DISPLAY_TIMING_DIAGNOSTICS
static int64_t s_render_started_us, s_flush_started_us, s_monitor_started_us;
static uint64_t s_render_total_us, s_flush_total_us, s_image_total_us;
static uint32_t s_render_max_us, s_flush_max_us, s_render_count, s_image_count, s_image_max_us;

static void display_driver_timing(lv_event_t *event)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START: s_render_started_us = now; break;
    case LV_EVENT_FLUSH_START: s_flush_started_us = now; break;
    case LV_EVENT_FLUSH_FINISH: {
        uint32_t elapsed = now - s_flush_started_us;
        s_flush_total_us += elapsed;
        if (elapsed > s_flush_max_us) s_flush_max_us = elapsed;
        break;
    }
    case LV_EVENT_RENDER_READY: {
        uint32_t elapsed = now - s_render_started_us;
        s_render_total_us += elapsed;
        ++s_render_count;
        if (elapsed > s_render_max_us) s_render_max_us = elapsed;
        if (now - s_monitor_started_us < 15000000) break;
        ESP_LOGI(TAG, "LCD frames=%lu render_avg_max_us=%llu/%lu flush_total_max_us=%llu/%lu image_n_total_max=%lu/%llu/%lu",
                 (unsigned long)s_render_count,
                 (unsigned long long)(s_render_total_us / s_render_count), (unsigned long)s_render_max_us,
                 (unsigned long long)s_flush_total_us, (unsigned long)s_flush_max_us,
                 (unsigned long)s_image_count, (unsigned long long)s_image_total_us, (unsigned long)s_image_max_us);
        s_monitor_started_us = now;
        s_render_count = s_render_max_us = s_flush_max_us = s_image_count = s_image_max_us = 0;
        s_render_total_us = s_flush_total_us = s_image_total_us = 0;
        break;
    }
    default: break;
    }
}
#endif

#if CONFIG_APP_DISPLAY_TIMING_DIAGNOSTICS || CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
static bool display_driver_draw_image(lv_draw_task_t *task, const lv_draw_image_dsc_t *image,
                                      const lv_area_t *area)
{
    int64_t started = esp_timer_get_time();
    bool drawn = false;
#if CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
    drawn = display_ppa_try_image(task, image, area);
#endif
    if (!drawn) lv_draw_sw_image(task, image, area);
#if CONFIG_APP_DISPLAY_TIMING_DIAGNOSTICS
    uint32_t elapsed = esp_timer_get_time() - started;
    ++s_image_count;
    s_image_total_us += elapsed;
    if (elapsed > s_image_max_us) s_image_max_us = elapsed;
#else
    (void)started;
#endif
    return true;
}
#endif

static void display_driver_configure_touch(lv_indev_t *indev)
{
    if (!indev) return;
    lv_indev_set_scroll_limit(indev, DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX);
    lv_indev_set_scroll_throw(indev, DISPLAY_DRIVER_TOUCH_SCROLL_THROW);
    lv_indev_set_gesture_min_distance(indev, DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX);
    ESP_LOGI(TAG, "touch input ready: scroll_limit=%u scroll_throw=%u gesture_limit=%u",
             DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX, DISPLAY_DRIVER_TOUCH_SCROLL_THROW,
             DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX);
}

esp_err_t display_driver_init(display_driver_handles_t *handles)
{
	if (s_initialized) {
		if (handles != NULL) {
			handles->display = s_display;
			handles->touch_indev = s_touch_indev;
		}
		return ESP_OK;
	}

	bsp_display_cfg_t cfg = {
		.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
		.buffer_size = DISPLAY_DRIVER_FRAME_PIXELS,
		.trans_size = 0,
		.double_buffer = true,
		.flags = {
			/* DSI scanout buffers are shared with LVGL, not SPI transfer buffers. */
			.buff_dma = true,
			.buff_spiram = true,
		},
	};
	cfg.lvgl_port_cfg.task_stack = 12 * 1024;
	cfg.lvgl_port_cfg.task_priority = DISPLAY_DRIVER_LVGL_TASK_PRIORITY;
	cfg.lvgl_port_cfg.task_affinity = APP_TASK_CORE_UI;
	cfg.lvgl_port_cfg.task_stack_caps = APP_TASK_STACK_CAPS_INTERNAL;
	/* Match the BSP's bounded UI loop even when no animation timer is active. */
	cfg.lvgl_port_cfg.task_max_sleep_ms = 20;

	s_display = bsp_display_start_with_config(&cfg);
	ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "bsp display start failed");
	ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, TAG, "LVGL lock failed");
	lv_disp_set_rotation(s_display, DISPLAY_DRIVER_LANDSCAPE_ROTATION);
	s_orientation = DISPLAY_DRIVER_ORIENTATION_LANDSCAPE;
#if CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
    display_ppa_install();
    lv_display_add_event_cb(s_display, display_ppa_flush_event, LV_EVENT_ALL, NULL);
#endif
#if CONFIG_APP_DISPLAY_TIMING_DIAGNOSTICS
    lv_display_add_event_cb(s_display, display_driver_timing, LV_EVENT_ALL, NULL);
#endif
#if CONFIG_APP_DISPLAY_TIMING_DIAGNOSTICS || CONFIG_APP_DISPLAY_PPA_BLIT_DIAGNOSTICS
    esp_err_t hook_error = xiaotai_lvgl_image_hook(display_driver_draw_image);
    if (hook_error != ESP_OK) {
        lvgl_port_unlock();
        return hook_error;
    }
#endif

	s_touch_indev = bsp_display_get_input_dev();
	if (s_touch_indev == NULL) {
		lvgl_port_unlock();
		return ESP_FAIL;
	}
	display_driver_configure_touch(s_touch_indev);
	lvgl_port_unlock();
	ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight on failed");
	s_initialized = true;
	if (handles != NULL) {
		handles->display = s_display;
		handles->touch_indev = s_touch_indev;
	}

	ESP_LOGI(TAG,
		 "display ready: physical=%dx%d ui=%ux%u rotation=%u draw_buf=%uB buffers=2 "
		 "caps=psram-dma lvgl_transfer=dsi-frame-switch",
		 BSP_LCD_H_RES,
		 BSP_LCD_V_RES,
		 display_driver_width(),
		 display_driver_height(),
		 (unsigned)DISPLAY_DRIVER_LANDSCAPE_ROTATION,
		 (unsigned)(DISPLAY_DRIVER_FRAME_PIXELS * sizeof(uint16_t)));
	return ESP_OK;
}

bool display_driver_is_initialized(void)
{
	return s_initialized;
}

uint16_t display_driver_width(void)
{
	if (s_initialized && s_display != NULL) {
		return (uint16_t)lv_disp_get_hor_res(s_display);
	}
	return hardware_board_get_display_config()->width;
}

uint16_t display_driver_height(void)
{
	if (s_initialized && s_display != NULL) {
		return (uint16_t)lv_disp_get_ver_res(s_display);
	}
	return hardware_board_get_display_config()->height;
}

display_driver_orientation_t display_driver_get_orientation(void)
{
	return s_orientation;
}

esp_err_t display_driver_set_orientation(display_driver_orientation_t orientation)
{
	ESP_RETURN_ON_FALSE(s_initialized && s_display != NULL,
			    ESP_ERR_INVALID_STATE,
			    TAG,
			    "display is not initialized");
	ESP_RETURN_ON_FALSE(orientation == DISPLAY_DRIVER_ORIENTATION_PORTRAIT ||
				orientation == DISPLAY_DRIVER_ORIENTATION_LANDSCAPE,
			    ESP_ERR_INVALID_ARG,
			    TAG,
			    "invalid display orientation");
	if (orientation == s_orientation) {
		return ESP_OK;
	}
	/* The product layout and DSI scanout use a fixed native landscape surface.
	 * JD9165 has no axis-swap command; do not report a hardware rotation success. */
	ESP_RETURN_ON_FALSE(orientation == DISPLAY_DRIVER_ORIENTATION_LANDSCAPE,
	                    ESP_ERR_NOT_SUPPORTED, TAG, "native landscape panel");

	lv_display_rotation_t rotation =
		orientation == DISPLAY_DRIVER_ORIENTATION_PORTRAIT ?
			DISPLAY_DRIVER_PORTRAIT_ROTATION :
			DISPLAY_DRIVER_LANDSCAPE_ROTATION;
	lv_disp_set_rotation(s_display, rotation);
	s_orientation = orientation;
	ESP_LOGI(TAG,
		 "display orientation: mode=%s ui=%ux%u rotation=%u",
		 orientation == DISPLAY_DRIVER_ORIENTATION_PORTRAIT ? "portrait" : "landscape",
		 display_driver_width(),
		 display_driver_height(),
		 (unsigned)rotation);
	return ESP_OK;
}

esp_err_t display_driver_blit_rgb565(uint16_t x,
                                    uint16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    const uint16_t *pixels,
                                    uint32_t *elapsed_us)
{
	ESP_RETURN_ON_FALSE(s_initialized && s_display != NULL,
				    ESP_ERR_INVALID_STATE,
				    TAG,
				    "display is not initialized");
	ESP_RETURN_ON_FALSE(pixels != NULL && width > 0U && height > 0U,
				    ESP_ERR_INVALID_ARG,
				    TAG,
				    "invalid direct LCD frame");
	ESP_RETURN_ON_FALSE((uint32_t)x + width <= display_driver_width() &&
					(uint32_t)y + height <= display_driver_height(),
				    ESP_ERR_INVALID_SIZE,
				    TAG,
				    "direct LCD region is outside the display");

	esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
	esp_lcd_panel_io_handle_t io = bsp_display_get_io_handle();
	ESP_RETURN_ON_FALSE(panel != NULL && io != NULL,
				    ESP_ERR_INVALID_STATE,
				    TAG,
				    "LCD handles are unavailable");

	size_t transfer_size = (size_t)width * height * sizeof(*pixels);
	ESP_RETURN_ON_FALSE(esp_ptr_external_ram(pixels) &&
				    esp_ptr_dma_ext_capable(pixels),
				    ESP_ERR_INVALID_ARG,
				    TAG,
				    "direct LCD frame is not PSRAM DMA capable");

	int64_t started_us = esp_timer_get_time();
	ESP_RETURN_ON_ERROR(esp_cache_msync((void *)pixels,
					 transfer_size,
					 ESP_CACHE_MSYNC_FLAG_DIR_C2M),
				    TAG,
				    "direct LCD cache sync failed");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel,
					      x,
					      y,
					      x + width,
					      y + height,
					      pixels),
				    TAG,
				    "direct LCD draw failed");

	/* This BSP deliberately leaves DMA2D copies disabled. A non-scanout input
	 * is copied synchronously by the IDF DPI driver; there is no SPI IO to drain.
	 * The product's normal video path composes through LVGL and swaps at VSYNC. */
	if (elapsed_us != NULL) {
		*elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
	}
	return ESP_OK;
}
