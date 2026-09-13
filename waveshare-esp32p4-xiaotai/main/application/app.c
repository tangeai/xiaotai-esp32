#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include "app_ai_device_action.h"
#include "app_ai_chat_config.h"
#include "app_audio_config.h"
#include "app_audio_policy.h"
#include "app_config.h"
#include "app_internal.h"
#include "app_log_policy.h"
#include "app_memory_policy.h"
#include "app_rtc_config.h"
#include "app_ui.h"
#include "app_task_affinity.h"
#include "ai_chat.h"
#include "ai_chat_token.h"
#include "audio_device.h"
#include "camera_pipeline.h"
#include "call_video_renderer.h"
#include "device.h"
#include "device_binding.h"
#include "device_call.h"
#include "device_identity.h"
#include "device_online.h"
#include "display.h"
#include "media_dma_reserve.h"
#include "media_governor.h"
#include "media_sink.h"
#include "network.h"
#include "ota.h"
#include "platform_nvs_async.h"
#include "rtc_media_bridge.h"
#include "rtc_transport.h"
#include "sender_test.h"
#include "system_time.h"
#include "thing_mqtt_client.h"
#include "thing_service_registry.h"
#include "wechat_voip_config.h"
#include "wechat_voip_service.h"

#if APP_CONFIG_DEBUG_SCREEN_SERVER_ENABLE
#include "screen_debug_server.h"
#endif
#if CONFIG_APP_SERIAL_CALL_CLI_ENABLE
#include "serial_call_cli.h"
#endif

static const char *TAG = "app";
static const char *CALL_FLOW_TAG = "CALL_FLOW";
#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
static const char *MEDIA_HEALTH_TAG = "MEDIA";
#endif

static void app_log_performance_profile(void)
{
	esp_chip_info_t chip = {0};
	esp_chip_info(&chip);

	ESP_LOGI(TAG,
		 "performance profile: chip_rev=%u.%02u cores=%u cpu=%uMHz pm=off "
		 "flash=qio/%uMHz psram=hex/%uMHz/%uMB l2=%uKB sdio=4bit/%uMHz",
		 (unsigned)(chip.revision / 100U),
		 (unsigned)(chip.revision % 100U),
		 (unsigned)chip.cores,
		 (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
		 (unsigned)CONFIG_ESPTOOLPY_FLASHFREQ_VAL,
		 (unsigned)CONFIG_SPIRAM_SPEED,
		 (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024U * 1024U)),
		 (unsigned)(CONFIG_CACHE_L2_CACHE_SIZE / 1024U),
		 (unsigned)(CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ / 1000U));
}

static void *app_calloc_psram(size_t count, size_t size)
{
	return app_memory_calloc_psram(count, size);
}

#define APP_CONTROL_TASK_STACK_SIZE 8192
#define APP_CONTROL_TASK_PRIORITY   2
#define APP_CONTROL_QUEUE_LENGTH    4
#define APP_RTC_RECONFIGURE_REASON_MAX 32
#define APP_RTC_PREPARE_WAIT_BINDING_MS 5000U
#define APP_RTC_PREPARE_POLL_MS        100U
#define APP_LIFECYCLE_TASK_STACK_SIZE 6144
#define APP_LIFECYCLE_TASK_PRIORITY   4
#define APP_LIFECYCLE_QUEUE_LENGTH    4
#define APP_MEDIA_HEALTH_INTERVAL_MS 10000U
#define APP_MEMORY_WATERLINE_INTERVAL_MS 10000U
#define APP_RTC_VIDEO_ADAPT_INTERVAL_MS 1000U
#define APP_RUNTIME_SNAPSHOT_INTERVAL_MS 10000U
#define APP_RUNTIME_MONITOR_TASK_STACK_SIZE 4096
#define APP_RUNTIME_MONITOR_TASK_PRIORITY   1
#define APP_RTC_VIDEO_TGMP_FEEDBACK_WAIT_MS 3000U
#define APP_DEVICE_UNBIND_ACK_WAIT_MS       2000U
#define APP_THING_BOOTSTRAP_TASK_STACK_SIZE (12U * 1024U)
#define APP_THING_BOOTSTRAP_TASK_PRIORITY   2
#define APP_AI_CHAT_TOKEN_PREFETCH_DELAY_MS 200U
#define APP_AI_CHAT_TOKEN_PREFETCH_MIN_INTERVAL_MS 60000U
#define APP_AI_CHAT_TOKEN_PREFETCH_TASK_STACK_SIZE (8U * 1024U)
#define APP_AI_CHAT_TOKEN_PREFETCH_TASK_PRIORITY   1
#define APP_AI_CALL_RTC_READY_TIMEOUT_MS           3500U
#define APP_AI_CALL_RTC_READY_POLL_MS              20U
#define APP_LIFECYCLE_QUIESCE_TIMEOUT_MS            3500U
#define APP_LIFECYCLE_QUIESCE_POLL_MS               20U
#define APP_REALTIME_CAPTURE_CODEC_GAIN_PERCENT    78U
#define APP_REALTIME_CAPTURE_UPLOAD_UNITY_PERCENT  100U
#define APP_REALTIME_CAPTURE_UI_UNITY_PERCENT      80U
#define APP_REALTIME_CAPTURE_AUTO_GAIN_MAX_PERCENT 300U
#define APP_REALTIME_FAR_END_UPLOAD_GAIN_PERCENT   28U
#define APP_REALTIME_FAR_END_AUTO_GAIN_MAX_PERCENT 100U

typedef enum {
	APP_CONTROL_EVENT_SPEAKER_VOLUME = 1,
	APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE,
	APP_CONTROL_EVENT_RTC_RECONFIGURE,
	APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY,
	APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT,
	APP_CONTROL_EVENT_DEVICE_UNBIND,
	APP_CONTROL_EVENT_DEVICE_BINDING_REFRESH,
	APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED,
	APP_CONTROL_EVENT_DEVICE_ONLINE_READY,
	APP_CONTROL_EVENT_CALL_SESSION_ENDED,
	APP_CONTROL_EVENT_RTC_VIDEO_BITRATE_REQUIRED,
	APP_CONTROL_EVENT_LOCAL_AUDIO_SETTINGS,
} app_control_event_type_t;

typedef enum {
	APP_LIFECYCLE_EVENT_ENTER_APP = 1,
	APP_LIFECYCLE_EVENT_RETURN_HOME,
	APP_LIFECYCLE_EVENT_START_APP_SERVICES,
	APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT,
	APP_LIFECYCLE_EVENT_CALL_ACCEPT,
} app_lifecycle_event_type_t;

typedef struct {
	app_control_event_type_t type;
	uint8_t percent;
	char reason[APP_RTC_RECONFIGURE_REASON_MAX];
	char rtc_device_id[APP_RTC_CONFIG_TEXT_MAX];
	char rtc_device_secret[APP_RTC_CONFIG_TEXT_MAX];
	char rtc_client_id[APP_RTC_CONFIG_TEXT_MAX];
} app_control_event_t;

typedef struct {
	app_lifecycle_event_type_t type;
	app_id_t app_id;
	ai_chat_device_action_route_t call_route;
	app_call_type_t call_type;
	char call_target_id[AI_CHAT_DEVICE_ACTION_TARGET_MAX];
} app_lifecycle_event_t;

typedef struct {
	char reason[APP_RTC_RECONFIGURE_REASON_MAX];
} app_thing_bootstrap_context_t;

static portMUX_TYPE s_app_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_rtc_video_bitrate_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_audio_control_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_app_transition_mutex_buffer;
static SemaphoreHandle_t s_app_transition_mutex;
static QueueHandle_t s_app_control_queue;
static TaskHandle_t s_app_control_task;
static QueueHandle_t s_app_lifecycle_queue;
static TaskHandle_t s_app_lifecycle_task;
#if CONFIG_APP_MEMORY_WATERLINE_LOG || CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || \
	CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS || \
	CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE || CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
static TaskHandle_t s_app_runtime_monitor_task;
#endif
#if CONFIG_APP_MEMORY_WATERLINE_LOG
static bool s_memory_health_valid;
static app_memory_health_t s_memory_health;
static uint32_t s_memory_psram_alloc_failures;
#endif
static app_id_t s_active_app = APP_ID_HOME;
static uint32_t s_active_resources;
static bool s_door_open;
static bool s_rtc_runtime_initialized;
static bool s_rtc_runtime_init_in_progress;
static bool s_rtc_sdk_prepared;
static bool s_rtc_identity_conflict_handled;
static bool s_device_binding_control_pending;
static char s_rtc_identity_conflict_device_id[APP_RTC_CONFIG_TEXT_MAX];
static char s_rtc_identity_conflict_client_id[APP_RTC_CONFIG_TEXT_MAX];
static bool s_thing_bootstrap_running;
static bool s_thing_bootstrap_rerun_pending;
static char s_thing_bootstrap_rerun_reason[APP_RTC_RECONFIGURE_REASON_MAX];
static TaskHandle_t s_ai_chat_token_prefetch_task;
static int64_t s_ai_chat_token_last_prefetch_us;
static bool s_rtc_video_bitrate_pending;
static bool s_rtc_video_bitrate_event_queued;
static uint8_t s_rtc_video_bitrate_stream_id;
static uint32_t s_rtc_video_bitrate_target_bps;
static uint32_t s_rtc_video_bitrate_epoch = 1U;
static uint32_t s_rtc_video_bitrate_event_epoch;
static uint32_t s_rtc_video_bitrate_logged_epoch;
static TickType_t s_rtc_video_bitrate_wait_started_tick;
static bool s_rtc_video_bitrate_feedback_seen;
static bool s_rtc_video_bitrate_fallback_logged;
static bool s_audio_control_event_queued;
static bool s_speaker_volume_pending;
static bool s_capture_gain_pending;
static uint8_t s_pending_speaker_volume;
static uint8_t s_pending_capture_gain;

static void app_ai_chat_media_active_changed(bool active, void *ctx);
static esp_err_t app_sync_rtc_video_bitrate_control(void);
static esp_err_t app_restore_device_call_video_profile(void);

typedef struct {
	bool video_renderer_started;
	bool video_profile_applied;
	media_governor_video_config_t previous_video_config;
} app_call_resource_context_t;

static app_call_resource_context_t s_call_resources;
static app_call_resource_context_t s_wechat_call_resources;

static esp_err_t app_configure_thing_service_registry(void)
{
	const thing_service_registry_config_t config = {
		.discovery_url = APP_CONFIG_THING_SERVICE_DISCOVERY_URL,
		.device_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.voip_api_base = APP_CONFIG_WECHAT_VOIP_API_BASE,
		.ai_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.call_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.mqtt_uri = APP_CONFIG_DEVICE_BINDING_MQTT_URI,
		.tirtc_endpoint = APP_CONFIG_RTC_SERVICE_ENDPOINT,
	};

	return thing_service_registry_init(&config);
}

static void app_log_heap_snapshot(const char *stage)
{
#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
	app_memory_snapshot_t memory = {0};
	media_dma_reserve_snapshot_t dma_reserve = {0};

	app_memory_get_snapshot(&memory);
	media_dma_reserve_get_snapshot(&dma_reserve);

	ESP_LOGI(TAG,
		 "%s heap: internal_free=%u internal_largest=%u internal_min=%u dma_free=%u dma_largest=%u dma_min=%u "
		 "psram_free=%u psram_largest=%u psram_min=%u psram_fail=%u dma_escrow=%u/%u",
		 stage != NULL ? stage : "runtime",
		 (unsigned)memory.internal_free,
		 (unsigned)memory.internal_largest,
		 (unsigned)memory.internal_min_free,
		 (unsigned)memory.dma_free,
		 (unsigned)memory.dma_largest,
		 (unsigned)memory.dma_min_free,
		 (unsigned)memory.psram_free,
		 (unsigned)memory.psram_largest,
		 (unsigned)memory.psram_min_free,
		 (unsigned)memory.psram_alloc_failures,
		 (unsigned)dma_reserve.reserved_bytes,
		 (unsigned)dma_reserve.configured_bytes);
#else
	(void)stage;
#endif
}

#if CONFIG_APP_MEMORY_WATERLINE_LOG
static void app_monitor_memory_health(bool force_log)
{
	app_memory_snapshot_t memory = {0};
	app_memory_health_t health;
	bool health_changed;
	bool allocation_failed;
	const char *event;

	app_memory_get_snapshot(&memory);
	health = app_memory_classify(&memory);
	health_changed = s_memory_health_valid && health != s_memory_health;
	allocation_failed = s_memory_health_valid &&
		memory.psram_alloc_failures != s_memory_psram_alloc_failures;

	if (!force_log && s_memory_health_valid && !health_changed && !allocation_failed) {
		return;
	}

	if (!s_memory_health_valid) {
		event = "baseline";
	} else if (health_changed && health == APP_MEMORY_HEALTH_NORMAL) {
		event = "recovered";
	} else if (health_changed) {
		event = "pressure";
	} else {
		event = "allocation-failure";
	}

	if (health == APP_MEMORY_HEALTH_CRITICAL) {
		ESP_LOGE(TAG,
			 "memory waterline: event=%s level=%s int=%uK/%uK min=%uK dma=%uK/%uK min=%uK ps=%uK/%uK min=%uK failures=%u",
			 event,
			 app_memory_health_name(health),
			 (unsigned)(memory.internal_free / 1024U),
			 (unsigned)(memory.internal_largest / 1024U),
			 (unsigned)(memory.internal_min_free / 1024U),
			 (unsigned)(memory.dma_free / 1024U),
			 (unsigned)(memory.dma_largest / 1024U),
			 (unsigned)(memory.dma_min_free / 1024U),
			 (unsigned)(memory.psram_free / 1024U),
			 (unsigned)(memory.psram_largest / 1024U),
			 (unsigned)(memory.psram_min_free / 1024U),
			 (unsigned)memory.psram_alloc_failures);
	} else if (health == APP_MEMORY_HEALTH_WARNING || allocation_failed) {
		ESP_LOGW(TAG,
			 "memory waterline: event=%s level=%s int=%uK/%uK min=%uK dma=%uK/%uK min=%uK ps=%uK/%uK min=%uK failures=%u",
			 event,
			 app_memory_health_name(health),
			 (unsigned)(memory.internal_free / 1024U),
			 (unsigned)(memory.internal_largest / 1024U),
			 (unsigned)(memory.internal_min_free / 1024U),
			 (unsigned)(memory.dma_free / 1024U),
			 (unsigned)(memory.dma_largest / 1024U),
			 (unsigned)(memory.dma_min_free / 1024U),
			 (unsigned)(memory.psram_free / 1024U),
			 (unsigned)(memory.psram_largest / 1024U),
			 (unsigned)(memory.psram_min_free / 1024U),
			 (unsigned)memory.psram_alloc_failures);
	} else {
		ESP_LOGI(TAG,
			 "memory waterline: event=%s level=%s int=%uK/%uK min=%uK dma=%uK/%uK min=%uK ps=%uK/%uK min=%uK failures=%u",
			 event,
			 app_memory_health_name(health),
			 (unsigned)(memory.internal_free / 1024U),
			 (unsigned)(memory.internal_largest / 1024U),
			 (unsigned)(memory.internal_min_free / 1024U),
			 (unsigned)(memory.dma_free / 1024U),
			 (unsigned)(memory.dma_largest / 1024U),
			 (unsigned)(memory.dma_min_free / 1024U),
			 (unsigned)(memory.psram_free / 1024U),
			 (unsigned)(memory.psram_largest / 1024U),
			 (unsigned)(memory.psram_min_free / 1024U),
			 (unsigned)memory.psram_alloc_failures);
	}

	s_memory_health = health;
	s_memory_psram_alloc_failures = memory.psram_alloc_failures;
	s_memory_health_valid = true;
}
#endif

#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE
typedef struct {
	bool valid;
	int64_t sample_us;
	uint32_t camera_dropped_frames;
	uint32_t backpressure_events;
	uint32_t tx_video_frames;
	size_t tx_video_bytes;
	uint32_t tx_failures;
	uint32_t rx_video_frames;
	uint64_t rx_video_bytes;
	uint32_t decoded_video_frames;
	uint32_t converted_video_frames;
	uint32_t presented_video_frames;
	uint32_t dropped_video_frames;
	uint32_t conversion_dropped_video_frames;
	uint32_t decode_failures;
	uint32_t conversion_failures;
	uint64_t conversion_time_us;
	uint64_t conversion_pack_time_us;
	uint64_t conversion_ppa_time_us;
	uint64_t conversion_swap_time_us;
	uint32_t video_discontinuities;
	uint32_t video_input_overflows;
	uint32_t aec_process_frames;
	uint64_t aec_process_us_total;
} app_media_health_baseline_t;

static app_media_health_baseline_t s_media_health_baseline;

static void app_media_health_update_baseline(const camera_pipeline_metrics_t *camera,
					      const rtc_transport_stats_t *rtc,
					      const call_video_renderer_stats_t *renderer,
					      const audio_stats_t *audio,
					      uint32_t backpressure_events,
					      int64_t sample_us)
{
	s_media_health_baseline.valid = true;
	s_media_health_baseline.sample_us = sample_us;
	s_media_health_baseline.camera_dropped_frames = camera->dropped_frames;
	s_media_health_baseline.backpressure_events = backpressure_events;
	s_media_health_baseline.tx_video_frames = rtc->tx_video_frames;
	s_media_health_baseline.tx_video_bytes = rtc->tx_video_bytes;
	s_media_health_baseline.tx_failures = rtc->tx_failures;
	s_media_health_baseline.rx_video_frames = renderer->received_frames;
	s_media_health_baseline.rx_video_bytes = renderer->received_bytes;
	s_media_health_baseline.decoded_video_frames = renderer->decoded_frames;
	s_media_health_baseline.converted_video_frames = renderer->converted_frames;
	s_media_health_baseline.presented_video_frames = renderer->presented_frames;
	s_media_health_baseline.dropped_video_frames = renderer->dropped_frames;
	s_media_health_baseline.conversion_dropped_video_frames =
		renderer->conversion_dropped_frames;
	s_media_health_baseline.decode_failures = renderer->decode_failures;
	s_media_health_baseline.conversion_failures = renderer->conversion_failures;
	s_media_health_baseline.conversion_time_us = renderer->conversion_time_us;
	s_media_health_baseline.conversion_pack_time_us =
		renderer->conversion_pack_time_us;
	s_media_health_baseline.conversion_ppa_time_us =
		renderer->conversion_ppa_time_us;
	s_media_health_baseline.conversion_swap_time_us =
		renderer->conversion_swap_time_us;
	s_media_health_baseline.video_discontinuities = renderer->discontinuities;
	s_media_health_baseline.video_input_overflows = renderer->input_overflows;
	s_media_health_baseline.aec_process_frames = audio->aec_process_frames;
	s_media_health_baseline.aec_process_us_total = audio->aec_process_us_total;
}

