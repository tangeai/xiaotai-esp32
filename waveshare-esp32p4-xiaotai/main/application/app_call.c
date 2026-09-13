#include "app.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_internal.h"
#include "app_task_affinity.h"
#include "device_call.h"
#include "network.h"
#include "qr_scanner.h"
#include "rtc_transport.h"

static const char *TAG = "app_call";
static const char *CALL_FLOW_TAG = "CALL_FLOW";

#define APP_CALL_SCAN_RESTORE_TASK_STACK_SIZE 4096
#define APP_CALL_SCAN_RESTORE_TASK_PRIORITY   3
#define APP_CALL_START_TASK_STACK_SIZE        (12U * 1024U)
#define APP_CALL_START_TASK_PRIORITY          4
#define APP_CALL_START_CANCEL_POLL_MS         20U
#define APP_CALL_START_CANCEL_TIMEOUT_MS      5000U
#define APP_CALL_START_ERROR_VISIBLE_US       (3LL * 1000LL * 1000LL)
#define APP_CALL_HANGUP_TASK_STACK_SIZE       (8U * 1024U)
#define APP_CALL_HANGUP_TASK_PRIORITY         5

typedef enum {
	APP_CALL_START_NONE = 0,
	APP_CALL_START_OUTGOING,
	APP_CALL_START_ACCEPT,
} app_call_start_action_t;

typedef struct {
	uint32_t generation;
	bool running;
	bool cancel_requested;
	bool overlay_active;
	app_call_start_action_t action;
	app_call_type_t call_type;
	app_call_state_t ui_state;
	esp_err_t last_error;
	int64_t completed_at_us;
	char peer_device_id[APP_CALL_CONTACT_DEVICE_ID_MAX];
	char message[96];
} app_call_start_runtime_t;

typedef struct {
	app_scan_preview_cb_t preview_cb;
	app_contact_scan_result_cb_t result_cb;
	void *ctx;
	bool resources_suspended;
} app_contact_scan_state_t;

static app_contact_scan_state_t s_contact_scan;
static portMUX_TYPE s_call_start_lock = portMUX_INITIALIZER_UNLOCKED;
static app_call_start_runtime_t s_call_start;
static portMUX_TYPE s_call_hangup_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_call_hangup_queued;

static bool app_call_outgoing_state_ready(device_call_state_t *state_out)
{
	device_call_snapshot_t call = {0};

	device_call_get_snapshot(&call);
	if (state_out != NULL) {
		*state_out = call.state;
	}
	return call.state == DEVICE_CALL_STATE_IDLE ||
	       call.state == DEVICE_CALL_STATE_ERROR;
}

static void app_contact_scan_restore_task(void *arg)
{
	bool restore = (bool)(uintptr_t)arg;

	if (restore && app_get_active_app() == APP_ID_CALL) {
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "resume call resources after scan failed: %s", esp_err_to_name(ret));
		} else {
			ESP_LOGD(TAG, "contact scan resources resumed: %s", esp_err_to_name(ret));
		}
	}
	vTaskDeleteWithCaps(NULL);
}

