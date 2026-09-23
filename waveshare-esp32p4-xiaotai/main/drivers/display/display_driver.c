#include "display_driver.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_35.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "app_task_affinity.h"
#include "hardware_board.h"

static const char *TAG = "display_driver";

#define DISPLAY_DRIVER_LANDSCAPE_ROTATION LV_DISP_ROT_270
#define DISPLAY_DRIVER_PORTRAIT_ROTATION  LV_DISP_ROT_NONE
#define DISPLAY_DRIVER_DRAW_LINES 32
#define DISPLAY_DRIVER_TRANSFER_LINES 16
#define DISPLAY_DRIVER_LVGL_TASK_PRIORITY 6
#define DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX  18
#define DISPLAY_DRIVER_TOUCH_SCROLL_THROW     0
#define DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX 45

static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_initialized;
static display_driver_orientation_t s_orientation = DISPLAY_DRIVER_ORIENTATION_LANDSCAPE;

_Static_assert(LV_COLOR_DEPTH == 16, "direct LCD video requires RGB565 LVGL color depth");

static void display_driver_configure_touch(lv_indev_t *indev)
{
	if (indev == NULL || indev->driver == NULL) {
		return;
	}

	indev->driver->scroll_limit = DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX;
	indev->driver->scroll_throw = DISPLAY_DRIVER_TOUCH_SCROLL_THROW;
	indev->driver->gesture_limit = DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX;
	ESP_LOGI(TAG,
		 "touch input ready: scroll_limit=%u scroll_throw=%u gesture_limit=%u",
		 (unsigned)indev->driver->scroll_limit,
		 (unsigned)indev->driver->scroll_throw,
		 (unsigned)indev->driver->gesture_limit);
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
		.buffer_size = BSP_LCD_V_RES * DISPLAY_DRIVER_DRAW_LINES,
		.trans_size = BSP_LCD_V_RES * DISPLAY_DRIVER_TRANSFER_LINES,
		.double_buffer = true,
		.flags = {
			/*
			 * Draw in PSRAM and reserve one 16-line internal DMA transport
			 * buffer at display startup. LVGL uses this path for normal pages
			 * and while call controls are visible; 16 lines reduce a 480x320
			 * refresh from 80 synchronous SPI chunks to 20. Once controls
			 * auto-hide, call video switches to one frame-sized PSRAM DMA
			 * transaction. ESP-Hosted RX keeps its own fixed DMA buffers.
			 */
			.buff_dma = false,
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
	lv_disp_set_rotation(s_display, DISPLAY_DRIVER_LANDSCAPE_ROTATION);
	s_orientation = DISPLAY_DRIVER_ORIENTATION_LANDSCAPE;

	s_touch_indev = bsp_display_get_input_dev();
	ESP_RETURN_ON_FALSE(s_touch_indev != NULL, ESP_FAIL, TAG, "bsp touch init failed");
	display_driver_configure_touch(s_touch_indev);
	ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight on failed");
	s_initialized = true;
	if (handles != NULL) {
		handles->display = s_display;
		handles->touch_indev = s_touch_indev;
	}

	ESP_LOGI(TAG,
		 "display ready: physical=%dx%d ui=%ux%u rotation=%u draw_buf=%uB buffers=2 "
		 "caps=psram lvgl_transfer=%uB caps=internal-dma direct_video=psram-dma",
		 BSP_LCD_H_RES,
		 BSP_LCD_V_RES,
		 display_driver_width(),
		 display_driver_height(),
		 (unsigned)DISPLAY_DRIVER_LANDSCAPE_ROTATION,
		 (unsigned)(BSP_LCD_V_RES * DISPLAY_DRIVER_DRAW_LINES * sizeof(lv_color_t)),
		 (unsigned)(BSP_LCD_V_RES * DISPLAY_DRIVER_TRANSFER_LINES * sizeof(lv_color_t)));
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

	lv_disp_rot_t rotation =
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

	/* The panel call queues one frame-sized PSRAM DMA transfer. Drain it before
	 * the renderer releases this frame slot back to the converter. */
	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, -1, NULL, 0),
				    TAG,
				    "direct LCD DMA wait failed");
	if (elapsed_us != NULL) {
		*elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
	}
	return ESP_OK;
}