static void app_monitor_media_health(void)
{
	rtc_transport_stats_t rtc = {0};
	camera_pipeline_metrics_t camera = {0};
	call_video_renderer_stats_t renderer = {0};
	media_sink_stats_t sink = {0};
	audio_stats_t audio = {0};
	network_state_t network = {0};
	const int64_t now_us = esp_timer_get_time();
	const uint32_t backpressure_events =
		media_governor_get_network_backpressure_count();

	rtc_transport_get_stats(&rtc);
	camera_pipeline_get_metrics(&camera);
	call_video_renderer_get_stats(&renderer);
	media_sink_get_stats(&sink);
	audio_device_get_stats(&audio);
	network_get_state(&network);

	const bool media_active =
		rtc.active_connection &&
		(rtc.call_active || camera.rtc_enabled ||
		 renderer.running || audio.capture_enabled || audio.speaker_enabled);
	if (!media_active) {
		memset(&s_media_health_baseline, 0, sizeof(s_media_health_baseline));
		const media_governor_network_sample_t inactive_sample = {0};
		(void)media_governor_update_auto_adaptation(&inactive_sample);
		return;
	}

	const bool counters_restarted =
		s_media_health_baseline.valid &&
		(camera.dropped_frames < s_media_health_baseline.camera_dropped_frames ||
		 backpressure_events < s_media_health_baseline.backpressure_events ||
		 rtc.tx_video_frames < s_media_health_baseline.tx_video_frames ||
		 rtc.tx_video_bytes < s_media_health_baseline.tx_video_bytes ||
		 rtc.tx_failures < s_media_health_baseline.tx_failures ||
		 renderer.received_frames < s_media_health_baseline.rx_video_frames ||
		 renderer.received_bytes < s_media_health_baseline.rx_video_bytes ||
		 renderer.decoded_frames < s_media_health_baseline.decoded_video_frames ||
		 renderer.converted_frames < s_media_health_baseline.converted_video_frames ||
		 renderer.presented_frames < s_media_health_baseline.presented_video_frames ||
		 renderer.dropped_frames < s_media_health_baseline.dropped_video_frames ||
		 renderer.conversion_dropped_frames <
			 s_media_health_baseline.conversion_dropped_video_frames ||
		 renderer.decode_failures < s_media_health_baseline.decode_failures ||
		 renderer.conversion_failures < s_media_health_baseline.conversion_failures ||
		 renderer.conversion_time_us < s_media_health_baseline.conversion_time_us ||
		 renderer.conversion_pack_time_us <
			 s_media_health_baseline.conversion_pack_time_us ||
		 renderer.conversion_ppa_time_us <
			 s_media_health_baseline.conversion_ppa_time_us ||
		 renderer.conversion_swap_time_us <
			 s_media_health_baseline.conversion_swap_time_us ||
		 renderer.discontinuities < s_media_health_baseline.video_discontinuities ||
		 renderer.input_overflows < s_media_health_baseline.video_input_overflows ||
		 audio.aec_process_frames < s_media_health_baseline.aec_process_frames ||
		 audio.aec_process_us_total < s_media_health_baseline.aec_process_us_total);
	if (!s_media_health_baseline.valid || counters_restarted) {
		app_media_health_update_baseline(&camera,
					       &rtc,
					       &renderer,
					       &audio,
					       backpressure_events,
					       now_us);
#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
		ESP_LOGI(MEDIA_HEALTH_TAG,
			 "M0 %ux%u@%u a=%d/%d/%d g=%u/%u r=%d",
			 (unsigned)camera.width,
			 (unsigned)camera.height,
			 (unsigned)camera.target_fps,
			 audio.aec_active ? 1 : 0,
			 audio.aec_reference_active ? 1 : 0,
			 audio.aec_near_end_detected ? 1 : 0,
			 (unsigned)audio.capture_upload_gain_percent,
			 (unsigned)audio.capture_auto_gain_max_percent,
			 (int)network.rssi);
#endif
		return;
	}

	uint32_t elapsed_ms =
		(uint32_t)((now_us - s_media_health_baseline.sample_us) / 1000LL);
	if (elapsed_ms == 0U) {
		elapsed_ms = 1U;
	}

	const uint32_t tx_frames =
		rtc.tx_video_frames - s_media_health_baseline.tx_video_frames;
	const size_t tx_bytes =
		rtc.tx_video_bytes - s_media_health_baseline.tx_video_bytes;
	const uint32_t camera_dropped_frames =
		camera.dropped_frames - s_media_health_baseline.camera_dropped_frames;
	const uint32_t backpressure_event_delta =
		backpressure_events - s_media_health_baseline.backpressure_events;
	const uint32_t tx_failures =
		rtc.tx_failures - s_media_health_baseline.tx_failures;
	const uint32_t rx_video_frames =
		renderer.received_frames - s_media_health_baseline.rx_video_frames;
	const uint64_t rx_video_bytes =
		renderer.received_bytes - s_media_health_baseline.rx_video_bytes;
	const uint32_t decoded_video_frames =
		renderer.decoded_frames - s_media_health_baseline.decoded_video_frames;
	const uint32_t converted_video_frames =
		renderer.converted_frames - s_media_health_baseline.converted_video_frames;
	const uint32_t presented_video_frames =
		renderer.presented_frames - s_media_health_baseline.presented_video_frames;
	const uint32_t dropped_video_frames =
		renderer.dropped_frames - s_media_health_baseline.dropped_video_frames;
	const uint32_t conversion_dropped_video_frames =
		renderer.conversion_dropped_frames -
		s_media_health_baseline.conversion_dropped_video_frames;
	const uint32_t decode_failures =
		renderer.decode_failures - s_media_health_baseline.decode_failures;
	const uint32_t conversion_failures =
		renderer.conversion_failures - s_media_health_baseline.conversion_failures;
	const uint64_t conversion_time_us =
		renderer.conversion_time_us - s_media_health_baseline.conversion_time_us;
	const uint32_t video_discontinuities =
		renderer.discontinuities - s_media_health_baseline.video_discontinuities;
	const uint32_t video_input_overflows =
		renderer.input_overflows - s_media_health_baseline.video_input_overflows;
	const uint32_t aec_frames =
		audio.aec_process_frames - s_media_health_baseline.aec_process_frames;
	const uint64_t aec_process_us =
		audio.aec_process_us_total - s_media_health_baseline.aec_process_us_total;
	const uint32_t tx_fps_x10 =
		(uint32_t)(((uint64_t)tx_frames * 10000ULL) / elapsed_ms);
	const uint32_t tx_bitrate_kbps =
		(uint32_t)(((uint64_t)tx_bytes * 8ULL) / elapsed_ms);
	const uint32_t rx_fps_x10 =
		(uint32_t)(((uint64_t)rx_video_frames * 10000ULL) / elapsed_ms);
	const uint32_t rx_bitrate_kbps =
		(uint32_t)((rx_video_bytes * 8ULL) / elapsed_ms);
	const uint32_t decoded_fps_x10 =
		(uint32_t)(((uint64_t)decoded_video_frames * 10000ULL) / elapsed_ms);
	const uint32_t converted_fps_x10 =
		(uint32_t)(((uint64_t)converted_video_frames * 10000ULL) / elapsed_ms);
	const uint32_t conversion_avg_us =
		converted_video_frames > 0U ?
			(uint32_t)(conversion_time_us / converted_video_frames) : 0U;
	const uint32_t presented_fps_x10 =
		(uint32_t)(((uint64_t)presented_video_frames * 10000ULL) / elapsed_ms);
	const uint32_t aec_avg_us =
		aec_frames > 0U ? (uint32_t)(aec_process_us / aec_frames) : 0U;

	const media_governor_network_sample_t network_sample = {
		.active = rtc.call_active &&
			  rtc.local_video_send_enabled &&
			  camera.rtc_enabled,
		.backpressure_active = media_governor_is_network_backpressured(),
		.backpressure_events = backpressure_event_delta,
		.target_fps = camera.target_fps,
		.camera_fps_x10 = camera.measured_fps_x10,
		.tx_fps_x10 = tx_fps_x10,
		.tx_failures = tx_failures,
		.tx_queue_depth = rtc.local_video_tx_queue_len,
		.send_buffer_used = rtc.send_buffer_used,
		.send_buffer_limit = rtc.send_buffer_limit,
		.wifi_rssi = network.rssi,
	};
	if (media_governor_update_auto_adaptation(&network_sample)) {
		camera_pipeline_on_rtc_video_config_changed();
	}

#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
	ESP_LOGI(MEDIA_HEALTH_TAG,
		 "M u=%u.%u/%uk d=%u.%u/%uk x=%u.%u c=%u.%u/%ums p=%u.%u "
		 "q=%u+%u dr=%u+%u+%u er=%u+%u rs=%u/%u aq=%u/%ums a=%d/%d/%d/%ums g=%u/%u r=%d",
		 (unsigned)(tx_fps_x10 / 10U),
		 (unsigned)(tx_fps_x10 % 10U),
		 (unsigned)tx_bitrate_kbps,
		 (unsigned)(rx_fps_x10 / 10U),
		 (unsigned)(rx_fps_x10 % 10U),
		 (unsigned)rx_bitrate_kbps,
		 (unsigned)(decoded_fps_x10 / 10U),
		 (unsigned)(decoded_fps_x10 % 10U),
		 (unsigned)(converted_fps_x10 / 10U),
		 (unsigned)(converted_fps_x10 % 10U),
		 (unsigned)((conversion_avg_us + 500U) / 1000U),
		 (unsigned)(presented_fps_x10 / 10U),
		 (unsigned)(presented_fps_x10 % 10U),
		 (unsigned)renderer.queue_depth,
		 (unsigned)renderer.conversion_queue_depth,
		 (unsigned)camera_dropped_frames,
		 (unsigned)dropped_video_frames,
		 (unsigned)conversion_dropped_video_frames,
		 (unsigned)decode_failures,
		 (unsigned)conversion_failures,
		 (unsigned)video_discontinuities,
		 (unsigned)video_input_overflows,
		 (unsigned)sink.audio_queue_len,
		 (unsigned)sink.audio_buffered_ms,
		 audio.aec_active ? 1 : 0,
		 audio.aec_reference_active ? 1 : 0,
		 audio.aec_near_end_detected ? 1 : 0,
		 (unsigned)((aec_avg_us + 500U) / 1000U),
		 (unsigned)audio.capture_upload_gain_percent,
		 (unsigned)audio.capture_effective_auto_gain_max_percent,
		 (int)network.rssi);
#else
	(void)camera_dropped_frames;
	(void)tx_bitrate_kbps;
	(void)rx_fps_x10;
	(void)rx_bitrate_kbps;
	(void)decoded_fps_x10;
	(void)converted_fps_x10;
	(void)conversion_avg_us;
	(void)presented_fps_x10;
	(void)dropped_video_frames;
	(void)conversion_dropped_video_frames;
	(void)decode_failures;
	(void)conversion_failures;
	(void)video_discontinuities;
	(void)video_input_overflows;
	(void)aec_avg_us;
#endif

	app_media_health_update_baseline(&camera,
				       &rtc,
				       &renderer,
				       &audio,
				       backpressure_events,
				       now_us);
}
#endif

#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
static void app_log_runtime_snapshot(void)
{
	app_memory_snapshot_t memory = {0};
	network_state_t network = {0};
	rtc_transport_stats_t rtc = {0};
	camera_pipeline_metrics_t camera = {0};
	media_sink_stats_t sink = {0};
	audio_stats_t audio = {0};
	media_dma_reserve_snapshot_t dma_reserve = {0};
	UBaseType_t control_stack_hwm = s_app_control_task != NULL ?
		uxTaskGetStackHighWaterMark(s_app_control_task) : 0;
	UBaseType_t lifecycle_stack_hwm = s_app_lifecycle_task != NULL ?
		uxTaskGetStackHighWaterMark(s_app_lifecycle_task) : 0;
	UBaseType_t monitor_stack_hwm = s_app_runtime_monitor_task != NULL ?
		uxTaskGetStackHighWaterMark(s_app_runtime_monitor_task) : 0;

	app_memory_get_snapshot(&memory);
	network_get_state(&network);
	rtc_transport_get_stats(&rtc);
	camera_pipeline_get_metrics(&camera);
	media_sink_get_stats(&sink);
	audio_device_get_stats(&audio);
	media_dma_reserve_get_snapshot(&dma_reserve);

	ESP_LOGI(TAG,
		 "runtime snapshot: internal_free=%u internal_largest=%u internal_min=%u dma_free=%u dma_largest=%u dma_min=%u "
		 "psram_free=%u psram_largest=%u psram_min=%u psram_fail=%u "
		 "camera=%d rtc=%d %ux%u fps=%u.%u bitrate=%ukbps drop=%u enc_fail=%u direct=%d "
		 "video_pool=%u video_slot=%u video_q=%u free=%u audio_pool=%u audio_tx_q=%u audio_free=%u audio_rx_q=%u audio_buf_ms=%u "
		 "wifi=%d rssi=%d rtc_sendbuf=%u audio_cap=%d speaker=%d dma_escrow=%u/%u rel=%u rec=%u fail=%u "
		 "task_hwm=ctrl:%u,life:%u,monitor:%u",
		 (unsigned)memory.internal_free,
		 (unsigned)memory.internal_largest,
		 (unsigned)memory.internal_min_free,
		 (unsigned)memory.dma_free,
		 (unsigned)memory.dma_largest,
		 (unsigned)memory.dma_min_free,
		 (unsigned)memory.psram_free,
		 (unsigned)memory.psram_largest,
		 (unsigned)memory.psram_min_free,
		 (unsigned)memory.psram_alloc_failures,
		 camera.running ? 1 : 0,
		 camera.rtc_enabled ? 1 : 0,
		 (unsigned)camera.width,
		 (unsigned)camera.height,
		 (unsigned)(camera.measured_fps_x10 / 10U),
		 (unsigned)(camera.measured_fps_x10 % 10U),
		 (unsigned)camera.measured_bitrate_kbps,
		 (unsigned)camera.dropped_frames,
		 (unsigned)camera.encode_failures,
		 camera.direct_input ? 1 : 0,
		 (unsigned)rtc.local_video_tx_pool_capacity,
		 (unsigned)rtc.local_video_tx_largest_slot,
		 (unsigned)rtc.local_video_tx_queue_len,
		 (unsigned)rtc.local_video_tx_free_slots,
		 (unsigned)rtc.local_audio_tx_pool_capacity,
		 (unsigned)rtc.local_audio_tx_queue_len,
		 (unsigned)rtc.local_audio_tx_free_slots,
		 (unsigned)sink.audio_queue_len,
		 (unsigned)sink.audio_buffered_ms,
		 network.connected ? 1 : 0,
		 (int)network.rssi,
		 (unsigned)rtc.send_buffer_used,
		 audio.capture_enabled ? 1 : 0,
		 audio.speaker_enabled ? 1 : 0,
		 (unsigned)dma_reserve.reserved_bytes,
		 (unsigned)dma_reserve.configured_bytes,
		 (unsigned)dma_reserve.release_count,
		 (unsigned)dma_reserve.reclaim_count,
		 (unsigned)dma_reserve.reserve_fail_count,
		 (unsigned)control_stack_hwm,
		 (unsigned)lifecycle_stack_hwm,
		 (unsigned)monitor_stack_hwm);
}
#endif

static void app_preload_persistent_state(void)
{
	app_rtc_config_snapshot_t rtc_settings = {0};
	app_call_contacts_snapshot_t call_contacts = {0};

	app_get_rtc_config_snapshot(&rtc_settings);
	app_get_call_contacts(&call_contacts);
	ESP_LOGI(TAG,
		 "persistent state preloaded: rtc_device_id_len=%u call_contacts=%u",
		 (unsigned)strlen(rtc_settings.device_id),
		 (unsigned)call_contacts.count);
}

static esp_err_t app_set_speaker_volume_internal(uint8_t percent, bool persist);
static void app_apply_pending_audio_settings(bool marker_consumed);
static esp_err_t app_release_active_app_locked(app_id_t app_id);
static esp_err_t app_enter_app_locked(app_id_t app_id);
static esp_err_t app_return_home_locked(void);
static esp_err_t app_enter_app_sync(app_id_t app_id);
static esp_err_t app_return_home_sync(void);
static esp_err_t app_start_app_services(app_id_t app_id);
static esp_err_t app_enqueue_lifecycle_event(app_lifecycle_event_type_t type, app_id_t app_id);
static esp_err_t app_enqueue_ai_chat_call_lifecycle_event(
	ai_chat_device_action_route_t route,
	const char *target_id,
	app_call_type_t call_type);
static const char *app_id_name(app_id_t app_id);
static esp_err_t app_prepare_rtc_after_time_sync(const char *reason);
static esp_err_t app_prepare_rtc_after_config_if_ready(const char *reason);
static esp_err_t app_reconfigure_tirtc_after_settings_change(const char *reason);
static void app_request_rtc_reconfigure_after_settings_change(const char *reason);
static void app_request_rtc_prepare_after_identity(const char *reason);
static void app_request_rtc_identity_conflict(int error,
					      const char *device_id,
					      const char *client_id,
					      void *ctx);
static bool app_rtc_identity_conflict_mark_pending(const char *device_id, const char *client_id);
static void app_rtc_identity_conflict_clear_if_new_credentials(const char *device_id);
static bool app_wait_device_binding_before_rtc(const char *reason);
static void app_wait_identity_before_rtc_prepare(const char *reason);
static esp_err_t app_start_device_binding_reconcile_if_needed(const char *reason);
static esp_err_t app_start_device_identity_services(const char *reason);
static esp_err_t app_start_device_online_if_ready(const char *reason);
static void app_start_device_identity_ingress(void);
static esp_err_t app_configure_device_call(void);
static bool app_incoming_session_allowed(app_id_t incoming_app);
static bool app_device_call_can_accept_incoming(void *ctx);
static bool app_wechat_incoming_allowed(void *ctx);
static void app_device_call_session_ended(void *ctx);
static void app_release_call_session_resources_if_idle(void);
static void app_release_call_session_resources_internal(bool restore_video_profile);
static esp_err_t app_prepare_wechat_call_resources(bool local_video_enabled,
						   bool remote_video_enabled,
						   void *ctx);
static void app_release_wechat_call_resources(void *ctx);
static void app_release_wechat_call_resources_internal(bool leave_wechat_page);
static esp_err_t app_handle_device_unbind(const char *reason);
static void app_request_device_unbind(const char *reason);
static esp_err_t app_queue_device_binding_refresh(const char *reason);
static void app_clear_device_binding_control_pending(void);
static esp_err_t app_refresh_device_binding_internal(const char *reason);
static void app_device_online_ready_cb(void *ctx);
static void app_schedule_thing_bootstrap(const char *reason);
static void app_time_sync_cb(esp_err_t result, bool time_valid, void *ctx);
static esp_err_t app_reset_device_binding_internal(const char *reason);
static void app_request_ai_chat_start_if_idle(const char *reason);
static void app_schedule_ai_chat_token_prefetch(const char *reason);
static void app_reset_ai_chat_token_prefetch_throttle(void);

enum {
	APP_RESOURCE_RTC = 1U << 0,
	APP_RESOURCE_AUDIO = 1U << 1,
	APP_RESOURCE_CAMERA = 1U << 2,
};

typedef struct {
	app_id_t app_id;
	uint32_t resources;
} app_resource_profile_t;

#define APP_WECHAT_RESOURCE_MASK \
	(APP_RESOURCE_RTC | APP_RESOURCE_AUDIO | \
	 ((APP_CONFIG_WECHAT_VOIP_LOCAL_VIDEO_ENABLE != 0) ? APP_RESOURCE_CAMERA : 0U))

#define APP_AI_CHAT_RESOURCE_MASK \
	(APP_RESOURCE_RTC | APP_RESOURCE_AUDIO | \
	 ((APP_CONFIG_AI_CHAT_VIDEO_ENABLE != 0) ? APP_RESOURCE_CAMERA : 0U))

static const app_resource_profile_t s_app_resource_profiles[] = {
	{ APP_ID_HOME, 0 },
	{ APP_ID_DEVICE, 0 },
	{ APP_ID_CALL, 0 },
	{ APP_ID_WECHAT, APP_WECHAT_RESOURCE_MASK },
	{ APP_ID_AI_CHAT, APP_AI_CHAT_RESOURCE_MASK },
	{ APP_ID_SYSTEM, 0 },
};

static const char *app_id_name(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_DEVICE:
		return "device";
	case APP_ID_CALL:
		return "call";
	case APP_ID_WECHAT:
		return "wechat";
	case APP_ID_AI_CHAT:
		return "ai_chat";
	case APP_ID_SYSTEM:
		return "system";
	case APP_ID_HOME:
	default:
		return "home";
	}
}

static const char *app_ai_call_route_name(
	ai_chat_device_action_route_t route)
{
	switch (route) {
	case AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL:
		return "device";
	case AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP:
		return "wechat";
	case AI_CHAT_DEVICE_ACTION_ROUTE_NONE:
	default:
		return "none";
	}
}

static bool app_ai_call_target_valid(ai_chat_device_action_route_t route,
				     const char *target_id)
{
	if (target_id == NULL || target_id[0] == '\0') {
		return false;
	}
	if (route == AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL) {
		return strlen(target_id) == APP_CALL_CONTACT_DEVICE_ID_LENGTH;
	}
	if (route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP) {
		return strlen(target_id) < APP_WECHAT_OPEN_ID_MAX;
	}
	return false;
}

static esp_err_t app_wait_ai_call_rtc_ready(
	ai_chat_device_action_route_t route)
{
	uint32_t waited_ms = 0U;
	rtc_transport_state_t rtc_state = rtc_transport_get_state();

	while (rtc_state != RTC_TRANSPORT_STATE_READY &&
	       waited_ms < APP_AI_CALL_RTC_READY_TIMEOUT_MS) {
		if (rtc_state == RTC_TRANSPORT_STATE_ERROR) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(APP_AI_CALL_RTC_READY_POLL_MS));
		waited_ms += APP_AI_CALL_RTC_READY_POLL_MS;
		rtc_state = rtc_transport_get_state();
	}
	if (rtc_state != RTC_TRANSPORT_STATE_READY) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=rtc_not_ready state=%d wait_ms=%u",
			 app_ai_call_route_name(route),
			 (int)rtc_state,
			 (unsigned)waited_ms);
		return rtc_state == RTC_TRANSPORT_STATE_ERROR ?
			ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
	}

	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=ai_call_handoff_rtc_ready route=%s wait_ms=%u",
		 app_ai_call_route_name(route),
		 (unsigned)waited_ms);
	return ESP_OK;
}