static void app_restore_contact_scan_resources(bool restore)
{
	bool resources_suspended = s_contact_scan.resources_suspended;

	s_contact_scan.resources_suspended = false;
	if (restore && resources_suspended && app_get_active_app() == APP_ID_CALL) {
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "resume call resources after scan failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_defer_contact_scan_resources(bool restore)
{
	bool resources_suspended = s_contact_scan.resources_suspended;

	s_contact_scan.resources_suspended = false;
	if (!restore || !resources_suspended) {
		return;
	}

	BaseType_t task_ret = xTaskCreateWithCaps(app_contact_scan_restore_task,
						  "call_scan_res",
						  APP_CALL_SCAN_RESTORE_TASK_STACK_SIZE,
						  (void *)(uintptr_t)restore,
						  APP_CALL_SCAN_RESTORE_TASK_PRIORITY,
						  NULL,
						  APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		ESP_LOGW(TAG, "defer contact scan resource resume failed; resuming inline");
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "inline contact scan resource resume failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_contact_scan_preview_cb(const scan_preview_frame_t *frame,
					void *ctx)
{
	(void)ctx;

	if (s_contact_scan.preview_cb != NULL) {
		s_contact_scan.preview_cb(frame, s_contact_scan.ctx);
	}
}

static void app_contact_scan_result_cb(esp_err_t result,
				       const qr_scanner_contact_t *contact,
				       void *ctx)
{
	const char *device_id = "";
	const char *raw_payload = "";
	app_contact_scan_result_cb_t result_cb = s_contact_scan.result_cb;
	void *result_ctx = s_contact_scan.ctx;

	(void)ctx;

	if (contact != NULL && contact->raw_payload[0] != '\0') {
		raw_payload = contact->raw_payload;
	}
	if (result == ESP_OK) {
		if (contact == NULL ||
		    strlen(contact->device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
			result = ESP_ERR_INVALID_RESPONSE;
		} else {
			device_id = contact->device_id;
			ESP_LOGD(TAG, "contact QR accepted: device_id_len=%u", (unsigned)strlen(device_id));
		}
	}

	if (result_cb != NULL) {
		result_cb(result, device_id, raw_payload, result_ctx);
	}
	app_defer_contact_scan_resources(true);
	s_contact_scan = (app_contact_scan_state_t){0};
}

static app_call_start_runtime_t app_call_start_get_runtime(void)
{
	app_call_start_runtime_t runtime = {0};

	taskENTER_CRITICAL(&s_call_start_lock);
	runtime = s_call_start;
	taskEXIT_CRITICAL(&s_call_start_lock);
	return runtime;
}

bool app_call_start_is_in_progress(void)
{
	bool running = false;

	taskENTER_CRITICAL(&s_call_start_lock);
	running = s_call_start.running;
	taskEXIT_CRITICAL(&s_call_start_lock);
	return running;
}

static bool app_call_start_should_cancel(uint32_t generation)
{
	bool cancel = false;

	taskENTER_CRITICAL(&s_call_start_lock);
	cancel = !s_call_start.running ||
		 s_call_start.generation != generation ||
		 s_call_start.cancel_requested;
	taskEXIT_CRITICAL(&s_call_start_lock);
	return cancel || app_get_active_app() != APP_ID_CALL;
}

static bool app_call_start_commit_success(uint32_t generation)
{
	bool committed = false;

	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.running &&
	    s_call_start.generation == generation &&
	    !s_call_start.cancel_requested) {
		s_call_start.running = false;
		s_call_start.overlay_active = false;
		s_call_start.action = APP_CALL_START_NONE;
		s_call_start.last_error = ESP_OK;
		s_call_start.completed_at_us = 0;
		s_call_start.message[0] = '\0';
		committed = true;
	}
	taskEXIT_CRITICAL(&s_call_start_lock);
	return committed;
}

static void app_call_start_finish(uint32_t generation,
				  esp_err_t result,
				  bool canceled,
				  const char *message)
{
	int64_t completed_at_us = esp_timer_get_time();

	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.generation == generation) {
		canceled = canceled || s_call_start.cancel_requested;
		s_call_start.running = false;
		s_call_start.cancel_requested = false;
		s_call_start.action = APP_CALL_START_NONE;
		if (canceled || result == ESP_OK) {
			s_call_start.overlay_active = false;
			s_call_start.last_error = ESP_OK;
			s_call_start.completed_at_us = 0;
			s_call_start.message[0] = '\0';
		} else {
			s_call_start.overlay_active = true;
			s_call_start.ui_state = APP_CALL_STATE_ERROR;
			s_call_start.last_error = result;
			s_call_start.completed_at_us = completed_at_us;
			strlcpy(s_call_start.message,
				message != NULL ? message : "call start failed",
				sizeof(s_call_start.message));
		}
	}
	taskEXIT_CRITICAL(&s_call_start_lock);
}

static void app_call_start_cleanup(const app_call_start_runtime_t *runtime,
				   bool resources_acquired,
				   bool signalling_started)
{
	if (runtime == NULL) {
		return;
	}

	if (signalling_started) {
		esp_err_t hangup_ret = device_call_hangup();
		if (hangup_ret != ESP_OK && hangup_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "cancel call signalling failed: %s", esp_err_to_name(hangup_ret));
		}
	} else if (runtime->action == APP_CALL_START_ACCEPT &&
		   device_call_has_pending_incoming()) {
		esp_err_t reject_ret = device_call_reject_pending();
		if (reject_ret != ESP_OK && reject_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "reject unserviceable incoming call failed: %s",
				 esp_err_to_name(reject_ret));
		}
	}

	if (signalling_started) {
		rtc_transport_flush_remote_media();
		app_reset_rtc_call_media_state();
	}
	if (resources_acquired) {
		app_release_call_session_resources();
	}
	app_state_reset_call_media_policy();
}

