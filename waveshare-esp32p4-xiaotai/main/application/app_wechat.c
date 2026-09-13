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
#include "network.h"
#include "qr_scanner.h"
#include "wechat_voip_service.h"

static const char *TAG = "app_wechat";

#define APP_WECHAT_SCAN_RESTORE_TASK_STACK_SIZE 4096
#define APP_WECHAT_SCAN_RESTORE_TASK_PRIORITY   3

typedef struct {
	app_scan_preview_cb_t preview_cb;
	app_wechat_contact_scan_result_cb_t result_cb;
	void *ctx;
	bool resources_suspended;
} app_wechat_contact_scan_state_t;

static app_wechat_contact_scan_state_t s_wechat_contact_scan;

static void app_wechat_scan_restore_task(void *arg)
{
	bool restore = (bool)(uintptr_t)arg;

	if (restore && app_get_active_app() == APP_ID_WECHAT) {
		esp_err_t ret = app_resume_wechat_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "restore wechat scan resources failed: %s", esp_err_to_name(ret));
		} else {
			ESP_LOGD(TAG, "wechat scan resources resumed: %s", esp_err_to_name(ret));
		}
	}
	vTaskDeleteWithCaps(NULL);
}

static void app_restore_wechat_scan_resources(bool restore)
{
	if (!s_wechat_contact_scan.resources_suspended) {
		return;
	}

	if (restore && app_get_active_app() == APP_ID_WECHAT) {
		esp_err_t ret = app_resume_wechat_scan_resources();
		if (ret != ESP_OK) {
			ESP_LOGW(TAG, "restore wechat scan resources failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_defer_wechat_scan_resources(bool restore)
{
	bool resources_suspended = s_wechat_contact_scan.resources_suspended;

	s_wechat_contact_scan.resources_suspended = false;
	if (!restore || !resources_suspended) {
		return;
	}

	BaseType_t task_ret = xTaskCreateWithCaps(app_wechat_scan_restore_task,
						  "wechat_scan_res",
						  APP_WECHAT_SCAN_RESTORE_TASK_STACK_SIZE,
						  (void *)(uintptr_t)restore,
						  APP_WECHAT_SCAN_RESTORE_TASK_PRIORITY,
						  NULL,
						  APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		ESP_LOGW(TAG, "defer wechat scan resource resume failed; resuming inline");
		esp_err_t ret = app_resume_wechat_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "inline wechat scan resource resume failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_wechat_scan_preview_cb(const scan_preview_frame_t *frame,
				       void *ctx)
{
	(void)ctx;

	if (s_wechat_contact_scan.preview_cb != NULL) {
		s_wechat_contact_scan.preview_cb(frame,
						s_wechat_contact_scan.ctx);
	}
}

static void app_wechat_scan_result_cb(esp_err_t result,
				      const qr_scanner_contact_t *contact,
				      void *ctx)
{
	char open_id_buf[APP_WECHAT_OPEN_ID_MAX] = {0};
	char raw_payload_buf[QR_SCANNER_PAYLOAD_MAX] = {0};
	app_wechat_contact_scan_result_cb_t result_cb = s_wechat_contact_scan.result_cb;
	void *result_ctx = s_wechat_contact_scan.ctx;

	(void)ctx;

	if (contact != NULL && contact->raw_payload[0] != '\0') {
		strlcpy(raw_payload_buf, contact->raw_payload, sizeof(raw_payload_buf));
	}
	if (result == ESP_OK) {
		if (contact == NULL || contact->open_id[0] == '\0') {
			result = ESP_ERR_INVALID_RESPONSE;
		} else {
			strlcpy(open_id_buf, contact->open_id, sizeof(open_id_buf));
			result = wechat_voip_service_add_contact(open_id_buf);
			if (result == ESP_OK) {
				ESP_LOGD(TAG, "authorized wechat contact QR accepted");
			}
		}
	}

	if (result_cb != NULL) {
		result_cb(result, open_id_buf, raw_payload_buf, result_ctx);
	}
	app_defer_wechat_scan_resources(true);
	s_wechat_contact_scan = (app_wechat_contact_scan_state_t){0};
}

esp_err_t app_wechat_call_contact(const char *open_id)
{
	return app_wechat_call_contact_with_type(open_id, APP_CALL_TYPE_VIDEO);
}

esp_err_t app_wechat_call_contact_with_type(const char *open_id,
					    app_call_type_t call_type)
{
	if (open_id == NULL || open_id[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (call_type != APP_CALL_TYPE_AUDIO &&
	    call_type != APP_CALL_TYPE_VIDEO) {
		return ESP_ERR_INVALID_ARG;
	}
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	wechat_voip_call_media_t call_media =
		call_type == APP_CALL_TYPE_VIDEO ?
			WECHAT_VOIP_CALL_MEDIA_VIDEO :
			WECHAT_VOIP_CALL_MEDIA_AUDIO;
	ESP_LOGD(TAG,
		 "wechat contact call requested: media=%s",
		 call_media == WECHAT_VOIP_CALL_MEDIA_VIDEO ? "video" : "audio");
	return wechat_voip_service_request_call(open_id, call_media);
}

esp_err_t app_wechat_add_contact(const char *open_id)
{
	if (open_id == NULL || open_id[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGD(TAG, "wechat contact add requested");
	return wechat_voip_service_add_contact(open_id);
}

esp_err_t app_wechat_remove_contact(const char *open_id)
{
	if (open_id == NULL || open_id[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGD(TAG, "wechat contact remove requested");
	return wechat_voip_service_remove_contact(open_id);
}

esp_err_t app_wechat_update_contact_remark(const char *open_id,
					   const char *remark)
{
	if (open_id == NULL || open_id[0] == '\0' || remark == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(open_id) >= APP_WECHAT_OPEN_ID_MAX ||
	    !wechat_voip_remark_is_valid(remark)) {
		return ESP_ERR_INVALID_SIZE;
	}
	if (app_get_active_app() != APP_ID_WECHAT || !network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	return wechat_voip_service_update_contact_remark(open_id, remark);
}

esp_err_t app_scan_wechat_contact(void)
{
	qr_scanner_contact_t contact = {0};

	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(qr_scanner_scan_contact(&contact), TAG, "scan wechat contact failed");
	if (contact.open_id[0] == '\0') {
		return ESP_ERR_INVALID_RESPONSE;
	}

	ESP_LOGD(TAG, "wechat contact QR accepted");
	return wechat_voip_service_add_contact(contact.open_id);
}

esp_err_t app_start_wechat_contact_scan(app_scan_preview_cb_t preview_cb,
					app_wechat_contact_scan_result_cb_t result_cb,
					void *ctx)
{
	esp_err_t ret = ESP_OK;
	app_id_t active_app = app_get_active_app();

	if (active_app != APP_ID_WECHAT) {
		ESP_LOGW(TAG, "start wechat contact scan rejected: active_app=%d", (int)active_app);
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_suspend_wechat_scan_resources();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start wechat contact scan failed while suspending resources: %s",
			 esp_err_to_name(ret));
		return ret;
	}

	s_wechat_contact_scan = (app_wechat_contact_scan_state_t){
		.preview_cb = preview_cb,
		.result_cb = result_cb,
		.ctx = ctx,
		.resources_suspended = true,
	};

	ret = qr_scanner_start_contact(app_wechat_scan_preview_cb, app_wechat_scan_result_cb, NULL);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start wechat contact scan failed while starting qr scanner: %s",
			 esp_err_to_name(ret));
		app_restore_wechat_scan_resources(true);
		s_wechat_contact_scan = (app_wechat_contact_scan_state_t){0};
	} else {
		ESP_LOGD(TAG, "wechat contact scan started");
	}
	return ret;
}

esp_err_t app_stop_wechat_contact_scan(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
		app_restore_wechat_scan_resources(true);
		s_wechat_contact_scan = (app_wechat_contact_scan_state_t){0};
	}
	return ret;
}

void app_cancel_wechat_contact_scan_for_lifecycle(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "cancel wechat contact scan failed: %s", esp_err_to_name(ret));
	}
	app_restore_wechat_scan_resources(false);
	s_wechat_contact_scan = (app_wechat_contact_scan_state_t){0};
}

esp_err_t app_wechat_hangup_call(void)
{
	return wechat_voip_service_reject_or_hangup();
}

esp_err_t app_wechat_accept_call(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_WECHAT) {
		ret = app_enter_app(APP_ID_WECHAT);
		if (ret != ESP_OK) {
			(void)wechat_voip_service_reject_or_hangup();
			return ret;
		}
	}

	ret = wechat_voip_service_answer();
	if (ret != ESP_OK) {
		(void)wechat_voip_service_reject_or_hangup();
	}
	return ret;
}

esp_err_t app_wechat_reject_call(void)
{
	return wechat_voip_service_reject_or_hangup();
}