static void app_handle_ai_chat_call_contact(
	ai_chat_device_action_route_t route,
	const char *target_id,
	app_call_type_t call_type)
{
	app_id_t target_app =
		route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
			APP_ID_WECHAT : APP_ID_CALL;
	esp_err_t ret = ESP_OK;

	if (!app_ai_call_target_valid(route, target_id)) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=invalid_target",
			 app_ai_call_route_name(route));
		return;
	}
	if (call_type != APP_CALL_TYPE_AUDIO &&
	    call_type != APP_CALL_TYPE_VIDEO) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=invalid_call_type",
			 app_ai_call_route_name(route));
		return;
	}
	if (s_app_transition_mutex == NULL ||
	    xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		ESP_LOGE(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=transition_lock",
			 app_ai_call_route_name(route));
		return;
	}

	if (app_get_active_app() != APP_ID_AI_CHAT) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=wrong_owner active=%s",
			 app_ai_call_route_name(route),
			 app_id_name(app_get_active_app()));
		ret = ESP_ERR_INVALID_STATE;
	} else {
		/*
		 * Keep release, RTC ownership transfer, target enter and call
		 * submission in one lifecycle transaction. P4 disconnect completion
		 * is asynchronous; the next owner must not submit a call while the
		 * shared TiRTC session is still DISCONNECTING.
		 */
		ESP_LOGI(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_begin route=%s target_len=%u",
			 app_ai_call_route_name(route),
			 (unsigned)strlen(target_id));
		ret = app_release_active_app_locked(APP_ID_AI_CHAT);
		if (ret == ESP_OK) {
			ret = app_wait_ai_call_rtc_ready(route);
		}
		if (ret == ESP_OK) {
			ret = app_enter_app_locked(target_app);
		}
		if (ret == ESP_OK) {
			ret = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
				app_wechat_call_contact_with_type(target_id, call_type) :
				app_call_contact(target_id, call_type);
		}
	}
	xSemaphoreGive(s_app_transition_mutex);

	if (ret == ESP_OK) {
		esp_err_t display_ret =
			route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
				display_open_wechat_active_page_async() :
				display_open_call_active_page_async();
		if (display_ret != ESP_OK) {
			ESP_LOGW(CALL_FLOW_TAG,
				 "stage=ai_call_page_failed route=%s page=active ret=%s",
				 app_ai_call_route_name(route),
				 esp_err_to_name(display_ret));
		}
	} else if (app_get_active_app() == target_app) {
		if (route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP) {
			(void)display_open_wechat_page_async();
		} else {
			(void)display_open_call_page_async();
		}
	}
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=ai_call_handoff_done route=%s ret=%s",
		 app_ai_call_route_name(route),
		 esp_err_to_name(ret));
}

static uint32_t app_resource_mask_for_app(app_id_t app_id)
{
	for (size_t index = 0; index < sizeof(s_app_resource_profiles) / sizeof(s_app_resource_profiles[0]); ++index) {
		if (s_app_resource_profiles[index].app_id == app_id) {
			return s_app_resource_profiles[index].resources;
		}
	}
	return 0;
}

app_id_t app_get_active_app(void)
{
	app_id_t app_id = APP_ID_HOME;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	app_id = s_active_app;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return app_id;
}

bool app_is_door_open(void)
{
	bool open = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	open = s_door_open;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return open;
}

static void app_set_active_app(app_id_t app_id)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_active_app = app_id;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	(void)device_online_report_state_async("app");
}

static uint32_t app_get_active_resources(void)
{
	uint32_t resources = 0;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	resources = s_active_resources;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return resources;
}

static void app_set_active_resources(uint32_t resources)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_active_resources = resources;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

esp_err_t app_configure_tirtc(void)
{
	rtc_transport_config_t *rtc_config = app_calloc_psram(1, sizeof(*rtc_config));
	if (rtc_config == NULL) {
		return ESP_ERR_NO_MEM;
	}

	esp_err_t ret = app_build_rtc_transport_config(rtc_config);
	if (ret == ESP_OK) {
		ret = rtc_transport_configure(rtc_config);
	}
	free(rtc_config);
	return ret;
}

static esp_err_t app_ai_chat_device_action_cb(
	const ai_chat_device_action_t *action,
	ai_chat_device_action_result_t *result,
	void *ctx)
{
	(void)ctx;
	if (result == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s_app_lifecycle_queue == NULL ||
	    s_app_transition_mutex == NULL ||
	    app_get_active_app() != APP_ID_AI_CHAT) {
		memset(result, 0, sizeof(*result));
		strlcpy(result->status, "busy", sizeof(result->status));
		strlcpy(result->message,
			"应用正在切换，请稍后再试",
			sizeof(result->message));
		return ESP_ERR_INVALID_STATE;
	}
	return app_ai_device_action_execute(action, result);
}

static esp_err_t app_ai_chat_device_action_committed_cb(
	const ai_chat_device_action_t *action,
	const ai_chat_device_action_result_t *result,
	void *ctx)
{
	(void)ctx;
	if (result == NULL || !result->ok ||
	    result->call_route == AI_CHAT_DEVICE_ACTION_ROUTE_NONE ||
	    result->target_id[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	app_call_type_t call_type =
		app_ai_device_action_requests_video(action) ?
			APP_CALL_TYPE_VIDEO : APP_CALL_TYPE_AUDIO;
	return app_enqueue_ai_chat_call_lifecycle_event(result->call_route,
							 result->target_id,
							 call_type);
}

static esp_err_t app_build_ai_chat_config(ai_chat_config_t *config)
{
	app_rtc_config_snapshot_t rtc_settings = {0};
	device_binding_identity_t identity = {0};

	if (config == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	app_get_rtc_config_snapshot(&rtc_settings);
	memset(config, 0, sizeof(*config));
	config->enabled = APP_CONFIG_AI_CHAT_ENABLE != 0;
	config->video_enabled = APP_CONFIG_AI_CHAT_VIDEO_ENABLE != 0;
	config->media_active_cb = app_ai_chat_media_active_changed;
	config->media_active_ctx = NULL;
	config->on_device_action = app_ai_chat_device_action_cb;
	config->on_device_action_committed =
		app_ai_chat_device_action_committed_cb;
	config->device_action_ctx = NULL;
	strlcpy(config->device_id, rtc_settings.device_id, sizeof(config->device_id));
	strlcpy(config->user_id, APP_CONFIG_AI_CHAT_USER_ID, sizeof(config->user_id));
	strlcpy(config->role_id, APP_CONFIG_AI_CHAT_ROLE_ID, sizeof(config->role_id));
	strlcpy(config->device_key, rtc_settings.device_secret, sizeof(config->device_key));
	strlcpy(config->token_api_base,
		thing_service_registry_ai_api_base(),
		sizeof(config->token_api_base));
	if (device_identity_get(&identity) == ESP_OK) {
		strlcpy(config->device_mac, identity.mac, sizeof(config->device_mac));
	}
	return ESP_OK;
}

static esp_err_t app_configure_ai_chat(void)
{
	ai_chat_config_t config = {0};

	ESP_RETURN_ON_ERROR(app_build_ai_chat_config(&config), TAG, "build ai chat config failed");
	return ai_chat_configure(&config);
}

static bool app_rtc_device_credentials_available(void)
{
	app_rtc_config_snapshot_t settings = {0};

	app_get_rtc_config_snapshot(&settings);
	return settings.device_id[0] != '\0' && settings.device_secret[0] != '\0';
}

static void app_ai_chat_token_prefetch_task(void *ctx)
{
	(void)ctx;
	vTaskDelay(pdMS_TO_TICKS(APP_AI_CHAT_TOKEN_PREFETCH_DELAY_MS));

	esp_err_t ret = ESP_ERR_INVALID_STATE;
	bool attempted = false;

	if (app_get_active_app() == APP_ID_HOME &&
	    network_is_connected() &&
	    system_time_has_valid_time() &&
	    app_rtc_device_credentials_available()) {
		ai_chat_config_t config = {0};
		ret = app_build_ai_chat_config(&config);
		if (ret == ESP_OK && config.enabled) {
			attempted = true;
			ret = ai_chat_token_prefetch_join(&config);
		}
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_FOUND) {
			ESP_LOGD(TAG, "AI Chat token prefetch skipped: %s", esp_err_to_name(ret));
		}
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (attempted && ret != ESP_OK) {
		s_ai_chat_token_last_prefetch_us = 0;
	}
	s_ai_chat_token_prefetch_task = NULL;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	vTaskDeleteWithCaps(NULL);
}

static void app_reset_ai_chat_token_prefetch_throttle(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_ai_chat_token_last_prefetch_us = 0;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static void app_schedule_ai_chat_token_prefetch(const char *reason)
{
	(void)reason;

	if (!network_is_connected() ||
	    !system_time_has_valid_time() ||
	    !app_rtc_device_credentials_available() ||
	    app_get_active_app() != APP_ID_HOME) {
		return;
	}

	int64_t now_us = esp_timer_get_time();
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	bool running = s_ai_chat_token_prefetch_task != NULL;
	bool throttled = s_ai_chat_token_last_prefetch_us > 0 &&
			 now_us - s_ai_chat_token_last_prefetch_us <
			     (int64_t)APP_AI_CHAT_TOKEN_PREFETCH_MIN_INTERVAL_MS * 1000LL;
	if (!running && !throttled) {
		s_ai_chat_token_last_prefetch_us = now_us;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (running || throttled) {
		return;
	}

	TaskHandle_t task = NULL;
	BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_ai_chat_token_prefetch_task,
							      "ai_tok_pref",
							      APP_AI_CHAT_TOKEN_PREFETCH_TASK_STACK_SIZE,
							      NULL,
							      APP_AI_CHAT_TOKEN_PREFETCH_TASK_PRIORITY,
							      &task,
							      APP_TASK_CORE_BACKGROUND,
							      APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_ai_chat_token_last_prefetch_us = 0;
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		return;
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_ai_chat_token_prefetch_task = task;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static bool app_rtc_identity_conflict_mark_pending(const char *device_id, const char *client_id)
{
	const char *safe_device_id = device_id != NULL ? device_id : "";
	const char *safe_client_id = client_id != NULL ? client_id : "";
	bool duplicate = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	duplicate = s_rtc_identity_conflict_handled &&
		    strcmp(s_rtc_identity_conflict_device_id, safe_device_id) == 0 &&
		    strcmp(s_rtc_identity_conflict_client_id, safe_client_id) == 0;
	if (!duplicate) {
		s_rtc_identity_conflict_handled = true;
		strlcpy(s_rtc_identity_conflict_device_id, safe_device_id, sizeof(s_rtc_identity_conflict_device_id));
		strlcpy(s_rtc_identity_conflict_client_id, safe_client_id, sizeof(s_rtc_identity_conflict_client_id));
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);

	return !duplicate;
}

static void app_rtc_identity_conflict_clear_if_new_credentials(const char *device_id)
{
	if (device_id == NULL || device_id[0] == '\0') {
		return;
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (s_rtc_identity_conflict_handled &&
	    strcmp(s_rtc_identity_conflict_device_id, device_id) != 0) {
		s_rtc_identity_conflict_handled = false;
		s_rtc_identity_conflict_device_id[0] = '\0';
		s_rtc_identity_conflict_client_id[0] = '\0';
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static esp_err_t app_device_binding_save_credentials(const char *device_id,
						     const char *device_key,
						     void *ctx)
{
	(void)ctx;

	if (device_id == NULL || device_key == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	return app_update_rtc_device_credentials(device_id, device_key);
}

static esp_err_t app_device_binding_load_credentials(device_binding_credentials_t *credentials,
						     void *ctx)
{
	app_rtc_config_snapshot_t settings = {0};

	(void)ctx;

	if (credentials == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	memset(credentials, 0, sizeof(*credentials));
	app_get_rtc_config_snapshot(&settings);
	if (settings.device_id[0] == '\0' || settings.device_secret[0] == '\0') {
		return ESP_ERR_NOT_FOUND;
	}

	strlcpy(credentials->device_id, settings.device_id, sizeof(credentials->device_id));
	strlcpy(credentials->device_key, settings.device_secret, sizeof(credentials->device_key));
	return ESP_OK;
}

static esp_err_t app_configure_device_binding(void)
{
	device_binding_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_device_api_base(),
		.mqtt_uri = thing_service_registry_mqtt_uri(),
		.wait_timeout_ms = APP_CONFIG_DEVICE_BINDING_WAIT_TIMEOUT_MS,
		.load_credentials = app_device_binding_load_credentials,
		.save_credentials = app_device_binding_save_credentials,
		.ctx = NULL,
	};

	return device_binding_init(&config);
}

static esp_err_t app_device_online_load_credentials(device_online_credentials_t *credentials,
						    void *ctx)
{
	app_rtc_config_snapshot_t settings = {0};

	(void)ctx;

	if (credentials == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	app_get_rtc_config_snapshot(&settings);
	if (settings.device_id[0] == '\0' || settings.device_secret[0] == '\0') {
		return ESP_ERR_NOT_FOUND;
	}

	strlcpy(credentials->device_id, settings.device_id, sizeof(credentials->device_id));
	strlcpy(credentials->device_key, settings.device_secret, sizeof(credentials->device_key));
	return ESP_OK;
}

static bool app_device_online_payload_is_unbind(const char *payload, size_t payload_len)
{
	bool unbind = false;
	cJSON *root = NULL;
	const cJSON *type = NULL;

	if (payload == NULL || payload_len == 0) {
		return false;
	}

	root = cJSON_ParseWithLength(payload, payload_len);
	if (root == NULL) {
		return false;
	}

	type = cJSON_GetObjectItemCaseSensitive(root, "type");
	unbind = cJSON_IsString(type) && strcmp(type->valuestring, "unbind") == 0;
	cJSON_Delete(root);
	return unbind;
}

static esp_err_t app_start_binding_with_retained_credentials(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "rebind";

	/* A retained-credential reconcile still needs a fresh signed Report session. */
	device_binding_reset_state(safe_reason);
	esp_err_t ret = device_binding_start_async(safe_reason);
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "start retained binding failed: %s", esp_err_to_name(ret));
	}
	return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static void app_device_online_message_cb(const char *topic,
					 const char *payload,
					 size_t payload_len,
					 void *ctx)
{
	(void)ctx;

	if (app_device_online_payload_is_unbind(payload, payload_len)) {
		ESP_LOGI(TAG, "device unbind command received");
		app_request_device_unbind("mqtt-unbind");
	}
}

static esp_err_t app_device_online_build_status(char *buffer,
						size_t buffer_size,
						const char *reason,
						uint32_t seq,
						void *ctx)
{
	network_state_t network = {0};
	rtc_transport_stats_t rtc = {0};
	app_rtc_config_snapshot_t rtc_settings = {0};
	device_online_snapshot_t online = {0};
	app_id_t active_app = app_get_active_app();
	bool door_open = app_is_door_open();
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "state";
	const char *payload_type = strcmp(safe_reason, "heartbeat") == 0 ? "heartbeat" : "status";

	(void)ctx;

	if (buffer == NULL || buffer_size == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	network_get_state(&network);
	rtc_transport_get_stats(&rtc);
	app_get_rtc_config_snapshot(&rtc_settings);
	device_online_get_snapshot(&online);

	int written = snprintf(buffer,
			       buffer_size,
			       "{\"type\":\"%s\",\"reason\":\"%s\",\"seq\":%lu,"
			       "\"ts\":%lld,\"app\":\"%s\",\"network\":%d,"
			       "\"mqtt\":%d,\"bound\":%d,\"ip\":\"%s\",\"rssi\":%d,"
			       "\"rtc_sdk\":%d,"
			       "\"rtc_connected\":%d,\"call_active\":%d,"
			       "\"incoming\":%d,\"door_open\":%d,"
			       "\"device_id\":\"%s\"}",
			       payload_type,
			       safe_reason,
			       (unsigned long)seq,
			       (long long)(esp_timer_get_time() / 1000000LL),
			       app_id_name(active_app),
			       network.connected ? 1 : 0,
			       online.mqtt_connected ? 1 : 0,
			       online.bound ? 1 : 0,
			       network.ip_addr,
			       (int)network.rssi,
			       rtc.sdk_initialized ? 1 : 0,
			       rtc.active_connection ? 1 : 0,
			       rtc.call_active ? 1 : 0,
			       rtc.incoming_call_pending ? 1 : 0,
			       door_open ? 1 : 0,
			       rtc_settings.device_id);
	if (written <= 0 || written >= (int)buffer_size) {
		buffer[0] = '\0';
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static void app_device_online_rebind_required_cb(void *ctx)
{
	(void)ctx;

	ESP_LOGI(TAG, "device token reset reported by server");
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device rebind event dropped: control queue not ready");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED,
	};
	strlcpy(event.reason, "token-reset", sizeof(event.reason));
	if (xQueueSendToBack(s_app_control_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG, "device rebind event dropped: control queue full");
	}
}

static void app_device_online_ready_cb(void *ctx)
{
	(void)ctx;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device online event dropped: control queue not ready");
		return;
	}

	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_ONLINE_READY,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG, "device online event dropped: control queue full");
	}
}

static esp_err_t app_handle_device_unbind(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "mqtt-unbind";
	esp_err_t ack_ret = thing_mqtt_client_wait_last_command_ack(APP_DEVICE_UNBIND_ACK_WAIT_MS);

	if (ack_ret == ESP_OK) {
		ESP_LOGI(TAG, "device unbind command ACK confirmed by broker");
	} else {
		ESP_LOGW(TAG,
			 "device unbind command ACK not confirmed before reset: ret=%s",
			 esp_err_to_name(ack_ret));
	}
	ESP_LOGI(TAG,
		 "device unbind command starts signed rebind with retained identity: reason=%s",
		 safe_reason);
	return app_reset_device_binding_internal(safe_reason);
}

static void app_request_device_unbind(const char *reason)
{
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device unbind event dropped: control queue not ready");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_UNBIND,
	};
	strlcpy(event.reason, reason != NULL ? reason : "mqtt-unbind", sizeof(event.reason));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "device unbind event dropped: queue full reason=%s", event.reason);
	}
}

static void app_clear_device_binding_control_pending(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_device_binding_control_pending = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static esp_err_t app_queue_device_binding_refresh(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "manual-refresh";
	bool already_pending = false;

	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_pending = s_device_binding_control_pending;
	if (!already_pending) {
		s_device_binding_control_pending = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_pending) {
		return ESP_OK;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_BINDING_REFRESH,
	};
	strlcpy(event.reason, safe_reason, sizeof(event.reason));
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		app_clear_device_binding_control_pending();
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static esp_err_t app_configure_device_online(void)
{
	device_online_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_device_api_base(),
		.mqtt_uri = thing_service_registry_mqtt_uri(),
		.heartbeat_interval_ms = 0,
		.load_credentials = app_device_online_load_credentials,
		.on_message = app_device_online_message_cb,
		.build_status = app_device_online_build_status,
		.on_rebind_required = app_device_online_rebind_required_cb,
		.on_online_ready = app_device_online_ready_cb,
		.ctx = NULL,
		.status_ctx = NULL,
		.rebind_ctx = NULL,
		.online_ready_ctx = NULL,
	};

	return device_online_init(&config);
}

static esp_err_t app_configure_device_call(void)
{
	const device_call_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_call_api_base(),
		.can_accept_incoming = app_device_call_can_accept_incoming,
		.on_session_ended = app_device_call_session_ended,
		.ctx = NULL,
	};

	return device_call_init(&config);
}

static bool app_device_call_can_accept_incoming(void *ctx)
{
	(void)ctx;
	return app_incoming_session_allowed(APP_ID_CALL);
}

static bool app_wechat_incoming_allowed(void *ctx)
{
	(void)ctx;
	return app_incoming_session_allowed(APP_ID_WECHAT);
}

static bool app_incoming_session_allowed(app_id_t incoming_app)
{
	app_id_t active_app = app_get_active_app();
	device_call_snapshot_t call = {0};
	wechat_voip_call_state_t wechat = wechat_voip_service_get_call_state();
	rtc_transport_stats_t rtc = {0};
	bool ai_busy = ai_chat_owns_control_button();
	bool call_busy = false;
	bool rtc_busy = false;

	device_call_get_snapshot(&call);
	rtc_transport_get_stats(&rtc);
	if (rtc.state == RTC_TRANSPORT_STATE_ERROR) {
		ESP_LOGW(TAG,
			 "incoming session rejected: incoming=%s reason=rtc_unavailable sdk_started=%d",
			 app_id_name(incoming_app),
			 rtc.sdk_started ? 1 : 0);
		return false;
	}
	call_busy = call.pending_incoming ||
		    call.state == DEVICE_CALL_STATE_OUTGOING ||
		    call.state == DEVICE_CALL_STATE_INCOMING ||
		    call.state == DEVICE_CALL_STATE_CONNECTING ||
		    call.state == DEVICE_CALL_STATE_IN_CALL;

	if ((incoming_app != APP_ID_AI_CHAT && ai_busy) ||
	    (incoming_app != APP_ID_CALL && call_busy) ||
	    (incoming_app != APP_ID_WECHAT && wechat != WECHAT_VOIP_CALL_STATE_IDLE)) {
		ESP_LOGW(TAG,
			 "incoming session rejected busy: incoming=%s owner=%s ai=%d call=%u pending=%d wechat=%u",
			 app_id_name(incoming_app),
			 app_id_name(active_app),
			 ai_busy ? 1 : 0,
			 (unsigned)call.state,
			 call.pending_incoming ? 1 : 0,
			 (unsigned)wechat);
		return false;
	}

	rtc_busy = rtc.active_connection || rtc.call_active || rtc.incoming_call_pending ||
		   rtc.state == RTC_TRANSPORT_STATE_CONNECTED ||
		   rtc.state == RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING ||
		   rtc.state == RTC_TRANSPORT_STATE_DISCONNECTING;
	if (rtc_busy && active_app != APP_ID_DEVICE && active_app != incoming_app) {
		ESP_LOGW(TAG,
			 "incoming session rejected by RTC owner: incoming=%s owner=%s state=%u active=%d call=%d pending=%d",
			 app_id_name(incoming_app),
			 app_id_name(active_app),
			 (unsigned)rtc.state,
			 rtc.active_connection ? 1 : 0,
			 rtc.call_active ? 1 : 0,
			 rtc.incoming_call_pending ? 1 : 0);
		return false;
	}
	return true;
}

static esp_err_t app_sync_rtc_video_bitrate_control(void)
{
#if CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
	media_governor_transport_bitrate_range_t range = {0};
	media_governor_get_transport_bitrate_range(&range);
	if (range.min_bitrate_bps == 0U ||
	    range.min_bitrate_bps >= range.start_bitrate_bps ||
	    range.start_bitrate_bps >= range.max_bitrate_bps) {
		return ESP_ERR_INVALID_STATE;
	}

	const rtc_transport_video_bitrate_params_t params = {
		.stream_id = RTC_TRANSPORT_LOCAL_VIDEO_STREAM_ID,
		.min_bitrate_bps = range.min_bitrate_bps,
		.max_bitrate_bps = range.max_bitrate_bps,
		.start_bitrate_bps = range.start_bitrate_bps,
	};
	return rtc_transport_set_video_bitrate_params(&params);
#else
	return rtc_transport_set_video_bitrate_params(NULL);
#endif
}

static void app_reset_rtc_video_transport_session(bool wait_for_feedback)
{
	bool video_config_changed = false;

	taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
	s_rtc_video_bitrate_epoch++;
	if (s_rtc_video_bitrate_epoch == 0U) {
		s_rtc_video_bitrate_epoch = 1U;
	}
	s_rtc_video_bitrate_pending = false;
	s_rtc_video_bitrate_event_queued = false;
	s_rtc_video_bitrate_target_bps = 0U;
	s_rtc_video_bitrate_event_epoch = 0U;
	s_rtc_video_bitrate_logged_epoch = 0U;
	s_rtc_video_bitrate_feedback_seen = false;
	s_rtc_video_bitrate_fallback_logged = false;
	s_rtc_video_bitrate_wait_started_tick =
		wait_for_feedback ? xTaskGetTickCount() : 0;
	taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);

	/* Never inherit a previous connection's reduced encoder rate. */
	esp_err_t reset_ret =
		media_governor_reset_transport_adaptation(true,
							 &video_config_changed);
	if (reset_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "reset transport video adaptation failed: %s",
			 esp_err_to_name(reset_ret));
	}

#if !CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
	(void)wait_for_feedback;
#endif

	if (video_config_changed) {
		camera_pipeline_on_rtc_video_config_changed();
	}
}

static void app_monitor_rtc_video_transport_compatibility(void)
{
#if CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
	const TickType_t now = xTaskGetTickCount();
	bool log_fallback = false;

	taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
	if (s_rtc_video_bitrate_wait_started_tick != 0 &&
	    !s_rtc_video_bitrate_feedback_seen &&
	    !s_rtc_video_bitrate_fallback_logged &&
	    now - s_rtc_video_bitrate_wait_started_tick >=
		    pdMS_TO_TICKS(APP_RTC_VIDEO_TGMP_FEEDBACK_WAIT_MS)) {
		s_rtc_video_bitrate_fallback_logged = true;
		log_fallback = true;
	}
	taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);

	if (log_fallback) {
		ESP_LOGI(TAG,
			 "TGMP feedback not observed; keep normal video profile");
	}
#endif
}

static void app_apply_pending_rtc_video_bitrate(bool marker_consumed)
{
	uint8_t stream_id = 0U;
	uint32_t target_bitrate_bps = 0U;
	uint32_t event_epoch = 0U;
	uint32_t current_epoch = 0U;
	bool pending = false;

	taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
	if (marker_consumed) {
		s_rtc_video_bitrate_event_queued = false;
	}
	if (s_rtc_video_bitrate_pending) {
		pending = true;
		stream_id = s_rtc_video_bitrate_stream_id;
		target_bitrate_bps = s_rtc_video_bitrate_target_bps;
		event_epoch = s_rtc_video_bitrate_event_epoch;
		s_rtc_video_bitrate_pending = false;
	}
	current_epoch = s_rtc_video_bitrate_epoch;
	taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);

	if (!pending ||
	    event_epoch != current_epoch ||
	    stream_id != RTC_TRANSPORT_LOCAL_VIDEO_STREAM_ID) {
		return;
	}

	rtc_transport_stats_t rtc = {0};
	rtc_transport_get_stats(&rtc);
	if (!rtc.active_connection ||
	    !rtc.call_active ||
	    !rtc.local_video_send_enabled) {
		return;
	}

	bool log_first_target = false;
	taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
	if (s_rtc_video_bitrate_logged_epoch != event_epoch) {
		s_rtc_video_bitrate_logged_epoch = event_epoch;
		log_first_target = true;
	}
	taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);
	if (log_first_target) {
		ESP_LOGI(TAG,
			 "TGMP bitrate feedback active: stream=%u first_target=%ukbps",
			 (unsigned)stream_id,
			 (unsigned)(target_bitrate_bps / 1000U));
	}

	bool changed = false;
	esp_err_t ret =
		media_governor_apply_transport_bitrate_target(target_bitrate_bps,
							     &changed);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "RTC video bitrate target apply failed: target=%ukbps ret=%s",
			 (unsigned)(target_bitrate_bps / 1000U),
			 esp_err_to_name(ret));
	} else if (changed) {
		camera_pipeline_on_rtc_video_config_changed();
	}
}