static void app_call_start_task(void *arg)
{
	uint32_t generation = (uint32_t)(uintptr_t)arg;
	int64_t started_at_us = esp_timer_get_time();
	app_call_start_runtime_t runtime = app_call_start_get_runtime();
	bool resources_acquired = false;
	bool signalling_started = false;
	bool canceled = false;
	esp_err_t ret = ESP_OK;
	const char *failure_message = "call start failed";
	bool video = runtime.call_type == APP_CALL_TYPE_VIDEO;

	if (!runtime.running || runtime.generation != generation) {
		vTaskDeleteWithCaps(NULL);
		return;
	}

	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_call_worker_begin gen=%lu action=%s peer=%s type=%s",
		 (unsigned long)generation,
		 runtime.action == APP_CALL_START_ACCEPT ? "accept" : "outgoing",
		 runtime.peer_device_id[0] != '\0' ? runtime.peer_device_id : "-",
		 video ? "video" : "audio");

	app_cancel_contact_scan_for_lifecycle();
	if (app_call_start_should_cancel(generation)) {
		canceled = true;
		goto done;
	}
	if (runtime.action == APP_CALL_START_OUTGOING &&
	    !app_call_outgoing_state_ready(NULL)) {
		ret = ESP_ERR_INVALID_STATE;
		failure_message = "call already active";
		goto done;
	}

	app_state_prepare_call_media(video, true);
	ret = app_acquire_call_session_resources(video);
	if (ret != ESP_OK) {
		failure_message = "media initialization failed";
		goto done;
	}
	resources_acquired = true;
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_call_media_ready gen=%lu elapsed_ms=%llu",
		 (unsigned long)generation,
		 (unsigned long long)((esp_timer_get_time() - started_at_us) / 1000LL));

	if (app_call_start_should_cancel(generation)) {
		canceled = true;
		goto done;
	}
	if (runtime.action == APP_CALL_START_ACCEPT &&
	    !device_call_has_pending_incoming()) {
		canceled = true;
		goto done;
	}

	if (runtime.action == APP_CALL_START_ACCEPT) {
		ret = device_call_accept_pending();
		failure_message = "answer call failed";
	} else {
		ret = device_call_request(runtime.peer_device_id,
					  video ? DEVICE_CALL_TYPE_VIDEO : DEVICE_CALL_TYPE_AUDIO);
		failure_message = "call request failed";
	}
	if (ret != ESP_OK) {
		goto done;
	}
	signalling_started = true;
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_call_signalling_queued gen=%lu elapsed_ms=%llu",
		 (unsigned long)generation,
		 (unsigned long long)((esp_timer_get_time() - started_at_us) / 1000LL));

	if (runtime.action == APP_CALL_START_ACCEPT) {
		(void)app_state_sync_call_media_defaults(true, NULL);
		ret = app_apply_media_policy();
		if (ret != ESP_OK) {
			failure_message = "media policy failed";
			goto done;
		}
	}

	if (!app_call_start_commit_success(generation)) {
		canceled = true;
		goto done;
	}

	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_call_worker_done gen=%lu action=%s ret=ESP_OK",
		 (unsigned long)generation,
		 runtime.action == APP_CALL_START_ACCEPT ? "accept" : "outgoing");
	vTaskDeleteWithCaps(NULL);
	return;

done:
	if (canceled || ret != ESP_OK) {
		app_call_start_cleanup(&runtime, resources_acquired, signalling_started);
	}
	app_call_start_finish(generation, ret, canceled, failure_message);
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_call_worker_done gen=%lu action=%s canceled=%d ret=%s",
		 (unsigned long)generation,
		 runtime.action == APP_CALL_START_ACCEPT ? "accept" : "outgoing",
		 canceled ? 1 : 0,
		 esp_err_to_name(ret));
	vTaskDeleteWithCaps(NULL);
}

