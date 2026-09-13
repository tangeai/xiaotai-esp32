#include "app.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_internal.h"
#include "app_task_affinity.h"
#include "qr_scanner.h"

static const char *TAG = "app_tirtc_scan";

#define APP_TIRTC_SCAN_RESTORE_TASK_STACK_SIZE 4096
#define APP_TIRTC_SCAN_RESTORE_TASK_PRIORITY   3

typedef struct {
	app_scan_preview_cb_t preview_cb;
	app_tirtc_config_scan_result_cb_t result_cb;
	void *ctx;
	bool resources_acquired;
} app_tirtc_config_scan_state_t;

static app_tirtc_config_scan_state_t s_tirtc_config_scan;

static void app_tirtc_config_scan_restore_task(void *arg)
{
	bool restore = (bool)(uintptr_t)arg;

	if (restore) {
		app_release_tirtc_config_scan_resources();
	}
	vTaskDeleteWithCaps(NULL);
}

static void app_restore_tirtc_config_scan_resources(bool restore)
{
	bool resources_acquired = s_tirtc_config_scan.resources_acquired;

	s_tirtc_config_scan.resources_acquired = false;
	if (restore && resources_acquired) {
		app_release_tirtc_config_scan_resources();
	}
}

static void app_defer_tirtc_config_scan_resources(bool restore)
{
	bool resources_acquired = s_tirtc_config_scan.resources_acquired;

	s_tirtc_config_scan.resources_acquired = false;
	if (!restore || !resources_acquired) {
		return;
	}

	BaseType_t task_ret = xTaskCreateWithCaps(app_tirtc_config_scan_restore_task,
						  "tirtc_scan_res",
						  APP_TIRTC_SCAN_RESTORE_TASK_STACK_SIZE,
						  (void *)(uintptr_t)restore,
						  APP_TIRTC_SCAN_RESTORE_TASK_PRIORITY,
						  NULL,
						  APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		ESP_LOGW(TAG, "defer tirtc config scan resource release failed; releasing inline");
		app_release_tirtc_config_scan_resources();
	}
}

static void app_tirtc_config_scan_preview_cb(const scan_preview_frame_t *frame,
					     void *ctx)
{
	(void)ctx;

	if (s_tirtc_config_scan.preview_cb != NULL) {
		s_tirtc_config_scan.preview_cb(frame, s_tirtc_config_scan.ctx);
	}
}

static void app_tirtc_config_scan_result_cb(esp_err_t result,
					    const qr_scanner_contact_t *contact,
					    void *ctx)
{
	const char *device_id = "";
	const char *device_secret = "";
	const char *raw_payload = "";
	app_tirtc_config_scan_result_cb_t result_cb = s_tirtc_config_scan.result_cb;
	void *result_ctx = s_tirtc_config_scan.ctx;

	(void)ctx;

	if (contact != NULL && contact->raw_payload[0] != '\0') {
		raw_payload = contact->raw_payload;
	}
	if (result == ESP_OK) {
		if (contact == NULL || contact->device_id[0] == '\0' || contact->pair_key[0] == '\0') {
			result = ESP_ERR_INVALID_RESPONSE;
		} else {
			device_id = contact->device_id;
			device_secret = contact->pair_key;
			ESP_LOGD(TAG, "tirtc config QR accepted: device_id_len=%u", (unsigned)strlen(device_id));
		}
	}

	if (result_cb != NULL) {
		result_cb(result, device_id, device_secret, raw_payload, result_ctx);
	}
	if (result == ESP_OK) {
		esp_err_t save_ret = app_request_update_rtc_device_credentials(device_id, device_secret);
		if (save_ret == ESP_OK) {
			ESP_LOGD(TAG, "tirtc config credential save queued");
		} else {
			ESP_LOGW(TAG,
				 "tirtc config credential save queue failed after UI dispatch: ret=%s",
				 esp_err_to_name(save_ret));
		}
	}
	app_defer_tirtc_config_scan_resources(true);
	s_tirtc_config_scan = (app_tirtc_config_scan_state_t){0};
}

esp_err_t app_start_tirtc_config_scan(app_scan_preview_cb_t preview_cb,
				      app_tirtc_config_scan_result_cb_t result_cb,
				      void *ctx)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_SYSTEM) {
		ESP_LOGW(TAG, "start tirtc config scan rejected: active_app=%d", (int)app_get_active_app());
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_acquire_tirtc_config_scan_resources();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start tirtc config scan failed while acquiring camera: %s", esp_err_to_name(ret));
		return ret;
	}

	s_tirtc_config_scan = (app_tirtc_config_scan_state_t){
		.preview_cb = preview_cb,
		.result_cb = result_cb,
		.ctx = ctx,
		.resources_acquired = true,
	};

	ret = qr_scanner_start_contact(app_tirtc_config_scan_preview_cb,
				       app_tirtc_config_scan_result_cb,
				       NULL);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start tirtc config scan failed while starting qr scanner: %s", esp_err_to_name(ret));
		app_restore_tirtc_config_scan_resources(true);
		s_tirtc_config_scan = (app_tirtc_config_scan_state_t){0};
	} else {
		ESP_LOGD(TAG, "tirtc config scan started");
	}
	return ret;
}

esp_err_t app_stop_tirtc_config_scan(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
		app_restore_tirtc_config_scan_resources(true);
		s_tirtc_config_scan = (app_tirtc_config_scan_state_t){0};
	}
	return ret;
}

void app_cancel_tirtc_config_scan_for_lifecycle(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "cancel tirtc config scan failed: %s", esp_err_to_name(ret));
	}
	app_restore_tirtc_config_scan_resources(true);
	s_tirtc_config_scan = (app_tirtc_config_scan_state_t){0};
}