static void app_rtc_video_bitrate_required(tirtc_conn_t conn,
					   uint8_t stream_id,
					   uint32_t target_bitrate_bps,
					   void *ctx)
{
	(void)conn;
	(void)ctx;

	if (stream_id != RTC_TRANSPORT_LOCAL_VIDEO_STREAM_ID ||
	    target_bitrate_bps == 0U ||
	    s_app_control_queue == NULL) {
		return;
	}

	bool post_marker = false;
	taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
	s_rtc_video_bitrate_stream_id = stream_id;
	s_rtc_video_bitrate_target_bps = target_bitrate_bps;
	s_rtc_video_bitrate_event_epoch = s_rtc_video_bitrate_epoch;
	s_rtc_video_bitrate_pending = true;
	s_rtc_video_bitrate_feedback_seen = true;
	s_rtc_video_bitrate_wait_started_tick = 0;
	if (!s_rtc_video_bitrate_event_queued) {
		s_rtc_video_bitrate_event_queued = true;
		post_marker = true;
	}
	taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);

	if (!post_marker) {
		return;
	}

	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_VIDEO_BITRATE_REQUIRED,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		taskENTER_CRITICAL(&s_rtc_video_bitrate_lock);
		s_rtc_video_bitrate_event_queued = false;
		taskEXIT_CRITICAL(&s_rtc_video_bitrate_lock);
	}
}

static void app_device_call_session_ended(void *ctx)
{
	(void)ctx;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device call resource release dropped: control queue not ready");
		return;
	}

	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_CALL_SESSION_ENDED,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "device call resource release dropped: control queue full");
	}
}

static void app_apply_pending_audio_settings(bool marker_consumed)
{
	bool speaker_pending = false;
	bool capture_pending = false;
	uint8_t speaker_volume = 0U;
	uint8_t capture_gain = 0U;

	taskENTER_CRITICAL(&s_audio_control_lock);
	if (marker_consumed) {
		s_audio_control_event_queued = false;
	}
	speaker_pending = s_speaker_volume_pending;
	capture_pending = s_capture_gain_pending;
	speaker_volume = s_pending_speaker_volume;
	capture_gain = s_pending_capture_gain;
	s_speaker_volume_pending = false;
	s_capture_gain_pending = false;
	taskEXIT_CRITICAL(&s_audio_control_lock);

	if (speaker_pending) {
		esp_err_t ret = app_set_speaker_volume_internal(speaker_volume, true);
		if (ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "local speaker volume apply failed: volume=%u ret=%s",
				 (unsigned)speaker_volume,
				 esp_err_to_name(ret));
		}
	}
	if (capture_pending) {
		esp_err_t ret = app_set_capture_gain(capture_gain);
		if (ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "local capture gain apply failed: gain=%u ret=%s",
				 (unsigned)capture_gain,
				 esp_err_to_name(ret));
		}
	}
}

static void app_control_task(void *arg)
{
	(void)arg;
	app_control_event_t event = {0};

	while (true) {
		if (xQueueReceive(s_app_control_queue, &event, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		switch (event.type) {
		case APP_CONTROL_EVENT_SPEAKER_VOLUME:
		{
			esp_err_t ret = app_set_speaker_volume_internal(event.percent, false);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "remote speaker volume apply failed: volume=%u ret=%s",
					 event.percent,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "credential-update";
			esp_err_t ret = app_set_rtc_device_credentials(event.rtc_device_id, event.rtc_device_secret);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "rtc credential save failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
				break;
			}
			app_rtc_identity_conflict_clear_if_new_credentials(event.rtc_device_id);
			ai_chat_token_invalidate_cache();
			app_reset_ai_chat_token_prefetch_throttle();

			ESP_LOGD(TAG,
				 "rtc credential saved: reason=%s device_id_len=%u",
				 reason,
				 (unsigned)strlen(event.rtc_device_id));
			if (network_is_connected() && system_time_has_valid_time()) {
				device_online_set_network_ready(true);
			}
			wechat_voip_service_suspend_ingress();
			device_call_reset_identity_state();
			(void)device_online_notify_credentials_changed(reason);
			ret = app_reconfigure_tirtc_after_settings_change(reason);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "rtc config apply failed after credential save: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			} else {
				ESP_LOGD(TAG, "rtc config apply done after credential save: reason=%s", reason);
			}
			ESP_LOGD(TAG,
				 "rtc credential update worker stack_hwm=%u",
				 (unsigned)uxTaskGetStackHighWaterMark(NULL));
			break;
		}
		case APP_CONTROL_EVENT_RTC_RECONFIGURE:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "settings";
			ESP_LOGD(TAG, "rtc config apply begin: reason=%s", reason);
			esp_err_t ret = app_reconfigure_tirtc_after_settings_change(reason);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "rtc config apply failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			} else {
				ESP_LOGD(TAG, "rtc config apply done: reason=%s", reason);
			}
			break;
		}
		case APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "identity-ready";
			app_wait_identity_before_rtc_prepare(reason);
			break;
		}
		case APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT:
		{
			if (!app_rtc_identity_conflict_mark_pending(event.rtc_device_id, event.rtc_client_id)) {
				ESP_LOGW(TAG,
					 "rtc device ownership conflict already reported: device_id=%s physical_client_id=%s, binding preserved",
					 event.rtc_device_id,
					 event.rtc_client_id);
				break;
			}
			ESP_LOGW(TAG,
				 "rtc device ownership conflict: device_id=%s is registered to another client_id; physical_client_id=%s, preserve binding and wait for server mapping repair",
				 event.rtc_device_id,
				 event.rtc_client_id);
			(void)device_online_report_state_async("rtc-client-conflict");
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_UNBIND:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "mqtt-unbind";
			esp_err_t ret = app_handle_device_unbind(reason);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device unbind flow failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_BINDING_REFRESH:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "manual-refresh";
			esp_err_t ret = app_refresh_device_binding_internal(reason);
			app_clear_device_binding_control_pending();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device binding refresh failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "token-reset";
			esp_err_t ret = app_start_binding_with_retained_credentials(reason);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device rebind flow failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_ONLINE_READY:
		{
			app_id_t active_app = app_get_active_app();

			ESP_LOGI(TAG, "network recovery: formal MQTT online, resume TiRTC");
			app_wait_identity_before_rtc_prepare("mqtt-online");
			app_start_device_identity_ingress();
			if (active_app == APP_ID_AI_CHAT) {
				app_request_ai_chat_start_if_idle("mqtt-online");
			} else if (active_app == APP_ID_WECHAT) {
				(void)app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES,
								  active_app);
			}
			break;
		}
		case APP_CONTROL_EVENT_CALL_SESSION_ENDED:
			app_release_call_session_resources_if_idle();
			break;
		case APP_CONTROL_EVENT_RTC_VIDEO_BITRATE_REQUIRED:
			app_apply_pending_rtc_video_bitrate(true);
			break;
		case APP_CONTROL_EVENT_LOCAL_AUDIO_SETTINGS:
			app_apply_pending_audio_settings(true);
			break;
		default:
			ESP_LOGW(TAG, "unknown app control event: type=%u", (unsigned)event.type);
			break;
		}
		app_apply_pending_audio_settings(false);
		app_apply_pending_rtc_video_bitrate(false);
	}
}

static void app_lifecycle_task(void *arg)
{
	(void)arg;
	app_lifecycle_event_t event = {0};

	while (true) {
		if (xQueueReceive(s_app_lifecycle_queue, &event, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		esp_err_t ret = ESP_OK;
		esp_err_t display_ret = ESP_OK;
		switch (event.type) {
		case APP_LIFECYCLE_EVENT_ENTER_APP:
			ret = app_enter_app_sync(event.app_id);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "lifecycle enter failed: app=%s ret=%s",
					 app_id_name(event.app_id),
					 esp_err_to_name(ret));
				break;
			}
			switch (event.app_id) {
			case APP_ID_DEVICE:
				display_ret = display_open_device_page_async();
				break;
			case APP_ID_CALL:
				display_ret = display_open_call_page_async();
				break;
			case APP_ID_WECHAT:
				display_ret = display_open_wechat_page_async();
				break;
			case APP_ID_AI_CHAT:
				display_ret = display_open_ai_chat_page_async();
				break;
			case APP_ID_SYSTEM:
				display_ret = display_open_system_page_async();
				break;
			case APP_ID_HOME:
			default:
				display_ret = display_open_home_page_async();
				break;
			}
			if (display_ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "lifecycle page commit failed: app=%s ret=%s",
					 app_id_name(event.app_id),
					 esp_err_to_name(display_ret));
			}
			break;
		case APP_LIFECYCLE_EVENT_RETURN_HOME:
			ret = app_return_home_sync();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG, "lifecycle return home failed: %s", esp_err_to_name(ret));
				break;
			}
			display_ret = display_open_home_page_async();
			if (display_ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "lifecycle home page commit failed: %s",
					 esp_err_to_name(display_ret));
			}
			break;
		case APP_LIFECYCLE_EVENT_START_APP_SERVICES:
			if (app_get_active_app() == event.app_id) {
				ret = app_start_app_services(event.app_id);
				if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
					ESP_LOGW(TAG,
						 "lifecycle start service failed: app=%s ret=%s",
						 app_id_name(event.app_id),
						 esp_err_to_name(ret));
				}
			}
			break;
		case APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT:
			app_handle_ai_chat_call_contact(event.call_route,
							event.call_target_id,
							event.call_type);
			break;
		case APP_LIFECYCLE_EVENT_CALL_ACCEPT:
			/* Keep call resource acquisition and signaling off the LVGL task. */
			ret = app_enter_app_sync(event.app_id);
			if (ret == ESP_OK) {
				ret = app_accept_call();
			}
			if (ret != ESP_OK) {
				ESP_LOGW(CALL_FLOW_TAG,
					 "stage=queued_call_accept_failed ret=%s",
					 esp_err_to_name(ret));
			}
			break;
		default:
			ESP_LOGW(TAG, "unknown lifecycle event: type=%u", (unsigned)event.type);
			break;
		}
	}
}

static esp_err_t app_start_control_task(void)
{
	if (s_app_transition_mutex == NULL) {
		s_app_transition_mutex =
			xSemaphoreCreateMutexStatic(&s_app_transition_mutex_buffer);
		if (s_app_transition_mutex == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_control_queue == NULL) {
		s_app_control_queue = xQueueCreateWithCaps(APP_CONTROL_QUEUE_LENGTH,
							   sizeof(app_control_event_t),
							   APP_QUEUE_CAPS_CONTROL);
		if (s_app_control_queue == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_control_task == NULL) {
		BaseType_t task_ret = xTaskCreateWithCaps(app_control_task,
							  "app_ctrl",
							  APP_CONTROL_TASK_STACK_SIZE,
							  NULL,
							  APP_CONTROL_TASK_PRIORITY,
							  &s_app_control_task,
							  APP_TASK_STACK_CAPS_CONTROL);
		if (task_ret != pdPASS) {
			vQueueDeleteWithCaps(s_app_control_queue);
			s_app_control_queue = NULL;
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_lifecycle_queue == NULL) {
		s_app_lifecycle_queue = xQueueCreateWithCaps(APP_LIFECYCLE_QUEUE_LENGTH,
							     sizeof(app_lifecycle_event_t),
							     APP_QUEUE_CAPS_CONTROL);
		if (s_app_lifecycle_queue == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_lifecycle_task == NULL) {
		BaseType_t task_ret = xTaskCreateWithCaps(app_lifecycle_task,
							  "app_lifecycle",
							  APP_LIFECYCLE_TASK_STACK_SIZE,
							  NULL,
							  APP_LIFECYCLE_TASK_PRIORITY,
							  &s_app_lifecycle_task,
							  APP_TASK_STACK_CAPS_CONTROL);
		if (task_ret != pdPASS) {
			vQueueDeleteWithCaps(s_app_lifecycle_queue);
			s_app_lifecycle_queue = NULL;
			return ESP_ERR_NO_MEM;
		}
	}

	return ESP_OK;
}

static esp_err_t app_enqueue_lifecycle_event(app_lifecycle_event_type_t type, app_id_t app_id)
{
	if (s_app_lifecycle_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	app_lifecycle_event_t event = {
		.type = type,
		.app_id = app_id,
	};

	/*
	 * This queue serializes lifecycle ownership. Resetting it here would
	 * silently discard an already accepted app or AI-call transition.
	 */
	return xQueueSendToBack(s_app_lifecycle_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t app_enqueue_ai_chat_call_lifecycle_event(
	ai_chat_device_action_route_t route,
	const char *target_id,
	app_call_type_t call_type)
{
	if (s_app_lifecycle_queue == NULL ||
	    !app_ai_call_target_valid(route, target_id) ||
	    (call_type != APP_CALL_TYPE_AUDIO &&
	     call_type != APP_CALL_TYPE_VIDEO)) {
		return ESP_ERR_INVALID_STATE;
	}

	app_lifecycle_event_t event = {
		.type = APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT,
		.app_id = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
			APP_ID_WECHAT : APP_ID_CALL,
		.call_route = route,
		.call_type = call_type,
	};
	strlcpy(event.call_target_id,
		target_id,
		sizeof(event.call_target_id));

	/*
	 * A successful action response already promised this handoff. Wait for
	 * queue space instead of dropping the committed transition.
	 */
	return xQueueSendToBack(s_app_lifecycle_queue,
				&event,
				portMAX_DELAY) == pdTRUE ?
		ESP_OK : ESP_ERR_TIMEOUT;
}

static void app_request_rtc_reconfigure_after_settings_change(const char *reason)
{
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "rtc config saved; apply skipped because app control queue is not ready");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_RECONFIGURE,
	};
	strlcpy(event.reason, reason != NULL ? reason : "settings", sizeof(event.reason));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG,
			 "rtc config saved; apply event dropped because app control queue is full: reason=%s",
			 event.reason);
	}
}

static bool app_ai_chat_can_auto_start(void)
{
	return ai_chat_can_start();
}

static void app_request_ai_chat_start_if_idle(const char *reason)
{
	if (app_get_active_app() != APP_ID_AI_CHAT || !app_ai_chat_can_auto_start()) {
		return;
	}

	esp_err_t ret = app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG,
			 "queue AI Chat start failed: reason=%s ret=%s",
			 reason != NULL ? reason : "unknown",
			 esp_err_to_name(ret));
	}
}

static esp_err_t app_enqueue_speaker_volume(uint8_t percent)
{
	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	if (percent > 100U) {
		percent = 100U;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_SPEAKER_VOLUME,
		.percent = percent,
	};

	if (xQueueSendToBack(s_app_control_queue, &event, 0) == pdTRUE) {
		return ESP_OK;
	}

	ESP_LOGW(TAG, "speaker volume command dropped: control queue full volume=%u", (unsigned)percent);
	return ESP_ERR_TIMEOUT;
}

static esp_err_t app_enqueue_local_audio_setting(bool speaker, uint8_t percent)
{
	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	if (percent > 100U) {
		percent = 100U;
	}

	bool post_marker = false;
	taskENTER_CRITICAL(&s_audio_control_lock);
	if (speaker) {
		s_pending_speaker_volume = percent;
		s_speaker_volume_pending = true;
	} else {
		s_pending_capture_gain = percent;
		s_capture_gain_pending = true;
	}
	if (!s_audio_control_event_queued) {
		s_audio_control_event_queued = true;
		post_marker = true;
	}
	taskEXIT_CRITICAL(&s_audio_control_lock);

	if (!post_marker) {
		return ESP_OK;
	}

	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_LOCAL_AUDIO_SETTINGS,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, 0) == pdTRUE) {
		return ESP_OK;
	}

	taskENTER_CRITICAL(&s_audio_control_lock);
	s_audio_control_event_queued = false;
	taskEXIT_CRITICAL(&s_audio_control_lock);
	ESP_LOGW(TAG, "local audio setting deferred: control queue full");
	return ESP_ERR_TIMEOUT;
}