static esp_err_t app_call_start_async(app_call_start_action_t action,
				      const char *peer_device_id,
				      app_call_type_t call_type)
{
	uint32_t generation = 0;
	app_call_start_runtime_t next = {
		.running = true,
		.overlay_active = true,
		.action = action,
		.call_type = call_type,
		.ui_state = action == APP_CALL_START_ACCEPT ?
			APP_CALL_STATE_CONNECTING : APP_CALL_STATE_OUTGOING,
		.last_error = ESP_OK,
	};

	strlcpy(next.peer_device_id,
		peer_device_id != NULL ? peer_device_id : "",
		sizeof(next.peer_device_id));
	strlcpy(next.message,
		action == APP_CALL_START_ACCEPT ? "preparing answer" : "preparing call",
		sizeof(next.message));

	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.running) {
		taskEXIT_CRITICAL(&s_call_start_lock);
		return ESP_ERR_INVALID_STATE;
	}
	generation = s_call_start.generation + 1U;
	if (generation == 0U) {
		generation = 1U;
	}
	next.generation = generation;
	s_call_start = next;
	taskEXIT_CRITICAL(&s_call_start_lock);

	BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_call_start_task,
							      "call_start",
							      APP_CALL_START_TASK_STACK_SIZE,
							      (void *)(uintptr_t)generation,
							      APP_CALL_START_TASK_PRIORITY,
							      NULL,
							      APP_TASK_CORE_BACKGROUND,
							      /* Resource transitions load persisted audio settings and
							       * may enter cache-off NVS/codec paths. This control stack
							       * must remain accessible while PSRAM cache is disabled. */
							      APP_TASK_STACK_CAPS_CONTROL);
	if (task_ret != pdPASS) {
		app_call_start_finish(generation,
				      ESP_ERR_NO_MEM,
				      false,
				      "call worker unavailable");
		return ESP_ERR_NO_MEM;
	}

	return ESP_OK;
}

void app_call_start_apply_snapshot(app_call_snapshot_t *snapshot)
{
	app_call_start_runtime_t runtime = {0};
	int64_t now_us = esp_timer_get_time();

	if (snapshot == NULL) {
		return;
	}

	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.overlay_active &&
	    !s_call_start.running &&
	    s_call_start.completed_at_us > 0 &&
	    now_us - s_call_start.completed_at_us >= APP_CALL_START_ERROR_VISIBLE_US) {
		s_call_start.overlay_active = false;
	}
	runtime = s_call_start;
	taskEXIT_CRITICAL(&s_call_start_lock);

	if (!runtime.overlay_active) {
		return;
	}
	if (!runtime.running &&
	    snapshot->state != APP_CALL_STATE_IDLE &&
	    snapshot->state != APP_CALL_STATE_ERROR) {
		return;
	}

	snapshot->state = runtime.ui_state;
	snapshot->type = runtime.call_type;
	snapshot->pending_incoming = false;
	snapshot->last_error = runtime.last_error;
	if (runtime.peer_device_id[0] != '\0') {
		strlcpy(snapshot->peer_device_id,
			runtime.peer_device_id,
			sizeof(snapshot->peer_device_id));
	}
	strlcpy(snapshot->message, runtime.message, sizeof(snapshot->message));
}

esp_err_t app_cancel_pending_call_start_for_lifecycle(void)
{
	uint32_t generation = 0;
	TickType_t wait_started = xTaskGetTickCount();
	TickType_t timeout_ticks = pdMS_TO_TICKS(APP_CALL_START_CANCEL_TIMEOUT_MS);

	taskENTER_CRITICAL(&s_call_start_lock);
	if (!s_call_start.running) {
		s_call_start.overlay_active = false;
		s_call_start.cancel_requested = false;
		taskEXIT_CRITICAL(&s_call_start_lock);
		return ESP_OK;
	}
	s_call_start.cancel_requested = true;
	generation = s_call_start.generation;
	taskEXIT_CRITICAL(&s_call_start_lock);

	while (true) {
		bool running = false;

		taskENTER_CRITICAL(&s_call_start_lock);
		running = s_call_start.running && s_call_start.generation == generation;
		taskEXIT_CRITICAL(&s_call_start_lock);
		if (!running) {
			break;
		}
		if ((xTaskGetTickCount() - wait_started) >= timeout_ticks) {
			ESP_LOGE(TAG,
				 "call start cancellation timed out: generation=%lu timeout_ms=%u",
				 (unsigned long)generation,
				 (unsigned)APP_CALL_START_CANCEL_TIMEOUT_MS);
			return ESP_ERR_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(APP_CALL_START_CANCEL_POLL_MS));
	}

	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.generation == generation) {
		s_call_start.overlay_active = false;
		s_call_start.cancel_requested = false;
	}
	taskEXIT_CRITICAL(&s_call_start_lock);
	return ESP_OK;
}