esp_err_t app_request_speaker_volume(uint8_t percent)
{
	return app_enqueue_local_audio_setting(true, percent);
}

esp_err_t app_request_capture_gain(uint8_t percent)
{
	return app_enqueue_local_audio_setting(false, percent);
}

static esp_err_t app_rtc_set_speaker_volume(uint8_t percent, void *ctx)
{
	(void)ctx;
	return app_enqueue_speaker_volume(percent);
}

static esp_err_t app_rtc_set_door_open(bool open, void *ctx)
{
	(void)ctx;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_door_open = open;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);

	ESP_LOGD(TAG, "door command accepted: state=%s hardware_driver=not_configured", open ? "open" : "locked");
	(void)device_online_report_state_async("door");
	return ESP_OK;
}

static bool app_rtc_media_uses_realtime_audio(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_HOME:
		/* Incoming IPC/device calls can become active before the UI changes page. */
	case APP_ID_DEVICE:
	case APP_ID_CALL:
	case APP_ID_WECHAT:
	case APP_ID_AI_CHAT:
		return true;
	case APP_ID_SYSTEM:
	default:
		return false;
	}
}

static void app_set_rtc_media_prepared(bool prepared)
{
	esp_err_t ret = app_audio_policy_set_source_prepared(
		APP_AUDIO_SOURCE_RTC_MEDIA,
		prepared);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "sync RTC audio preparation failed: prepared=%d ret=%s",
			 prepared ? 1 : 0,
			 esp_err_to_name(ret));
	}
}

static void app_set_ai_chat_media_prepared(bool prepared)
{
	esp_err_t ret = app_audio_policy_set_source_prepared(
		APP_AUDIO_SOURCE_AI_CHAT_MEDIA,
		prepared);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "sync AI Chat audio preparation failed: prepared=%d ret=%s",
			 prepared ? 1 : 0,
			 esp_err_to_name(ret));
	}
}

static void app_set_rtc_media_active(bool active, app_id_t active_app)
{
	esp_err_t ret = app_audio_policy_set_source_active(
		APP_AUDIO_SOURCE_RTC_MEDIA,
		active && app_rtc_media_uses_realtime_audio(active_app));
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "sync RTC realtime audio failed: active=%d app=%s ret=%s",
			 active ? 1 : 0,
			 app_id_name(active_app),
			 esp_err_to_name(ret));
	}
}

static void app_ai_chat_media_active_changed(bool active, void *ctx)
{
	(void)ctx;

	esp_err_t ret = app_audio_policy_set_source_active(
		APP_AUDIO_SOURCE_AI_CHAT_MEDIA,
		active);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "sync AI Chat realtime audio failed: active=%d ret=%s",
			 active ? 1 : 0,
			 esp_err_to_name(ret));
	}
}

void app_reset_rtc_call_media_state(void)
{
	(void)app_state_sync_call_media_defaults(false, NULL);
	app_set_rtc_media_active(false, APP_ID_HOME);
}

static void app_rtc_call_active_changed(bool active, void *ctx)
{
	app_control_state_t control = {0};
	app_id_t active_app = APP_ID_HOME;
	bool media_defaults_changed = false;

	(void)ctx;

	active_app = app_get_active_app();
	if (active && active_app == APP_ID_CALL &&
	    s_call_resources.video_profile_applied) {
		device_call_snapshot_t call = {0};

		device_call_get_snapshot(&call);
		if (!call.pending_incoming && call.state == DEVICE_CALL_STATE_IDLE) {
			/* The call page keeps its compact encoder profile warm between
			 * device calls. A generic listen-side connection can arrive while
			 * that page is still open; restore the saved IPC profile before the
			 * common RTC callback starts capture for that connection. */
			esp_err_t profile_ret = app_restore_device_call_video_profile();
			if (profile_ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "restore RTC profile for listen-side session failed: %s",
					 esp_err_to_name(profile_ret));
			}
		}
	}

	media_defaults_changed = app_state_sync_call_media_defaults(active, &control);
	app_set_rtc_media_active(active, active_app);
	if (active && active_app == APP_ID_WECHAT &&
	    APP_CONFIG_WECHAT_VOIP_LOCAL_VIDEO_ENABLE == 0) {
		app_state_set_video_enabled(false);
		control.video_enabled = false;
	}
	const bool wait_for_video_feedback =
		active && control.video_enabled && active_app != APP_ID_AI_CHAT;
	app_reset_rtc_video_transport_session(wait_for_video_feedback);
	if (!media_defaults_changed) {
		return;
	}

	ESP_LOGI(TAG,
		 "rtc call media defaults: active=%d video=%d audio=%d",
		 active ? 1 : 0,
		 control.video_enabled ? 1 : 0,
		 control.audio_enabled ? 1 : 0);

	if (active) {
		esp_err_t ret = app_apply_media_policy();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "apply media policy after rtc active failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_register_rtc_control_ops(void)
{
	const rtc_transport_control_ops_t ops = {
		.set_speaker_volume = app_rtc_set_speaker_volume,
		.set_door_open = app_rtc_set_door_open,
	};

	rtc_transport_set_control_ops(&ops, NULL);
}

static esp_err_t app_register_rtc_observer(void)
{
	const rtc_transport_observer_t observer = {
		.on_call_active = app_rtc_call_active_changed,
		.on_start_error = app_request_rtc_identity_conflict,
		.on_video_bitrate_required = app_rtc_video_bitrate_required,
	};

	return rtc_transport_register_observer(&observer, NULL);
}

static bool app_rtc_runtime_is_initialized(void)
{
	bool initialized = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	initialized = s_rtc_runtime_initialized;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return initialized;
}

static bool app_rtc_runtime_begin_init(void)
{
	bool should_init = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (!s_rtc_runtime_initialized && !s_rtc_runtime_init_in_progress) {
		s_rtc_runtime_init_in_progress = true;
		should_init = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return should_init;
}

static void app_rtc_runtime_finish_init(esp_err_t result)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (result == ESP_OK) {
		s_rtc_runtime_initialized = true;
	}
	s_rtc_runtime_init_in_progress = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static void app_rtc_sdk_set_prepared(bool prepared)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_rtc_sdk_prepared = prepared;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static bool app_rtc_sdk_is_prepared(void)
{
	bool prepared = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	prepared = s_rtc_sdk_prepared;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return prepared;
}

static esp_err_t app_init_rtc_transport(void)
{
	if (app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}
	if (!app_rtc_runtime_begin_init()) {
		return ESP_ERR_INVALID_STATE;
	}

	rtc_transport_config_t *rtc_config = app_calloc_psram(1, sizeof(*rtc_config));
	if (rtc_config == NULL) {
		app_rtc_runtime_finish_init(ESP_ERR_NO_MEM);
		return ESP_ERR_NO_MEM;
	}

	esp_err_t ret = app_build_rtc_transport_config(rtc_config);
	if (ret == ESP_OK) {
		ret = rtc_transport_init(rtc_config);
		if (ret == ESP_OK) {
			app_register_rtc_control_ops();
			ret = app_register_rtc_observer();
			if (ret == ESP_OK) {
				ret = app_sync_rtc_video_bitrate_control();
			}
		}
	}
	free(rtc_config);
	app_rtc_runtime_finish_init(ret);
	return ret;
}

static network_config_t app_make_network_config(void)
{
	network_config_t config = {
		.enabled = APP_CONFIG_WIFI_ENABLE != 0,
		.auto_connect = APP_CONFIG_WIFI_AUTO_CONNECT != 0,
		.default_ssid = APP_CONFIG_WIFI_SSID,
		.default_password = APP_CONFIG_WIFI_PASSWORD,
		.fallback_dns_ipv4 = APP_CONFIG_WIFI_FALLBACK_DNS_IPV4,
	};

	return config;
}

static ota_config_t app_make_ota_config(void)
{
	ota_config_t config = {
		.default_url = APP_CONFIG_OTA_DEFAULT_URL,
	};

	return config;
}

static esp_err_t app_start_network_baseline(void)
{
	network_config_t network_config = app_make_network_config();
	esp_err_t ret = network_prepare(&network_config);
	if (ret != ESP_OK) {
		return ret;
	}

	return ESP_OK;
}

static esp_err_t app_prepare_rtc_if_network_ready(void)
{
	if (!network_is_connected()) {
		return ESP_OK;
	}

	if (system_time_has_valid_time()) {
		if (!thing_service_registry_is_ready()) {
			app_schedule_thing_bootstrap("prepare");
			return ESP_ERR_INVALID_STATE;
		}
		esp_err_t identity_ret = app_start_device_identity_services("prepare");
		if (identity_ret != ESP_OK && identity_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "device identity start before rtc prepare failed: %s", esp_err_to_name(identity_ret));
		}
		if (!app_wait_device_binding_before_rtc("prepare")) {
			return ESP_ERR_INVALID_STATE;
		}
	}

	esp_err_t prepare_ret = app_prepare_rtc_after_time_sync("prepare");
	if (prepare_ret != ESP_OK) {
		if (prepare_ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGD(TAG, "rtc prepare waits for network time/sdk init");
		}
		return prepare_ret;
	}

	rtc_transport_network_state_t rtc_network = {
		.connected = true,
	};
	rtc_transport_on_network_state_changed(&rtc_network);
	return ESP_OK;
}

static esp_err_t app_prepare_rtc_after_time_sync(const char *reason)
{
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!system_time_has_valid_time()) {
		esp_err_t time_ret = system_time_request_sync(false);
		if (time_ret != ESP_OK) {
			ESP_LOGW(TAG, "schedule system time sync failed: %s", esp_err_to_name(time_ret));
		}
		return ESP_ERR_INVALID_STATE;
	}

#if APP_CONFIG_DEVICE_BINDING_ENABLE != 0
	device_online_snapshot_t online = {0};
	if (!device_online_is_online()) {
		device_online_get_snapshot(&online);
		ESP_LOGW(TAG,
			 "rtc prepare waits for ThingConnect online identity: reason=%s state=%d running=%d mqtt=%d",
			 reason != NULL ? reason : "time",
			 (int)online.state,
			 online.running ? 1 : 0,
			 online.mqtt_connected ? 1 : 0);
		return ESP_ERR_INVALID_STATE;
	}
#endif

	esp_err_t ret = app_init_rtc_transport();
	if (ret != ESP_OK) {
		return ret;
	}

	rtc_transport_network_state_t rtc_network = {
		.connected = true,
	};
	rtc_transport_on_network_state_changed(&rtc_network);

	bool was_prepared = app_rtc_sdk_is_prepared();
	ret = rtc_transport_prepare_sdk();
	if (ret == ESP_OK) {
		rtc_transport_stats_t rtc_stats = {0};
		rtc_transport_get_stats(&rtc_stats);
		if (!rtc_stats.sdk_initialized) {
			app_rtc_sdk_set_prepared(false);
			return ESP_ERR_INVALID_STATE;
		}
		app_rtc_sdk_set_prepared(true);
		if (!was_prepared) {
			ESP_LOGI(TAG, "rtc sdk initialized after time sync: reason=%s", reason != NULL ? reason : "time");
		}
		return ESP_OK;
	}
	if (ret != ESP_ERR_INVALID_STATE) {
		app_rtc_sdk_set_prepared(false);
		ESP_LOGW(TAG, "rtc sdk init after time sync failed: %s", esp_err_to_name(ret));
	}
	return ret;
}

static esp_err_t app_prepare_rtc_after_config_if_ready(const char *reason)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_OK;
	}

	esp_err_t ret = app_prepare_rtc_after_time_sync(reason != NULL ? reason : "settings");
	if (ret == ESP_ERR_INVALID_STATE) {
		return ESP_OK;
	}
	return ret;
}

static bool app_wait_device_binding_before_rtc(const char *reason)
{
	device_binding_snapshot_t binding = {0};
	uint32_t waited_ms = 0;

	if (!app_rtc_device_credentials_available()) {
		ESP_LOGD(TAG,
			 "rtc prepare skipped before binding: reason=%s",
			 reason != NULL ? reason : "identity-ready");
		return false;
	}

	while (waited_ms < APP_RTC_PREPARE_WAIT_BINDING_MS) {
		device_binding_get_snapshot(&binding);
		if (!binding.running ||
		    binding.state == DEVICE_BINDING_STATE_WAITING_USER ||
		    binding.state == DEVICE_BINDING_STATE_ERROR) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(APP_RTC_PREPARE_POLL_MS));
		waited_ms += APP_RTC_PREPARE_POLL_MS;
	}

	device_binding_get_snapshot(&binding);
	if (binding.running) {
		ESP_LOGW(TAG,
			 "rtc prepare waits for device binding reconciliation: reason=%s waited_ms=%u state=%d",
			 reason != NULL ? reason : "identity-ready",
			 (unsigned)waited_ms,
			 (int)binding.state);
		return false;
	}
	if (binding.state == DEVICE_BINDING_STATE_WAITING_USER) {
		ESP_LOGW(TAG,
			 "rtc prepare skipped until device binding completes: reason=%s",
			 reason != NULL ? reason : "identity-ready");
		return false;
	}
	if (binding.state == DEVICE_BINDING_STATE_ERROR) {
		ESP_LOGW(TAG,
			 "rtc prepare skipped after binding reconciliation error: reason=%s ret=%s",
			 reason != NULL ? reason : "identity-ready",
			 esp_err_to_name(binding.last_error));
		return false;
	}

	ESP_LOGD(TAG,
		 "rtc prepare binding gate passed: reason=%s state=%d waited_ms=%u",
		 reason != NULL ? reason : "identity-ready",
		 (int)binding.state,
		 (unsigned)waited_ms);
	return true;
}

static void app_wait_identity_before_rtc_prepare(const char *reason)
{
	device_online_snapshot_t online = {0};

	if (!app_wait_device_binding_before_rtc(reason)) {
		return;
	}

	device_online_get_snapshot(&online);
	ESP_LOGD(TAG,
		 "rtc prepare after identity gate: reason=%s online_state=%d online_running=%d mqtt=%d",
		 reason != NULL ? reason : "identity-ready",
		 (int)online.state,
		 online.running ? 1 : 0,
		 online.mqtt_connected ? 1 : 0);
	if (!device_online_is_online()) {
		ESP_LOGW(TAG,
			 "rtc prepare skipped: ThingConnect identity is not online reason=%s state=%d running=%d mqtt=%d",
			 reason != NULL ? reason : "identity-ready",
			 (int)online.state,
			 online.running ? 1 : 0,
			 online.mqtt_connected ? 1 : 0);
		return;
	}

	esp_err_t ret = app_prepare_rtc_after_time_sync(reason != NULL ? reason : "identity-ready");
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "prepare rtc after identity gate failed: %s", esp_err_to_name(ret));
	} else if (ret == ESP_OK) {
		app_schedule_ai_chat_token_prefetch(reason);
	}
}

static void app_request_rtc_prepare_after_identity(const char *reason)
{
	if (s_app_control_queue == NULL) {
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY,
	};
	strlcpy(event.reason, reason != NULL ? reason : "identity-ready", sizeof(event.reason));
	if (xQueueSendToBack(s_app_control_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG, "rtc prepare request dropped: reason=%s", event.reason);
	}
}

static void app_request_rtc_identity_conflict(int error,
					      const char *device_id,
					      const char *client_id,
					      void *ctx)
{
	(void)ctx;

	if (error != TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT) {
		return;
	}
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG,
			 "rtc identity conflict detected before control queue ready: device_id=%s client_id=%s",
			 device_id != NULL ? device_id : "",
			 client_id != NULL ? client_id : "");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT,
	};
	strlcpy(event.reason, "rtc-client-conflict", sizeof(event.reason));
	strlcpy(event.rtc_device_id, device_id != NULL ? device_id : "", sizeof(event.rtc_device_id));
	strlcpy(event.rtc_client_id, client_id != NULL ? client_id : "", sizeof(event.rtc_client_id));
	if (xQueueSendToBack(s_app_control_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG,
			 "rtc identity conflict event dropped: device_id=%s client_id=%s",
			 event.rtc_device_id,
			 event.rtc_client_id);
	}
}

static bool app_should_prepare_rtc_for_active_app(app_id_t app_id)
{
	(void)app_id;
	return true;
}

static void app_thing_bootstrap_task(void *arg)
{
	app_thing_bootstrap_context_t *context = (app_thing_bootstrap_context_t *)arg;
	char reason[APP_RTC_RECONFIGURE_REASON_MAX] = "thing-bootstrap";
	char rerun_reason[APP_RTC_RECONFIGURE_REASON_MAX] = {0};
	bool rerun_pending = false;

	if (context != NULL && context->reason[0] != '\0') {
		strlcpy(reason, context->reason, sizeof(reason));
	}

	if (network_is_connected() && system_time_has_valid_time()) {
		esp_err_t discovery_ret = thing_service_registry_refresh();
		if (discovery_ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "service discovery failed, using configured fallback endpoints: %s",
				 esp_err_to_name(discovery_ret));
		}
		esp_err_t call_endpoint_ret =
			device_call_set_api_base(thing_service_registry_call_api_base());
		if (call_endpoint_ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "device call endpoint update failed: %s",
				 esp_err_to_name(call_endpoint_ret));
		}

		if (network_is_connected() && system_time_has_valid_time()) {
			app_id_t active_app = app_get_active_app();
			bool manual_binding_refresh = strcmp(reason, "manual-refresh") == 0;
			esp_err_t identity_ret = manual_binding_refresh ?
				app_queue_device_binding_refresh(reason) :
				app_start_device_identity_services(reason);
			if (identity_ret != ESP_OK && identity_ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG,
					 "device identity start after service discovery failed: %s",
					 esp_err_to_name(identity_ret));
			}
			if (!manual_binding_refresh) {
				(void)device_online_report_state_async(reason);
				if (app_should_prepare_rtc_for_active_app(active_app)) {
					app_request_rtc_prepare_after_identity(reason);
				}
				if (active_app == APP_ID_AI_CHAT) {
					app_request_ai_chat_start_if_idle(reason);
				}
			}
		}
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	rerun_pending = s_thing_bootstrap_rerun_pending;
	if (rerun_pending) {
		strlcpy(rerun_reason,
			s_thing_bootstrap_rerun_reason[0] != '\0' ?
				s_thing_bootstrap_rerun_reason : "network-recovered",
			sizeof(rerun_reason));
	}
	s_thing_bootstrap_running = false;
	s_thing_bootstrap_rerun_pending = false;
	s_thing_bootstrap_rerun_reason[0] = '\0';
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	free(context);

	if (rerun_pending && network_is_connected() && system_time_has_valid_time()) {
		ESP_LOGI(TAG, "network recovery: service bootstrap rerun reason=%s", rerun_reason);
		app_schedule_thing_bootstrap(rerun_reason);
	}
	vTaskDeleteWithCaps(NULL);
}

static void app_schedule_thing_bootstrap(const char *reason)
{
	app_thing_bootstrap_context_t *context = NULL;
	bool already_running = false;

	if (!network_is_connected() || !system_time_has_valid_time()) {
		return;
	}

	context = app_calloc_psram(1, sizeof(*context));
	if (context == NULL) {
		ESP_LOGW(TAG, "service bootstrap context allocation failed");
		return;
	}
	strlcpy(context->reason,
		reason != NULL && reason[0] != '\0' ? reason : "thing-bootstrap",
		sizeof(context->reason));

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_running = s_thing_bootstrap_running;
	if (!already_running) {
		s_thing_bootstrap_running = true;
	} else {
		s_thing_bootstrap_rerun_pending = true;
		strlcpy(s_thing_bootstrap_rerun_reason,
			context->reason,
			sizeof(s_thing_bootstrap_rerun_reason));
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_running) {
		free(context);
		return;
	}

	BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_thing_bootstrap_task,
							     "thing_bootstrap",
							     APP_THING_BOOTSTRAP_TASK_STACK_SIZE,
							     context,
							     APP_THING_BOOTSTRAP_TASK_PRIORITY,
							     NULL,
							     APP_TASK_CORE_BACKGROUND,
							     APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_thing_bootstrap_running = false;
		s_thing_bootstrap_rerun_pending = false;
		s_thing_bootstrap_rerun_reason[0] = '\0';
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		free(context);
		ESP_LOGW(TAG, "service bootstrap task create failed");
	}
}

static void app_time_sync_cb(esp_err_t result, bool time_valid, void *ctx)
{
	(void)ctx;

	if (result != ESP_OK || !time_valid) {
		ESP_LOGW(TAG,
			 "system time sync callback: result=%s valid=%d",
			 esp_err_to_name(result),
			 time_valid ? 1 : 0);
		return;
	}

	app_schedule_thing_bootstrap("time-sync");
}

static esp_err_t app_acquire_rtc_resource(void)
{
	ESP_RETURN_ON_ERROR(app_apply_media_policy(), TAG, "apply media policy failed");
	return ESP_OK;
}

static void app_release_rtc_resource(void)
{
	rtc_transport_flush_remote_media();
	esp_err_t disconnect_ret = rtc_transport_disconnect();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "app lifecycle rtc disconnect failed: %s", esp_err_to_name(disconnect_ret));
	}
	rtc_transport_flush_remote_media();
	app_reset_rtc_call_media_state();
}

static bool app_rtc_session_needs_hangup(const rtc_transport_stats_t *stats)
{
	if (stats == NULL) {
		return false;
	}

	return stats->active_connection || stats->call_active || stats->incoming_call_pending ||
	       stats->state == RTC_TRANSPORT_STATE_CONNECTED ||
	       stats->state == RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING ||
	       stats->state == RTC_TRANSPORT_STATE_DISCONNECTING;
}

static esp_err_t app_hangup_rtc_session_if_active(app_id_t owner)
{
	rtc_transport_stats_t stats = {0};
	esp_err_t hangup_ret = ESP_OK;
	esp_err_t disconnect_ret = ESP_OK;

	if (!app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}

	rtc_transport_get_stats(&stats);
	if (!app_rtc_session_needs_hangup(&stats)) {
		return ESP_OK;
	}

	ESP_LOGI(TAG,
		 "app lifecycle hangup: app=%s state=%u active=%d call=%d incoming=%d",
		 app_id_name(owner),
		 (unsigned)stats.state,
		 stats.active_connection ? 1 : 0,
		 stats.call_active ? 1 : 0,
		 stats.incoming_call_pending ? 1 : 0);

	if (stats.active_connection && stats.state != RTC_TRANSPORT_STATE_DISCONNECTING) {
		hangup_ret = rtc_transport_hangup();
		if (hangup_ret != ESP_OK && hangup_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "app lifecycle hangup command failed: %s", esp_err_to_name(hangup_ret));
		}
	}
	rtc_transport_flush_remote_media();

	disconnect_ret = rtc_transport_disconnect();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "app lifecycle disconnect after hangup failed: %s", esp_err_to_name(disconnect_ret));
	}
	rtc_transport_flush_remote_media();

	app_reset_rtc_call_media_state();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		return disconnect_ret;
	}
	if (hangup_ret != ESP_OK && hangup_ret != ESP_ERR_INVALID_STATE) {
		return hangup_ret;
	}
	return ESP_OK;
}

static esp_err_t app_apply_realtime_capture_profile(uint8_t capture_gain_percent)
{
	uint32_t scaled_upload =
		((uint32_t)capture_gain_percent *
		 APP_REALTIME_CAPTURE_UPLOAD_UNITY_PERCENT +
		 APP_REALTIME_CAPTURE_UI_UNITY_PERCENT - 1U) /
		APP_REALTIME_CAPTURE_UI_UNITY_PERCENT;
	uint8_t upload_gain_percent =
		scaled_upload > 100U ? 100U : (uint8_t)scaled_upload;
	audio_capture_processing_config_t capture_config = {
		.send_volume_percent = capture_gain_percent,
		/* Keep the ES8311 ADC at the proven 23.4 dB point. */
		.codec_gain_percent = capture_gain_percent == 0U ?
			0U : APP_REALTIME_CAPTURE_CODEC_GAIN_PERCENT,
		/* Restore distant speech after AEC without changing its reference input. */
		.upload_gain_percent = upload_gain_percent,
		.auto_gain_max_percent = APP_REALTIME_CAPTURE_AUTO_GAIN_MAX_PERCENT,
		/* Hold residual echo down until the AEC adapter proves real double talk. */
		.far_end_gain_guard_enabled = true,
		.far_end_upload_gain_percent = APP_REALTIME_FAR_END_UPLOAD_GAIN_PERCENT,
		.far_end_auto_gain_max_percent =
			APP_REALTIME_FAR_END_AUTO_GAIN_MAX_PERCENT,
		.echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
		.high_pass_filter_enabled = true,
	};
	return microphone_set_processing_config(&capture_config);
}

static esp_err_t app_apply_realtime_audio_profile(void)
{
	app_audio_config_t audio_config = {0};
	ESP_RETURN_ON_ERROR(app_audio_config_load(&audio_config), TAG,
			    "load realtime audio config failed");

	ESP_RETURN_ON_ERROR(speaker_set_volume_percent(audio_config.speaker_volume_percent),
			    TAG,
			    "apply realtime speaker volume failed");
	ESP_RETURN_ON_ERROR(app_apply_realtime_capture_profile(
				audio_config.capture_gain_percent),
			    TAG,
			    "apply realtime capture profile failed");
	return ESP_OK;
}

static esp_err_t app_wait_shared_media_quiescent(app_id_t owner)
{
	uint32_t waited_ms = 0U;
	rtc_transport_stats_t rtc = {0};
	bool camera_running = false;

	if (owner == APP_ID_AI_CHAT) {
		esp_err_t ai_ret = ai_chat_wait_until_quiescent(
			APP_LIFECYCLE_QUIESCE_TIMEOUT_MS);
		if (ai_ret != ESP_OK) {
			app_log_heap_snapshot("AI lifecycle quiesce timeout");
			return ai_ret;
		}
	}

	while (true) {
		memset(&rtc, 0, sizeof(rtc));
		if (app_rtc_runtime_is_initialized()) {
			rtc_transport_get_stats(&rtc);
		}
		camera_running = camera_pipeline_is_running();
		bool rtc_busy = rtc.active_connection || rtc.call_active ||
				       rtc.incoming_call_pending ||
				       rtc.state == RTC_TRANSPORT_STATE_CONNECTED ||
				       rtc.state == RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING ||
				       rtc.state == RTC_TRANSPORT_STATE_DISCONNECTING;

		if (!rtc_busy && !camera_running) {
			if (waited_ms > 0U) {
				ESP_LOGI(TAG,
					 "app resources quiescent: owner=%s waited=%ums rtc_state=%d",
					 app_id_name(owner),
					 (unsigned)waited_ms,
					 (int)rtc.state);
			}
			return ESP_OK;
		}
		if (waited_ms >= APP_LIFECYCLE_QUIESCE_TIMEOUT_MS) {
			break;
		}

		vTaskDelay(pdMS_TO_TICKS(APP_LIFECYCLE_QUIESCE_POLL_MS));
		waited_ms += APP_LIFECYCLE_QUIESCE_POLL_MS;
	}

	ESP_LOGW(TAG,
		 "app resource quiesce timeout: owner=%s waited=%ums rtc_state=%d active=%d call=%d incoming=%d camera=%d",
		 app_id_name(owner),
		 (unsigned)waited_ms,
		 (int)rtc.state,
		 rtc.active_connection ? 1 : 0,
		 rtc.call_active ? 1 : 0,
		 rtc.incoming_call_pending ? 1 : 0,
		 camera_running ? 1 : 0);
	app_log_heap_snapshot("app lifecycle quiesce timeout");
	return ESP_ERR_TIMEOUT;
}

static esp_err_t app_acquire_audio_resource(void)
{
	ESP_RETURN_ON_ERROR(app_apply_realtime_audio_profile(), TAG,
			    "apply realtime audio profile failed");
	esp_err_t ret = audio_device_prepare();

	if (ret != ESP_OK) {
		return ret;
	}
	esp_err_t echo_ret = audio_device_prepare_echo_cancel();
	if (echo_ret != ESP_OK) {
		/* AEC is an enhancement: keep the call usable if it cannot reserve resources. */
		ESP_LOGW(TAG, "prepare call echo cancellation failed: %s", esp_err_to_name(echo_ret));
	}
	return ESP_OK;
}

static void app_release_audio_resource(void)
{
	(void)microphone_set_enabled(false);
	speaker_stop_playback();
	audio_device_release();
}

static esp_err_t app_acquire_camera_resource(void)
{
	/*
	 * APP_RESOURCE_CAMERA is a logical lease used to keep application
	 * lifecycles mutually exclusive. The active camera consumer owns the
	 * physical driver: qr_scanner while scanning, camera_pipeline while
	 * streaming. Taking a second driver reference here keeps the sensor open
	 * with the previous profile and prevents the consumer from applying its
	 * target before capture starts.
	 */
	return ESP_OK;
}

static void app_release_camera_resource(void)
{
	/* Physical camera lifetime is released by the active camera consumer. */
}

static esp_err_t app_acquire_resources(uint32_t resources)
{
	if ((resources & APP_RESOURCE_RTC) != 0U) {
		ESP_RETURN_ON_ERROR(app_acquire_rtc_resource(), TAG, "acquire rtc failed");
	}
	if ((resources & APP_RESOURCE_AUDIO) != 0U) {
		ESP_RETURN_ON_ERROR(app_acquire_audio_resource(), TAG, "acquire audio failed");
	}
	if ((resources & APP_RESOURCE_CAMERA) != 0U) {
		ESP_RETURN_ON_ERROR(app_acquire_camera_resource(), TAG, "acquire camera failed");
	}
	return ESP_OK;
}

static void app_release_resources(uint32_t resources)
{
	if ((resources & APP_RESOURCE_CAMERA) != 0U) {
		app_release_camera_resource();
	}
	if ((resources & APP_RESOURCE_AUDIO) != 0U) {
		app_release_audio_resource();
	}
	if ((resources & APP_RESOURCE_RTC) != 0U) {
		app_release_rtc_resource();
	}
}

static esp_err_t app_switch_resources(uint32_t target_resources)
{
	uint32_t current_resources = app_get_active_resources();
	uint32_t acquire_resources = target_resources & ~current_resources;
	uint32_t release_resources = current_resources & ~target_resources;

	esp_err_t ret = app_acquire_resources(acquire_resources);
	if (ret != ESP_OK) {
		app_release_resources(acquire_resources);
		return ret;
	}

	app_release_resources(release_resources);
	app_set_active_resources(target_resources);
	return ESP_OK;
}

static void app_network_state_cb(const network_state_t *state, void *ctx)
{
	(void)ctx;

	if (state == NULL) {
		return;
	}

	(void)device_online_report_state_async(state->connected ? "network-up" : "network-down");

	if (state->connected) {
		esp_err_t time_ret = system_time_request_sync(false);
		if (time_ret != ESP_OK) {
			ESP_LOGW(TAG, "schedule system time sync failed: %s", esp_err_to_name(time_ret));
		}
		if (system_time_has_valid_time()) {
			app_schedule_thing_bootstrap("network-ready");
		}
	} else {
		app_rtc_sdk_set_prepared(false);
		device_online_set_network_ready(false);
	}

	if (app_rtc_runtime_is_initialized()) {
		rtc_transport_network_state_t rtc_network = {
			.connected = state->connected,
		};
		rtc_transport_on_network_state_changed(&rtc_network);
	}

}

static bool app_rtc_test_video_active(void *ctx)
{
	(void)ctx;

	return sender_test_is_mode_active(SENDER_TEST_MODE_VIDEO);
}

static bool app_rtc_test_audio_active(void *ctx)
{
	(void)ctx;

	return sender_test_is_mode_active(SENDER_TEST_MODE_AUDIO);
}

static void app_rtc_request_test_audio_restart(void *ctx)
{
	(void)ctx;

	sender_test_request_audio_restart();
}

static esp_err_t app_apply_audio_preferences(void)
{
	app_audio_config_t audio_config = {0};

	ESP_RETURN_ON_ERROR(app_audio_config_load(&audio_config), TAG, "load audio config failed");
	ESP_RETURN_ON_ERROR(speaker_set_volume_percent(audio_config.speaker_volume_percent),
			    TAG,
			    "apply speaker volume failed");
	ESP_RETURN_ON_ERROR(microphone_set_gain_percent(audio_config.capture_gain_percent),
			    TAG,
			    "apply capture gain failed");
	return ESP_OK;
}

static bool app_capture_uplink_allowed(void)
{
	audio_stats_t audio = {0};

	audio_device_get_stats(&audio);
	return audio.capture_gain_percent > 0U;
}

static esp_err_t app_stop_app_services(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_CALL:
	{
		esp_err_t start_ret = app_cancel_pending_call_start_for_lifecycle();
		if (start_ret != ESP_OK) {
			return start_ret;
		}
		app_cancel_contact_scan_for_lifecycle();
		device_call_snapshot_t call = {0};
		device_call_get_snapshot(&call);
		esp_err_t call_ret = ESP_OK;
		if (call.pending_incoming || call.state == DEVICE_CALL_STATE_INCOMING) {
			call_ret = device_call_reject_pending();
		} else if (call.state == DEVICE_CALL_STATE_OUTGOING ||
			   call.state == DEVICE_CALL_STATE_CONNECTING ||
			   call.state == DEVICE_CALL_STATE_IN_CALL) {
			call_ret = device_call_hangup();
		}
		if (call_ret != ESP_OK && call_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "app lifecycle device call close failed: %s", esp_err_to_name(call_ret));
		}
		/* Never fall back to the legacy CALL/HANGUP signalling contract. */
		rtc_transport_flush_remote_media();
		esp_err_t disconnect_ret = rtc_transport_disconnect();
		if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "app lifecycle device call disconnect failed: %s",
				 esp_err_to_name(disconnect_ret));
		}
		app_release_call_session_resources_internal(true);
		break;
	}
	case APP_ID_AI_CHAT:
		(void)app_close_ai_chat();
		break;
	case APP_ID_WECHAT:
		app_cancel_wechat_contact_scan_for_lifecycle();
		wechat_voip_service_stop();
		app_release_wechat_call_resources_internal(true);
		break;
	case APP_ID_SYSTEM:
	{
		app_cancel_tirtc_config_scan_for_lifecycle();
		network_cancel_ping();
		sender_test_stop();
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		break;
	}
	case APP_ID_HOME:
	case APP_ID_DEVICE:
	{
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		break;
	}
	default:
		break;
	}
	return ESP_OK;
}

static esp_err_t app_start_app_services(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_WECHAT:
	{
		esp_err_t ret = app_prepare_rtc_if_network_ready();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			return ret;
		}
		return wechat_voip_service_start();
	}
	case APP_ID_AI_CHAT:
		app_reset_ai_chat_token_prefetch_throttle();
		if (!network_is_connected()) {
			ESP_LOGD(TAG, "AI Chat waits for network connection");
			return ESP_OK;
		}
	{
		esp_err_t ret = app_open_ai_chat();
		if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGD(TAG, "AI Chat waits for RTC/time readiness");
			return ESP_OK;
		}
		return ret;
	}
	case APP_ID_CALL:
	{
		if (!device_online_is_online()) {
			ESP_LOGI(CALL_FLOW_TAG,
				 "stage=contacts_refresh_skipped source=call_app_enter reason=device_offline");
			return ESP_OK;
		}
		esp_err_t ret = device_call_refresh_contacts_async();
		ESP_LOGI(CALL_FLOW_TAG,
			 "stage=contacts_refresh_requested source=call_app_enter ret=%s",
			 esp_err_to_name(ret));
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "refresh contacts on call app entry failed: %s", esp_err_to_name(ret));
		}
		return ESP_OK;
	}
	case APP_ID_DEVICE:
		return ESP_OK;
	case APP_ID_SYSTEM:
	case APP_ID_HOME:
	default:
		return ESP_OK;
	}
}

esp_err_t app_acquire_call_session_resources(bool video)
{
	esp_err_t ret = ESP_OK;
	uint32_t resources = APP_RESOURCE_RTC | APP_RESOURCE_AUDIO;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	/*
	 * Device calls start at the same latency as LAN playback. The sink only
	 * expands its jitter budget after measured underflow/backlog pressure, so
	 * cellular resilience does not impose permanent delay on healthy links.
	 */
	app_set_rtc_media_prepared(true);

	if (video) {
		/* Start the decoder while the normal encoder reservation still protects
		 * its contiguous reference block. Decoder startup may create persistent
		 * TinyH264/FreeRTOS control objects; placing them outside that block keeps
		 * the full encoder profile recoverable after a compact call ends. */
		ret = call_video_renderer_start_for_codec(CALL_VIDEO_CODEC_H264);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "start H264 call downlink failed: %s", esp_err_to_name(ret));
			goto fail;
		}
		s_call_resources.video_renderer_started = true;
	}

	/* Give the realtime audio path first ownership of internal DMA memory. */
	ret = app_switch_resources(resources);
	if (ret != ESP_OK) {
		goto fail;
	}

	if (video) {
		if (!s_call_resources.video_profile_applied) {
			media_governor_video_config_t call_config = {0};

			media_governor_get_rtc_video_config(&s_call_resources.previous_video_config);
			media_governor_build_device_call_video_config(&call_config);
			ret = media_governor_set_rtc_video_config(&call_config);
			if (ret != ESP_OK) {
				ESP_LOGE(TAG, "apply device-call video profile failed: %s", esp_err_to_name(ret));
				goto fail;
			}
			(void)media_governor_reset_transport_adaptation(false, NULL);
			ret = app_sync_rtc_video_bitrate_control();
			if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
				ESP_LOGW(TAG,
					 "device-call TGMP unavailable; keep normal video profile: %s",
					 esp_err_to_name(ret));
			}
			s_call_resources.video_profile_applied = true;
			camera_pipeline_on_rtc_video_config_changed();
			ESP_LOGI(TAG,
				 "device-call video profile applied: %ux%u@%u %ukbps",
				 (unsigned)call_config.width,
				 (unsigned)call_config.height,
				 (unsigned)call_config.fps,
					 (unsigned)(call_config.bitrate_bps / 1000U));
		}
	}

	ret = app_prepare_rtc_after_time_sync("call-session");
	if (ret != ESP_OK) {
		goto fail;
	}
	return ESP_OK;

fail:
	/* Acquisition never reached a usable call. Roll back both the renderer and
	 * the compact encoder profile instead of retaining a partial warm state. */
	app_release_call_session_resources_internal(true);
	return ret;
}