esp_err_t app_call_contact(const char *device_id, app_call_type_t call_type)
{
    bool video = call_type == APP_CALL_TYPE_VIDEO;
    device_call_state_t call_state = DEVICE_CALL_STATE_IDLE;

    if (device_id == NULL ||
        strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
        (call_type != APP_CALL_TYPE_AUDIO && call_type != APP_CALL_TYPE_VIDEO)) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=app_call_rejected reason=invalid_peer");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_call_begin peer=%s active_app=%d network=%d",
             device_id,
             (int)app_get_active_app(),
             network_is_connected() ? 1 : 0);
    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_call_rejected peer=%s reason=wrong_app",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }
    if (!network_is_connected()) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_call_rejected peer=%s reason=wifi_offline",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }
	if (!app_call_outgoing_state_ready(&call_state)) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=app_call_rejected peer=%s state=%u reason=call_busy",
			 device_id,
			 (unsigned)call_state);
		return ESP_ERR_INVALID_STATE;
	}

    esp_err_t ret = app_call_start_async(APP_CALL_START_OUTGOING,
                                         device_id,
                                         call_type);
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_call_queued peer=%s type=%s ret=%s",
             device_id,
             video ? "video" : "audio",
             esp_err_to_name(ret));
    return ret;
}

esp_err_t app_scan_contact(void)
{
	qr_scanner_contact_t contact = {0};

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(qr_scanner_scan_contact(&contact), TAG, "scan contact failed");
	if (strlen(contact.device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
		return ESP_ERR_INVALID_RESPONSE;
	}

	ESP_LOGD(TAG, "contact QR accepted: device_id_len=%u", (unsigned)strlen(contact.device_id));
	return ESP_OK;
}

esp_err_t app_start_contact_scan(app_scan_preview_cb_t preview_cb,
				 app_contact_scan_result_cb_t result_cb,
				 void *ctx)
{
	esp_err_t ret = ESP_OK;
	app_id_t active_app = app_get_active_app();

	if (active_app != APP_ID_CALL) {
		ESP_LOGW(TAG, "start contact scan rejected: active_app=%d", (int)active_app);
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_suspend_call_scan_resources();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start contact scan failed while suspending resources: %s", esp_err_to_name(ret));
		return ret;
	}

	s_contact_scan = (app_contact_scan_state_t){
		.preview_cb = preview_cb,
		.result_cb = result_cb,
		.ctx = ctx,
		.resources_suspended = true,
	};

	ret = qr_scanner_start_contact(app_contact_scan_preview_cb, app_contact_scan_result_cb, NULL);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start contact scan failed while starting qr scanner: %s", esp_err_to_name(ret));
		app_restore_contact_scan_resources(true);
		s_contact_scan = (app_contact_scan_state_t){0};
	} else {
		ESP_LOGD(TAG, "contact scan started");
	}
	return ret;
}

esp_err_t app_stop_contact_scan(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
		app_restore_contact_scan_resources(true);
		s_contact_scan = (app_contact_scan_state_t){0};
	}
	return ret;
}

void app_cancel_contact_scan_for_lifecycle(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "cancel contact scan failed: %s", esp_err_to_name(ret));
	}
	app_restore_contact_scan_resources(false);
	s_contact_scan = (app_contact_scan_state_t){0};
}