static esp_err_t app_restore_device_call_video_profile(void)
{
	if (!s_call_resources.video_profile_applied) {
		return ESP_OK;
	}

	esp_err_t ret = media_governor_set_rtc_video_config(
		&s_call_resources.previous_video_config);
	if (ret != ESP_OK) {
		return ret;
	}

	(void)media_governor_reset_transport_adaptation(false, NULL);
	esp_err_t bitrate_ret = app_sync_rtc_video_bitrate_control();
	if (bitrate_ret != ESP_OK && bitrate_ret != ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG,
			 "restore RTC transport bitrate range failed: %s",
			 esp_err_to_name(bitrate_ret));
	}
	camera_pipeline_on_rtc_video_config_changed();
	s_call_resources.video_profile_applied = false;
	ESP_LOGI(TAG,
		 "device-call video profile restored: %ux%u@%u %ukbps",
		 (unsigned)s_call_resources.previous_video_config.width,
		 (unsigned)s_call_resources.previous_video_config.height,
		 (unsigned)s_call_resources.previous_video_config.fps,
		 (unsigned)(s_call_resources.previous_video_config.bitrate_bps / 1000U));
	return ESP_OK;
}

static void app_release_call_session_resources_internal(bool restore_video_profile)
{
	restore_video_profile = restore_video_profile && s_call_resources.video_profile_applied;

	if (s_call_resources.video_renderer_started) {
		/* The software H264 decoder owns scarce internal allocations even though
		 * its frame pools and caller task stack live in PSRAM. Keeping it alive
		 * after hangup leaves ESP-Hosted without a usable DMA block. The fixed
		 * PSRAM pools stay prewarmed, so only the session-owned decoder worker is
		 * rebuilt for the next call. */
		esp_err_t video_ret = call_video_renderer_stop();
		if (video_ret == ESP_OK || video_ret == ESP_ERR_INVALID_STATE) {
			s_call_resources.video_renderer_started = false;
		} else {
			ESP_LOGW(TAG, "stop H264 call downlink failed: %s", esp_err_to_name(video_ret));
		}
	}

	/* Release the call's RTC/audio allocations first. On lifecycle exit the
	 * normal H264 profile is rebuilt only after those internal allocations are
	 * gone; between calls the smaller encoder reservation stays warm. */
	if (app_get_active_app() == APP_ID_CALL) {
		esp_err_t ret = app_switch_resources(app_resource_mask_for_app(APP_ID_CALL));
		if (ret != ESP_OK) {
			ESP_LOGW(TAG, "release call session resources failed: %s", esp_err_to_name(ret));
		}
		app_state_reset_call_media_policy();
	}

	if (restore_video_profile) {
		esp_err_t profile_ret = app_restore_device_call_video_profile();
		if (profile_ret != ESP_OK) {
			ESP_LOGW(TAG, "restore RTC video profile failed: %s", esp_err_to_name(profile_ret));
		}
	}

	app_set_rtc_media_prepared(false);
}

void app_release_call_session_resources(void)
{
	/* Keep the call-sized encoder reservation and PSRAM frame pools warm while
	 * the call page remains open. The session-owned H264 decoder is released. */
	app_release_call_session_resources_internal(false);
}

static esp_err_t app_prepare_wechat_call_resources(bool local_video_enabled,
						   bool remote_video_enabled,
						   void *ctx)
{
	esp_err_t ret = ESP_OK;

	(void)ctx;
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	app_set_rtc_media_prepared(true);

	if (remote_video_enabled && !s_wechat_call_resources.video_renderer_started) {
		ret = call_video_renderer_start_for_codec(CALL_VIDEO_CODEC_MJPEG);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "start WeChat MJPEG downlink failed: %s", esp_err_to_name(ret));
			goto fail;
		}
		s_wechat_call_resources.video_renderer_started = true;
	}

	ret = app_switch_resources(APP_WECHAT_RESOURCE_MASK);
	if (ret != ESP_OK) {
		goto fail;
	}

	if (local_video_enabled && !s_wechat_call_resources.video_profile_applied) {
		media_governor_video_config_t call_config = {0};

		media_governor_get_rtc_video_config(
			&s_wechat_call_resources.previous_video_config);
		media_governor_build_wechat_video_config(&call_config);
		ret = media_governor_set_rtc_video_config(&call_config);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG,
				 "apply WeChat video profile failed: %s",
				 esp_err_to_name(ret));
			goto fail;
		}
		s_wechat_call_resources.video_profile_applied = true;
		(void)media_governor_reset_transport_adaptation(false, NULL);
		ret = app_sync_rtc_video_bitrate_control();
		if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
			ESP_LOGW(TAG,
				 "WeChat TGMP unavailable; keep normal video profile: %s",
				 esp_err_to_name(ret));
		}
		camera_pipeline_on_rtc_video_config_changed();
		ESP_LOGI(TAG,
			 "WeChat video profile applied: %ux%u@%u %ukbps",
			 (unsigned)call_config.width,
			 (unsigned)call_config.height,
			 (unsigned)call_config.fps,
			 (unsigned)(call_config.bitrate_bps / 1000U));
	}

	ret = app_prepare_rtc_after_time_sync("wechat-call");
	if (ret != ESP_OK) {
		goto fail;
	}
	return ESP_OK;

fail:
	app_release_wechat_call_resources_internal(true);
	return ret;
}

static void app_release_wechat_call_resources_internal(bool leave_wechat_page)
{
	bool leave_page = leave_wechat_page || app_get_active_app() != APP_ID_WECHAT;

	if (s_wechat_call_resources.video_renderer_started && leave_page) {
		esp_err_t ret = call_video_renderer_stop();
		if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
			s_wechat_call_resources.video_renderer_started = false;
		} else {
			ESP_LOGW(TAG,
				 "stop WeChat MJPEG downlink failed: %s",
				 esp_err_to_name(ret));
		}
	} else if (s_wechat_call_resources.video_renderer_started) {
		call_video_renderer_flush();
	}

	if (leave_page && s_wechat_call_resources.video_profile_applied) {
		esp_err_t ret = media_governor_set_rtc_video_config(
			&s_wechat_call_resources.previous_video_config);
		if (ret == ESP_OK) {
			(void)media_governor_reset_transport_adaptation(false, NULL);
			esp_err_t bitrate_ret = app_sync_rtc_video_bitrate_control();
			if (bitrate_ret != ESP_OK &&
			    bitrate_ret != ESP_ERR_NOT_SUPPORTED) {
				ESP_LOGW(TAG,
					 "restore RTC transport bitrate range after WeChat failed: %s",
					 esp_err_to_name(bitrate_ret));
			}
			camera_pipeline_on_rtc_video_config_changed();
			s_wechat_call_resources.video_profile_applied = false;
			ESP_LOGI(TAG,
				 "WeChat video profile restored: %ux%u@%u %ukbps",
				 (unsigned)s_wechat_call_resources.previous_video_config.width,
				 (unsigned)s_wechat_call_resources.previous_video_config.height,
				 (unsigned)s_wechat_call_resources.previous_video_config.fps,
				 (unsigned)(s_wechat_call_resources.previous_video_config.bitrate_bps /
					    1000U));
		} else {
			ESP_LOGW(TAG,
				 "restore RTC video profile after WeChat failed: %s",
				 esp_err_to_name(ret));
		}
	}

	app_set_rtc_media_prepared(false);
}

static void app_release_wechat_call_resources(void *ctx)
{
	(void)ctx;
	app_release_wechat_call_resources_internal(false);
}

static void app_release_call_session_resources_if_idle(void)
{
	device_call_snapshot_t call = {0};

	if (app_get_active_app() != APP_ID_CALL) {
		return;
	}
	if (app_call_start_is_in_progress()) {
		return;
	}
	device_call_get_snapshot(&call);
	if (call.state == DEVICE_CALL_STATE_OUTGOING ||
	    call.state == DEVICE_CALL_STATE_CONNECTING ||
	    call.state == DEVICE_CALL_STATE_IN_CALL) {
		return;
	}
	ESP_LOGI(TAG,
		 "device-call session complete: state=%u error=%s reason=%s",
		 (unsigned)call.state,
		 esp_err_to_name(call.last_error),
		 call.message[0] != '\0' ? call.message : "-");
	app_release_call_session_resources();
}

esp_err_t app_suspend_call_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(app_resource_mask_for_app(APP_ID_HOME));
}

esp_err_t app_resume_call_scan_resources(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_switch_resources(app_resource_mask_for_app(APP_ID_CALL));
	if (ret != ESP_OK) {
		return ret;
	}
	return app_start_app_services(APP_ID_CALL);
}

esp_err_t app_suspend_wechat_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(APP_RESOURCE_CAMERA);
}

esp_err_t app_resume_wechat_scan_resources(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_switch_resources(app_resource_mask_for_app(APP_ID_WECHAT));
	if (ret != ESP_OK) {
		return ret;
	}
	return app_start_app_services(APP_ID_WECHAT);
}

esp_err_t app_acquire_tirtc_config_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(app_resource_mask_for_app(APP_ID_SYSTEM) | APP_RESOURCE_CAMERA);
}

void app_release_tirtc_config_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_SYSTEM) {
		return;
	}

	esp_err_t ret = app_switch_resources(app_resource_mask_for_app(APP_ID_SYSTEM));
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "release tirtc config scan resources failed: %s", esp_err_to_name(ret));
	}
}

esp_err_t app_init(void)
{
	display_actions_t display_actions = {0};
	ota_config_t ota_config = app_make_ota_config();

	app_ui_configure_display_actions(&display_actions);

	ESP_LOGI(TAG, "system init start");
	const esp_app_desc_t *app_desc = esp_app_get_description();
	ESP_LOGI(TAG,
		 "firmware version: %s project=%s built=%s %s",
		 app_desc != NULL ? app_desc->version : "unknown",
		 app_desc != NULL ? app_desc->project_name : "unknown",
		 app_desc != NULL ? app_desc->date : "unknown",
		 app_desc != NULL ? app_desc->time : "unknown");
	if (app_desc != NULL) {
		char elf_sha256[65];
		static const char hex[] = "0123456789abcdef";
		for (size_t i = 0; i < sizeof(app_desc->app_elf_sha256); ++i) {
			elf_sha256[i * 2] = hex[app_desc->app_elf_sha256[i] >> 4];
			elf_sha256[i * 2 + 1] = hex[app_desc->app_elf_sha256[i] & 0x0f];
		}
		elf_sha256[sizeof(elf_sha256) - 1] = '\0';
		ESP_LOGI(TAG, "firmware ELF SHA256=%s", elf_sha256);
	}
	/*
	 * The P4 JPEG driver owns small internal DMA descriptors. Reserve those
	 * before the DMA escrow, H264 encoder, RTC and audio stacks occupy the
	 * internal heap; every WeChat call reuses the same decoder handle.
	 */
	esp_err_t mjpeg_decoder_prewarm_ret =
		call_video_renderer_prewarm_mjpeg_decoder();
	if (mjpeg_decoder_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "MJPEG decoder early prewarm unavailable: %s",
			 esp_err_to_name(mjpeg_decoder_prewarm_ret));
	}
	/*
	 * Audio input/output and the realtime capture stack are retained after the
	 * first call. Reserve them before the H264 reference block so later call
	 * teardown restores the same heap topology as boot instead of leaving a
	 * small persistent allocation inside the encoder's contiguous region.
	 */
	esp_err_t audio_prewarm_ret = audio_device_prepare();
	if (audio_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG, "audio early prewarm unavailable: %s", esp_err_to_name(audio_prewarm_ret));
	}
	esp_err_t dma_escrow_ret = media_dma_reserve_init();
	if (dma_escrow_ret != ESP_OK) {
		ESP_LOGW(TAG, "DMA escrow init unavailable: %s", esp_err_to_name(dma_escrow_ret));
	}
	app_log_heap_snapshot("media prewarm before");
	esp_err_t h264_prewarm_ret = camera_pipeline_prewarm_h264();
	if (h264_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG, "H264 early prewarm unavailable: %s", esp_err_to_name(h264_prewarm_ret));
	}
	esp_err_t rtc_pool_prewarm_ret = rtc_transport_prewarm_media_pools();
	if (rtc_pool_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "RTC media PSRAM pool prewarm unavailable: %s",
			 esp_err_to_name(rtc_pool_prewarm_ret));
	}
	esp_err_t scaler_prewarm_ret = camera_pipeline_prewarm_call_scaler();
	if (scaler_prewarm_ret != ESP_OK && scaler_prewarm_ret != ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG,
			 "device-call scaler early prewarm unavailable: %s",
			 esp_err_to_name(scaler_prewarm_ret));
	}
	esp_err_t renderer_prewarm_ret = call_video_renderer_prewarm();
	if (renderer_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "device-call video pool prewarm unavailable: %s",
			 esp_err_to_name(renderer_prewarm_ret));
	}
	/*
	 * Reserve the high-performance AEC working set before networking and RTC
	 * fragment memory. The driver retains it across app switches, so first
	 * speech and later calls do not pay repeated ESP-SR creation cost.
	 */
	esp_err_t aec_prewarm_ret = audio_device_prepare_echo_cancel();
	if (aec_prewarm_ret != ESP_OK) {
		ESP_LOGW(TAG, "AEC early prewarm unavailable: %s", esp_err_to_name(aec_prewarm_ret));
	}
	app_log_heap_snapshot("media prewarm after");
	ESP_RETURN_ON_ERROR(app_audio_policy_init(), TAG, "audio policy init failed");
	ESP_RETURN_ON_ERROR(platform_nvs_async_init(), TAG, "nvs worker init failed");
	ESP_RETURN_ON_ERROR(device_init(app_on_boot_button_changed, NULL), TAG, "device init failed");
	app_preload_persistent_state();
	ESP_RETURN_ON_ERROR(app_start_control_task(), TAG, "app control worker init failed");
	ESP_RETURN_ON_ERROR(app_configure_thing_service_registry(), TAG, "thing service registry init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_binding(), TAG, "device binding init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_online(), TAG, "device online init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_call(), TAG, "device call init failed");
	const wechat_voip_media_lifecycle_t wechat_media_lifecycle = {
		.prepare = app_prepare_wechat_call_resources,
		.release = app_release_wechat_call_resources,
	};
	ESP_RETURN_ON_ERROR(
		wechat_voip_service_configure_media_lifecycle(&wechat_media_lifecycle, NULL),
		TAG,
		"WeChat media lifecycle configure failed");
	ESP_RETURN_ON_ERROR(
		wechat_voip_service_set_incoming_policy(app_wechat_incoming_allowed, NULL),
		TAG,
		"WeChat incoming policy configure failed");
	ESP_RETURN_ON_ERROR(rtc_transport_set_media_bridge(rtc_media_bridge_get_ops(),
							   rtc_media_bridge_get_context()),
			    TAG,
			    "rtc media bridge configure failed");
	app_ai_chat_config_t ai_chat_ui_config = {0};
	esp_err_t ai_chat_ui_ret = app_ai_chat_config_load(&ai_chat_ui_config);
	if (ai_chat_ui_ret != ESP_OK) {
		ESP_LOGW(TAG, "AI Chat UI preference init failed: %s", esp_err_to_name(ai_chat_ui_ret));
	}
	esp_err_t audio_pref_ret = app_apply_audio_preferences();
	if (audio_pref_ret != ESP_OK) {
		ESP_LOGW(TAG, "audio preference init failed: %s", esp_err_to_name(audio_pref_ret));
	}
	system_time_set_sync_cb(app_time_sync_cb, NULL);
	network_set_state_cb(app_network_state_cb, NULL);
	ESP_RETURN_ON_ERROR(app_start_network_baseline(), TAG, "network baseline init failed");

	rtc_transport_hooks_t rtc_hooks = {
		.is_test_video_active = app_rtc_test_video_active,
		.is_test_audio_active = app_rtc_test_audio_active,
		.request_test_audio_restart = app_rtc_request_test_audio_restart,
	};
	rtc_transport_set_hooks(&rtc_hooks, NULL);

	ESP_RETURN_ON_ERROR(app_switch_resources(app_resource_mask_for_app(APP_ID_HOME)),
			    TAG,
			    "acquire home resources failed");
	ESP_RETURN_ON_ERROR(ota_init(&ota_config), TAG, "ota init failed");
	ESP_RETURN_ON_ERROR(display_init(&display_actions), TAG, "display init failed");
	display_set_snapshot_provider(app_ui_fill_display_status, NULL);
#if APP_CONFIG_DEBUG_SCREEN_SERVER_ENABLE
	esp_err_t screen_debug_ret = screen_debug_server_start();
	if (screen_debug_ret != ESP_OK) {
		ESP_LOGW(TAG, "screen debug server start failed: %s", esp_err_to_name(screen_debug_ret));
	}
#endif
#if CONFIG_APP_SERIAL_CALL_CLI_ENABLE
	esp_err_t serial_cli_ret = serial_call_cli_start();
	if (serial_cli_ret != ESP_OK) {
		ESP_LOGW(TAG, "serial call CLI start failed: %s", esp_err_to_name(serial_cli_ret));
	}
#endif

	app_log_heap_snapshot("system ready");
#if CONFIG_APP_MEMORY_WATERLINE_LOG
	app_monitor_memory_health(true);
#endif
	app_log_performance_profile();
	ESP_LOGI(TAG, "system ready: ESP32-P4 TiRTC dashboard");
	return ESP_OK;
}

#if CONFIG_APP_MEMORY_WATERLINE_LOG || CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || \
	CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS || \
	CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE || CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
static void app_step_rtc_video_transport_adaptation(void)
{
#if CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
	bool changed = false;
	esp_err_t ret = media_governor_step_transport_adaptation(&changed);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "RTC video bitrate convergence failed: %s",
			 esp_err_to_name(ret));
	} else if (changed) {
		camera_pipeline_on_rtc_video_config_changed();
	}
#endif
}

static void app_runtime_monitor_loop(void)
{
	const TickType_t monitor_interval_ticks =
#if CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
		pdMS_TO_TICKS(APP_RTC_VIDEO_ADAPT_INTERVAL_MS);
#else
		pdMS_TO_TICKS(APP_MEDIA_HEALTH_INTERVAL_MS);
#endif
	TickType_t last_wake_tick = xTaskGetTickCount();
#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE
	TickType_t last_media_health_tick = last_wake_tick;
#endif
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
	TickType_t last_runtime_snapshot_tick = last_wake_tick;
#endif
#if CONFIG_APP_MEMORY_WATERLINE_LOG
	TickType_t last_memory_waterline_tick = last_wake_tick;
#endif

	while (true) {
		vTaskDelayUntil(&last_wake_tick, monitor_interval_ticks);
#if CONFIG_APP_MEMORY_WATERLINE_LOG || CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || \
	CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE || CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
		const TickType_t now = xTaskGetTickCount();
#endif

		/* The callback normally posts an app-control marker. If that bounded
		 * queue was momentarily full, consume its coalesced latest target here
		 * instead of leaving TGMP feedback pending indefinitely. */
		app_apply_pending_rtc_video_bitrate(false);
		app_monitor_rtc_video_transport_compatibility();
		app_step_rtc_video_transport_adaptation();

#if CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG || CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE
		if (now - last_media_health_tick >=
		    pdMS_TO_TICKS(APP_MEDIA_HEALTH_INTERVAL_MS)) {
			last_media_health_tick = now;
			app_monitor_media_health();
		}
#endif
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
		if (now - last_runtime_snapshot_tick >=
		    pdMS_TO_TICKS(APP_RUNTIME_SNAPSHOT_INTERVAL_MS)) {
			last_runtime_snapshot_tick = now;
			app_log_runtime_snapshot();
		}
#endif
#if CONFIG_APP_MEMORY_WATERLINE_LOG
		if (now - last_memory_waterline_tick >=
		    pdMS_TO_TICKS(APP_MEMORY_WATERLINE_INTERVAL_MS)) {
			last_memory_waterline_tick = now;
			app_monitor_memory_health(false);
		}
#endif
	}
}

static void app_runtime_monitor_task(void *arg)
{
	(void)arg;
	app_runtime_monitor_loop();
}
#endif

void app_run(void)
{
#if !CONFIG_APP_MEMORY_WATERLINE_LOG && !CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG && \
	!CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS && \
	!CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE && !CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
	APP_LOG_DETAIL(TAG, "runtime monitor disabled; main task can exit");
	return;
#else
	if (s_app_runtime_monitor_task != NULL) {
		return;
	}

	BaseType_t task_ret = xTaskCreateWithCaps(app_runtime_monitor_task,
						  "app_runtime",
						  APP_RUNTIME_MONITOR_TASK_STACK_SIZE,
						  NULL,
						  APP_RUNTIME_MONITOR_TASK_PRIORITY,
						  &s_app_runtime_monitor_task,
						  APP_TASK_STACK_CAPS_BACKGROUND);
	if (task_ret != pdPASS) {
		ESP_LOGE(TAG, "runtime monitor task start failed; keeping main task alive");
		app_runtime_monitor_loop();
	}

	APP_LOG_DETAIL(TAG, "media monitor task started; main task can exit");
#endif
}