esp_err_t app_hangup_call(void)
{
	esp_err_t ret = ESP_OK;

    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_hangup_rejected reason=wrong_app active_app=%d",
                 (int)app_get_active_app());
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(CALL_FLOW_TAG, "stage=app_hangup_begin");
	taskENTER_CRITICAL(&s_call_start_lock);
	if (s_call_start.running) {
		s_call_start.cancel_requested = true;
		taskEXIT_CRITICAL(&s_call_start_lock);
		ESP_LOGI(CALL_FLOW_TAG, "stage=app_hangup_done source=call_start_worker ret=ESP_OK");
		return ESP_OK;
	}
	taskEXIT_CRITICAL(&s_call_start_lock);

	/* Device-call signalling is owned exclusively by the ThingConnect service. */
	ret = device_call_hangup();
	if (ret != ESP_OK) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=app_hangup_done source=thing_connect ret=%s",
			 esp_err_to_name(ret));
		return ret;
	}
	rtc_transport_flush_remote_media();

	app_reset_rtc_call_media_state();
    app_release_call_session_resources();
    app_state_reset_call_media_policy();
    ESP_LOGI(CALL_FLOW_TAG, "stage=app_hangup_done source=thing_connect ret=ESP_OK");
    return ESP_OK;
}

static void app_hangup_call_task(void *arg)
{
    (void)arg;
    esp_err_t ret = app_hangup_call();

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=app_hangup_worker_done ret=%s", esp_err_to_name(ret));
    }
    taskENTER_CRITICAL(&s_call_hangup_lock);
    s_call_hangup_queued = false;
    taskEXIT_CRITICAL(&s_call_hangup_lock);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t app_hangup_call_async(void)
{
    if (app_get_active_app() != APP_ID_CALL) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_call_hangup_lock);
    if (s_call_hangup_queued) {
        taskEXIT_CRITICAL(&s_call_hangup_lock);
        return ESP_OK;
    }
    s_call_hangup_queued = true;
    taskEXIT_CRITICAL(&s_call_hangup_lock);

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_hangup_call_task,
                                                          "call_hangup",
                                                          APP_CALL_HANGUP_TASK_STACK_SIZE,
                                                          NULL,
                                                          APP_CALL_HANGUP_TASK_PRIORITY,
                                                          NULL,
                                                          APP_TASK_CORE_BACKGROUND,
                                                          /* Hangup must remain schedulable when the
                                                           * internal heap is under media pressure. */
                                                          APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ret != pdPASS) {
        taskENTER_CRITICAL(&s_call_hangup_lock);
        s_call_hangup_queued = false;
        taskEXIT_CRITICAL(&s_call_hangup_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(CALL_FLOW_TAG, "stage=app_hangup_queued");
    return ESP_OK;
}

esp_err_t app_accept_call(void)
{
    bool video = true;
    device_call_snapshot_t call = {0};

    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_accept_rejected reason=wrong_app active_app=%d",
                 (int)app_get_active_app());
        return ESP_ERR_INVALID_STATE;
    }

	if (!device_call_has_pending_incoming()) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=app_accept_rejected source=thing_connect reason=no_pending_call");
		return ESP_ERR_INVALID_STATE;
	}

	device_call_get_snapshot(&call);
	video = strcmp(call.call_type, "video") == 0;
    ESP_LOGI(CALL_FLOW_TAG, "stage=app_accept_begin source=thing_connect type=%s",
		     video ? "video" : "audio");
    esp_err_t ret = app_call_start_async(APP_CALL_START_ACCEPT,
					 call.peer_device_id,
					 video ? APP_CALL_TYPE_VIDEO : APP_CALL_TYPE_AUDIO);
    ESP_LOGI(CALL_FLOW_TAG,
	     "stage=app_accept_queued source=thing_connect ret=%s",
             esp_err_to_name(ret));
    return ret;
}

esp_err_t app_reject_call(void)
{
	if (!device_call_has_pending_incoming()) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=app_reject_rejected source=thing_connect reason=no_pending_call");
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGI(CALL_FLOW_TAG, "stage=app_reject_begin source=thing_connect");
	esp_err_t ret = device_call_reject_pending();
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=app_reject_done source=thing_connect ret=%s",
		 esp_err_to_name(ret));
    if (ret == ESP_OK) {
        app_state_reset_call_media_policy();
    }
    return ret;
}