static esp_err_t app_release_active_app_locked(app_id_t app_id)
{
	if (app_id == APP_ID_HOME) {
		return ESP_OK;
	}
	if (app_get_active_app() != app_id) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGI(TAG, "app release: %s", app_id_name(app_id));
	esp_err_t stop_ret = app_stop_app_services(app_id);
	if (stop_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "app stop failed: %s ret=%s",
			 app_id_name(app_id),
			 esp_err_to_name(stop_ret));
		return stop_ret;
	}

	esp_err_t release_ret =
		app_switch_resources(app_resource_mask_for_app(APP_ID_HOME));
	if (release_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "app release failed: %s ret=%s",
			 app_id_name(app_id),
			 esp_err_to_name(release_ret));
		app_set_active_app(APP_ID_HOME);
		return release_ret;
	}
	app_set_active_app(APP_ID_HOME);
	return app_wait_shared_media_quiescent(app_id);
}

static esp_err_t app_enter_app_locked(app_id_t app_id)
{
	if (app_id < APP_ID_HOME || app_id > APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_ARG;
	}

	app_id_t current = app_get_active_app();
	if (current == app_id) {
		return ESP_OK;
	}

	if (current != APP_ID_HOME) {
		ESP_RETURN_ON_ERROR(app_release_active_app_locked(current),
				    TAG,
				    "release current app failed");
	}

	ESP_LOGI(TAG, "app enter: %s", app_id_name(app_id));
	uint32_t target_resources = app_resource_mask_for_app(app_id);
	esp_err_t ret = app_switch_resources(target_resources);
	if (ret == ESP_OK) {
		ret = app_start_app_services(app_id);
	}
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "app enter failed: %s ret=%s", app_id_name(app_id), esp_err_to_name(ret));
		(void)app_stop_app_services(app_id);
		(void)app_switch_resources(app_resource_mask_for_app(APP_ID_HOME));
		app_set_active_app(APP_ID_HOME);
		return ret;
	}

	app_set_active_app(app_id);
	return ESP_OK;
}

static esp_err_t app_return_home_locked(void)
{
	app_id_t previous = app_get_active_app();

	if (previous != APP_ID_HOME) {
		ESP_LOGI(TAG, "app return home: release %s", app_id_name(previous));
		ESP_RETURN_ON_ERROR(app_stop_app_services(previous),
				    TAG,
				    "stop current app failed");
	}
	ESP_RETURN_ON_ERROR(app_switch_resources(app_resource_mask_for_app(APP_ID_HOME)),
			    TAG,
			    "return home resources failed");
	app_set_active_app(APP_ID_HOME);
	if (previous != APP_ID_HOME) {
		ESP_RETURN_ON_ERROR(app_wait_shared_media_quiescent(previous),
				    TAG,
				    "return home quiesce failed");
	}
	app_schedule_ai_chat_token_prefetch("home");
	return ESP_OK;
}

static esp_err_t app_enter_app_sync(app_id_t app_id)
{
	if (s_app_transition_mutex == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t ret = app_enter_app_locked(app_id);
	xSemaphoreGive(s_app_transition_mutex);
	return ret;
}

static esp_err_t app_return_home_sync(void)
{
	if (s_app_transition_mutex == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t ret = app_return_home_locked();
	xSemaphoreGive(s_app_transition_mutex);
	return ret;
}

esp_err_t app_enter_app(app_id_t app_id)
{
	return app_enter_app_sync(app_id);
}

esp_err_t app_return_home(void)
{
	return app_return_home_sync();
}

esp_err_t app_request_enter_app(app_id_t app_id)
{
	if (app_id < APP_ID_HOME || app_id > APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_ARG;
	}

	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_ENTER_APP, app_id);
}

esp_err_t app_request_return_home(void)
{
	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_RETURN_HOME, APP_ID_HOME);
}

esp_err_t app_request_accept_call(void)
{
	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_CALL_ACCEPT, APP_ID_CALL);
}

static void app_parse_ping_target(char *target, size_t target_len)
{
	rtc_transport_config_t rtc_config = {0};
	const char *endpoint = NULL;
	const char *start = NULL;
	size_t length = 0;

	rtc_transport_get_config(&rtc_config);
	endpoint = rtc_config.service_endpoint;
	if (endpoint == NULL || endpoint[0] == '\0') {
		strlcpy(target, "223.5.5.5", target_len);
		return;
	}

	start = strstr(endpoint, "://");
	start = (start != NULL) ? (start + 3) : endpoint;
	while (start[length] != '\0' && start[length] != '/' && start[length] != ':' && length < target_len - 1) {
		length++;
	}
	if (length == 0) {
		strlcpy(target, "223.5.5.5", target_len);
		return;
	}
	memcpy(target, start, length);
	target[length] = '\0';
}

esp_err_t app_connect_wifi(const char *ssid, const char *password)
{
	return network_connect(ssid, password);
}

esp_err_t app_request_wifi_scan(void)
{
	return network_request_scan();
}

esp_err_t app_update_device_uuid(const char *uuid)
{
	esp_err_t ret = device_set_uuid(uuid);
	if (ret != ESP_OK) {
		return ret;
	}

	if (!app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}
	return app_configure_tirtc();
}

esp_err_t app_start_ping_test(void)
{
	char target[NETWORK_PING_TARGET_MAX] = {0};

	app_parse_ping_target(target, sizeof(target));
	return network_start_ping(target);
}

esp_err_t app_disconnect_rtc(void)
{
	esp_err_t ret = rtc_transport_disconnect();

	if (ret != ESP_OK) {
		return ret;
	}

	app_reset_rtc_call_media_state();
	return ESP_OK;
}

static esp_err_t app_reconfigure_tirtc_after_settings_change(const char *reason)
{
	esp_err_t ret = ESP_OK;

	(void)app_configure_ai_chat();
	if (!app_rtc_runtime_is_initialized()) {
		return app_prepare_rtc_after_config_if_ready(reason);
	}

	if (rtc_transport_get_state() == RTC_TRANSPORT_STATE_STOPPED ||
	    rtc_transport_get_state() == RTC_TRANSPORT_STATE_READY) {
		ret = app_configure_tirtc();
		if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "rtc config saved; endpoint changes need reboot or explicit full reset after SDK init");
			return ESP_OK;
		}
		if (ret != ESP_OK) {
			return ret;
		}
		return app_prepare_rtc_after_config_if_ready(reason);
	}

	ESP_LOGI(TAG,
		 "rtc config changed while session is active; it will apply after disconnect or reboot: %s",
		 reason != NULL ? reason : "settings");
	return ESP_OK;
}

esp_err_t app_update_rtc_config_field(app_rtc_config_field_t field, const char *value)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_config_field(field, value), TAG, "rtc config save failed");

	app_request_rtc_reconfigure_after_settings_change("field");
	return ESP_OK;
}

esp_err_t app_update_rtc_device_credentials(const char *device_id, const char *device_secret)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_device_credentials(device_id, device_secret),
			    TAG,
			    "rtc device credentials save failed");
	ai_chat_token_invalidate_cache();
	app_reset_ai_chat_token_prefetch_throttle();
	app_rtc_identity_conflict_clear_if_new_credentials(device_id);

	if (network_is_connected() && system_time_has_valid_time()) {
		device_online_set_network_ready(true);
	}
	wechat_voip_service_suspend_ingress();
	device_call_reset_identity_state();
	(void)device_online_notify_credentials_changed("credentials");
	app_request_rtc_reconfigure_after_settings_change("credential-scan");
	app_request_rtc_prepare_after_identity("credentials");
	return ESP_OK;
}

static esp_err_t app_start_device_binding_reconcile_if_needed(const char *reason)
{
	device_binding_snapshot_t binding = {0};

	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!app_rtc_device_credentials_available()) {
		return device_binding_start_async(reason != NULL ? reason : "auto");
	}

	device_binding_get_snapshot(&binding);
	if (binding.running ||
	    binding.state == DEVICE_BINDING_STATE_WAITING_USER) {
		return ESP_OK;
	}

	/*
	 * Bound devices try the online/token path first. A token-reset callback
	 * starts signed binding report with retained credentials when required,
	 * so normal boot must not consume a new verification code proactively.
	 */
	return ESP_OK;
}

static esp_err_t app_start_device_online_if_ready(const char *reason)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		device_online_set_network_ready(false);
		return ESP_ERR_INVALID_STATE;
	}
	device_online_set_network_ready(true);
	if (!app_rtc_device_credentials_available()) {
		return ESP_ERR_NOT_FOUND;
	}

	esp_err_t ret = device_online_start_async(reason != NULL ? reason : "auto");
	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
		app_start_device_identity_ingress();
	}
	return ret;
}

static void app_start_device_identity_ingress(void)
{
	esp_err_t wechat_ret = wechat_voip_service_start_ingress();
	if (wechat_ret != ESP_OK) {
		ESP_LOGW(TAG, "wechat call ingress start failed: %s", esp_err_to_name(wechat_ret));
	}

	esp_err_t call_ret = device_call_start();
	if (call_ret != ESP_OK && call_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "device call listener start failed: %s", esp_err_to_name(call_ret));
	}
}

static esp_err_t app_start_device_identity_services(const char *reason)
{
	if (!thing_service_registry_is_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	esp_err_t binding_ret = app_start_device_binding_reconcile_if_needed(reason);
	esp_err_t ret = app_start_device_online_if_ready(reason);
	if (ret == ESP_OK) {
		return ESP_OK;
	}
	if (ret != ESP_ERR_NOT_FOUND) {
		return ret;
	}
	return binding_ret;
}

esp_err_t app_start_device_binding(void)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!thing_service_registry_is_ready()) {
		app_schedule_thing_bootstrap("manual-refresh");
		return ESP_OK;
	}

	return app_queue_device_binding_refresh("manual-refresh");
}

static esp_err_t app_refresh_device_binding_internal(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "manual-refresh";

	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (app_rtc_device_credentials_available()) {
		return app_reset_device_binding_internal(safe_reason);
	}

	device_binding_reset_state(safe_reason);
	esp_err_t ret = device_binding_start_async(safe_reason);
	if (ret == ESP_ERR_INVALID_STATE) {
		return ESP_OK;
	}
	return ret;
}

static esp_err_t app_reset_device_binding_internal(const char *reason)
{
	esp_err_t ret = ESP_OK;
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "reset-binding";

	if (!app_rtc_device_credentials_available()) {
		ESP_LOGW(TAG, "device binding reconcile needs retained credentials: reason=%s", safe_reason);
		return ESP_ERR_NOT_FOUND;
	}

	ESP_LOGI(TAG, "device binding reconcile begin: reason=%s identity=retained", safe_reason);
	wechat_voip_service_suspend_ingress();
	device_call_reset_identity_state();
	(void)rtc_transport_disconnect();
	app_reset_rtc_call_media_state();
	device_online_stop();
	device_online_invalidate_cache();
	ai_chat_token_invalidate_cache();
	app_reset_ai_chat_token_prefetch_throttle();
	device_binding_reset_state(safe_reason);

	if (network_is_connected() && system_time_has_valid_time()) {
		ret = device_binding_start_async(safe_reason);
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "signed binding reconcile start failed: %s", esp_err_to_name(ret));
			return ret;
		}
	}

	ESP_LOGI(TAG, "device binding reconcile queued: reason=%s identity=retained", safe_reason);
	return ESP_OK;
}

esp_err_t app_reset_device_binding(void)
{
	return app_reset_device_binding_internal("reset-binding");
}

esp_err_t app_request_update_rtc_device_credentials(const char *device_id, const char *device_secret)
{
	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (device_id == NULL || device_id[0] == '\0' ||
	    device_secret == NULL || device_secret[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(device_id) >= APP_RTC_CONFIG_TEXT_MAX ||
	    strlen(device_secret) >= APP_RTC_CONFIG_TEXT_MAX) {
		return ESP_ERR_INVALID_SIZE;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE,
	};
	strlcpy(event.reason, "credential-scan", sizeof(event.reason));
	strlcpy(event.rtc_device_id, device_id, sizeof(event.rtc_device_id));
	strlcpy(event.rtc_device_secret, device_secret, sizeof(event.rtc_device_secret));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	ESP_LOGD(TAG, "rtc credential save queued: device_id_len=%u", (unsigned)strlen(device_id));
	return ESP_OK;
}

esp_err_t app_set_rtc_server_env(app_rtc_server_env_t env)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_config_server_env(env), TAG, "rtc server save failed");

	app_request_rtc_reconfigure_after_settings_change("server");
	return ESP_OK;
}

esp_err_t app_start_ota(void)
{
	device_state_t device = {0};

	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	device_get_state(&device);
	return ota_start_default(device.uuid);
}

void app_restart_for_ota(void)
{
	ota_restart();
}

esp_err_t app_open_ai_chat(void)
{
	esp_err_t ret = ESP_OK;
	ai_chat_config_t ai_chat_config = {0};

	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	/* Prime the adaptive playout controller before the first remote packet.
	 * AEC remains tied to media-active, where a playback reference exists. */
	app_set_ai_chat_media_prepared(true);

	ret = app_prepare_rtc_if_network_ready();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "prepare rtc for AI Chat failed: %s", esp_err_to_name(ret));
		goto fail;
	}
	ret = app_build_ai_chat_config(&ai_chat_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "build AI Chat config failed: %s", esp_err_to_name(ret));
		goto fail;
	}
	ret = ai_chat_init(&ai_chat_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "init AI Chat failed: %s", esp_err_to_name(ret));
		goto fail;
	}
	ret = ai_chat_open();
	if (ret == ESP_OK) {
		return ESP_OK;
	}

fail:
	app_set_ai_chat_media_prepared(false);
	return ret;
}

esp_err_t app_request_start_ai_chat(void)
{
	if (app_get_active_app() != APP_ID_AI_CHAT) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
}

esp_err_t app_close_ai_chat(void)
{
	esp_err_t ret = ai_chat_close();
	app_set_ai_chat_media_prepared(false);
	return ret;
}

esp_err_t app_clear_ai_chat_messages(void)
{
	return ai_chat_clear_messages();
}

esp_err_t app_handle_ai_chat_button(bool pressed)
{
	return ai_chat_handle_control_button(pressed);
}

esp_err_t app_set_ai_chat_avatar(uint8_t avatar)
{
	esp_err_t ret = app_ai_chat_config_set_avatar(avatar);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "save AI Chat avatar failed: %s", esp_err_to_name(ret));
	}
	return ret;
}

static esp_err_t app_set_speaker_volume_internal(uint8_t percent, bool persist)
{
	esp_err_t ret = speaker_set_volume_percent(percent);
	if (ret != ESP_OK) {
		return ret;
	}

	if (!persist) {
		return ESP_OK;
	}

	esp_err_t save_ret = app_audio_config_save_speaker_volume(percent);
	if (save_ret != ESP_OK) {
		ESP_LOGW(TAG, "save speaker volume failed: %s", esp_err_to_name(save_ret));
	}
	return ESP_OK;
}

esp_err_t app_set_speaker_volume(uint8_t percent)
{
	return app_set_speaker_volume_internal(percent, true);
}

esp_err_t app_set_capture_gain(uint8_t percent)
{
	esp_err_t ret = (app_get_active_resources() & APP_RESOURCE_AUDIO) != 0U ?
		app_apply_realtime_capture_profile(percent) :
		microphone_set_gain_percent(percent);
	if (ret != ESP_OK) {
		return ret;
	}

	esp_err_t save_ret = app_audio_config_save_capture_gain(percent);
	if (save_ret != ESP_OK) {
		ESP_LOGW(TAG, "save capture gain failed: %s", esp_err_to_name(save_ret));
	}
	return app_apply_media_policy();
}

static media_governor_weak_network_mode_t app_to_media_video_adaptation_mode(app_rtc_video_adaptation_mode_t mode)
{
	switch (mode) {
	case APP_RTC_VIDEO_ADAPT_FRAMERATE_PRIORITY:
		return MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY;
	case APP_RTC_VIDEO_ADAPT_RESOLUTION_PRIORITY:
		return MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY;
	case APP_RTC_VIDEO_ADAPT_OFF:
	default:
		return MEDIA_GOVERNOR_WEAK_NETWORK_OFF;
	}
}

static app_rtc_video_adaptation_mode_t app_from_media_video_adaptation_mode(media_governor_weak_network_mode_t mode)
{
	switch (mode) {
	case MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY:
		return APP_RTC_VIDEO_ADAPT_FRAMERATE_PRIORITY;
	case MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY:
		return APP_RTC_VIDEO_ADAPT_RESOLUTION_PRIORITY;
	case MEDIA_GOVERNOR_WEAK_NETWORK_OFF:
	default:
		return APP_RTC_VIDEO_ADAPT_OFF;
	}
}

esp_err_t app_set_local_video_enabled(bool enabled)
{
	if (!app_state_is_call_active()) {
		return ESP_ERR_INVALID_STATE;
	}

	app_state_set_video_enabled(enabled);
	return app_apply_media_policy();
}

esp_err_t app_set_local_audio_enabled(bool enabled)
{
	if (!app_state_is_call_active()) {
		return ESP_ERR_INVALID_STATE;
	}

	app_state_set_audio_enabled(enabled);
	return app_apply_media_policy();
}

esp_err_t app_set_rtc_video_config(const app_rtc_video_config_t *config)
{
	if (config == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	media_governor_video_config_t media_config = {0};
	media_governor_get_rtc_video_config(&media_config);
	media_config.width = config->width;
	media_config.height = config->height;
	media_config.fps = config->fps;
	media_config.bitrate_bps = config->bitrate_bps;
	media_config.weak_network_mode =
		app_to_media_video_adaptation_mode(config->adaptation_mode);
	media_config.weak_network_level = config->adaptation_level;
	esp_err_t ret = media_governor_set_rtc_video_config(&media_config);
	if (ret != ESP_OK) {
		return ret;
	}
	(void)media_governor_reset_transport_adaptation(false, NULL);
	ret = app_sync_rtc_video_bitrate_control();
	if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG,
			 "RTC TGMP update unavailable; keep normal video profile: %s",
			 esp_err_to_name(ret));
	}

	camera_pipeline_on_rtc_video_config_changed();
	return ESP_OK;
}

esp_err_t app_apply_rtc_weak_network_level(app_rtc_video_adaptation_mode_t mode, uint8_t level)
{
	esp_err_t ret = media_governor_apply_weak_network_level(app_to_media_video_adaptation_mode(mode), level);
	if (ret != ESP_OK) {
		return ret;
	}

	camera_pipeline_on_rtc_video_config_changed();
	return ESP_OK;
}

void app_get_rtc_video_config(app_rtc_video_config_t *config)
{
	if (config == NULL) {
		return;
	}

	media_governor_video_config_t media_config = {0};
	media_governor_get_rtc_video_config(&media_config);
	config->width = media_config.width;
	config->height = media_config.height;
	config->fps = media_config.fps;
	config->bitrate_bps = media_config.bitrate_bps;
	config->adaptation_mode = app_from_media_video_adaptation_mode(media_config.weak_network_mode);
	config->adaptation_level = media_config.weak_network_level;
}

void app_on_boot_button_changed(bool pressed, void *ctx)
{
	(void)ctx;
	if (app_get_active_app() == APP_ID_AI_CHAT) {
		if (ai_chat_owns_control_button()) {
			esp_err_t ret = ai_chat_handle_control_button(pressed);
			if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG, "AI Chat boot action failed: %s", esp_err_to_name(ret));
			}
		} else if (pressed) {
			esp_err_t ret = app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
			if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG, "AI Chat boot restart failed: %s", esp_err_to_name(ret));
			}
		}
		return;
	}
}

void app_get_snapshot(app_snapshot_t *snapshot)
{
	app_snapshot_get(snapshot);
}

esp_err_t app_apply_media_policy(void)
{
	app_control_state_t control = {0};
	bool enable_audio_send = true;
	esp_err_t video_ret = ESP_OK;
	esp_err_t audio_ret = ESP_OK;

	control = app_state_get_control();
	enable_audio_send = control.audio_enabled &&
				    app_capture_uplink_allowed();

	video_ret = rtc_transport_set_local_video_send_enabled(control.video_enabled);
	audio_ret = rtc_transport_set_local_audio_send_enabled(enable_audio_send);
	if (video_ret != ESP_OK || audio_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "apply media policy failed: video=%s audio=%s",
			 esp_err_to_name(video_ret),
			 esp_err_to_name(audio_ret));
	}

	return video_ret != ESP_OK ? video_ret : audio_ret;
}
