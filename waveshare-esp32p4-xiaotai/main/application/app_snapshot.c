#include "app_internal.h"

#include <string.h>

#include "app_ai_chat_config.h"
#include "app_memory_policy.h"
#include "app_rtc_config.h"
#include "ai_chat.h"
#include "audio_device.h"
#include "camera_pipeline.h"
#include "device.h"
#include "device_binding.h"
#include "device_call.h"
#include "device_online.h"
#include "media_governor.h"
#include "network.h"
#include "ota.h"
#include "rtc_transport.h"
#include "sender_test.h"
#include "wechat_voip_service.h"

static const uint16_t APP_WIFI_CONNECT_FAIL_RETRY_THRESHOLD = 8;

static ai_chat_snapshot_t *app_snapshot_ai_chat_buffer(void)
{
	static ai_chat_snapshot_t *ai;

	if (ai == NULL) {
		ai = (ai_chat_snapshot_t *)app_memory_calloc_psram(1, sizeof(*ai));
	}
	return ai;
}

static void app_snapshot_fill_network(app_network_snapshot_t *network_snapshot)
{
	network_state_t network = {0};
	network_scan_snapshot_t wifi_scan = {0};
	network_ping_status_t ping = {0};
	uint16_t scan_count = 0;

	if (network_snapshot == NULL) {
		return;
	}

	network_get_state(&network);
	network_get_scan_results(&wifi_scan);
	network_get_ping_status(&ping);

	network_snapshot->connected = network.connected;
	network_snapshot->rssi = network.rssi;
	strlcpy(network_snapshot->ip_addr, network.ip_addr, sizeof(network_snapshot->ip_addr));
	strlcpy(network_snapshot->ssid, network.ssid, sizeof(network_snapshot->ssid));
	network_get_saved_config(network_snapshot->saved_ssid,
				 sizeof(network_snapshot->saved_ssid),
				 network_snapshot->saved_password,
				 sizeof(network_snapshot->saved_password));
	network_snapshot->connect_failed = network.retry_count >= APP_WIFI_CONNECT_FAIL_RETRY_THRESHOLD;
	network_snapshot->scan_in_progress = wifi_scan.in_progress;
	scan_count = wifi_scan.count > APP_WIFI_SCAN_MAX ? APP_WIFI_SCAN_MAX : wifi_scan.count;
	network_snapshot->scan_count = scan_count;
	for (uint16_t index = 0; index < scan_count; ++index) {
		strlcpy(network_snapshot->scan_results[index].ssid,
			wifi_scan.results[index].ssid,
			sizeof(network_snapshot->scan_results[index].ssid));
		network_snapshot->scan_results[index].rssi = wifi_scan.results[index].rssi;
		network_snapshot->scan_results[index].secure = wifi_scan.results[index].secure;
		network_snapshot->scan_results[index].channel = wifi_scan.results[index].channel;
	}
	network_snapshot->ping_running = ping.running;
	network_snapshot->ping_valid = ping.valid;
	network_snapshot->ping_transmitted = ping.transmitted;
	network_snapshot->ping_received = ping.received;
	network_snapshot->ping_latency_avg_ms = ping.avg_time_ms;
	network_snapshot->ping_jitter_ms = ping.jitter_ms;
	network_snapshot->ping_loss_percent = ping.loss_percent;
}

static void app_snapshot_fill_device(app_device_snapshot_t *device_snapshot)
{
	device_state_t device = {0};

	if (device_snapshot == NULL) {
		return;
	}

	device_get_state(&device);
	strlcpy(device_snapshot->uuid, device.uuid, sizeof(device_snapshot->uuid));
	device_snapshot->cpu_usage_percent = device.cpu_usage_percent;
	device_snapshot->door_open = app_is_door_open();
}

static void app_snapshot_fill_rtc(app_snapshot_t *snapshot, app_control_state_t *control)
{
	audio_stats_t audio = {0};
	camera_pipeline_metrics_t camera = {0};
	media_governor_video_config_t video_config = {0};
	rtc_transport_stats_t rtc = {0};

	if (snapshot == NULL || control == NULL) {
		return;
	}

	rtc_transport_get_stats(&rtc);
	camera_pipeline_get_metrics(&camera);
	media_governor_get_rtc_video_config(&video_config);
	*control = app_state_get_control();

	audio_device_get_stats(&audio);
	snapshot->rtc.connected = rtc.active_connection;
	snapshot->rtc.call_active = rtc.call_active;
	snapshot->rtc.incoming_call_pending = rtc.incoming_call_pending;
	snapshot->rtc.local_audio_send_enabled = rtc.local_audio_send_enabled;
	snapshot->rtc.state = (uint8_t)rtc.state;
	snapshot->rtc.tx_video_frames = rtc.tx_video_frames;
	snapshot->rtc.rx_video_frames = rtc.rx_video_frames;
	snapshot->rtc.tx_audio_frames = rtc.tx_audio_frames;
	snapshot->rtc.rx_audio_frames = rtc.rx_audio_frames;
	app_state_fill_rtc_frame_rates(&snapshot->rtc, &rtc);
	snapshot->rtc.tx_video_width = camera.width;
	snapshot->rtc.tx_video_height = camera.height;
	snapshot->rtc.tx_video_target_fps = camera.target_fps;
	snapshot->rtc.tx_video_configured_bitrate_kbps = camera.configured_bitrate_bps / 1000U;
	snapshot->rtc.tx_video_measured_fps_x10 = camera.measured_fps_x10;
	snapshot->rtc.tx_video_measured_bitrate_kbps = camera.measured_bitrate_kbps;
	if (!rtc.active_connection || !rtc.call_active) {
		snapshot->rtc.tx_video_width = 0U;
		snapshot->rtc.tx_video_height = 0U;
		snapshot->rtc.tx_video_target_fps = 0U;
		snapshot->rtc.tx_video_configured_bitrate_kbps = 0U;
		snapshot->rtc.tx_video_measured_fps_x10 = 0U;
		snapshot->rtc.tx_video_measured_bitrate_kbps = 0U;
	} else if (snapshot->rtc.tx_video_width == 0U) {
		snapshot->rtc.tx_video_width = video_config.width;
	}
	if (rtc.active_connection && rtc.call_active && snapshot->rtc.tx_video_height == 0U) {
		snapshot->rtc.tx_video_height = video_config.height;
	}
	if (rtc.active_connection && rtc.call_active && snapshot->rtc.tx_video_target_fps == 0U) {
		snapshot->rtc.tx_video_target_fps = video_config.fps;
	}
	if (rtc.active_connection && rtc.call_active && snapshot->rtc.tx_video_configured_bitrate_kbps == 0U) {
		snapshot->rtc.tx_video_configured_bitrate_kbps = video_config.bitrate_bps / 1000U;
	}

	snapshot->controls.video_enabled = control->video_enabled;
	snapshot->controls.audio_enabled = control->audio_enabled;
	snapshot->controls.effective_video_enabled = rtc.call_active && control->video_enabled;
	snapshot->controls.effective_audio_enabled = rtc.call_active &&
						    control->audio_enabled &&
						    audio.capture_gain_percent > 0U;
}

static void app_snapshot_fill_audio(app_audio_snapshot_t *audio_snapshot)
{
	audio_stats_t audio = {0};

	if (audio_snapshot == NULL) {
		return;
	}

	audio_device_get_stats(&audio);
	audio_snapshot->ready = audio.ready;
	audio_snapshot->speaker_enabled = audio.speaker_enabled;
	audio_snapshot->input_level = audio.input_level;
	audio_snapshot->output_level = audio.output_level;
	audio_snapshot->speaker_volume_percent = audio.speaker_volume_percent;
	audio_snapshot->capture_gain_percent = audio.capture_gain_percent;
}

static void app_snapshot_fill_test(app_test_snapshot_t *test_snapshot)
{
	sender_test_snapshot_t sender_test = {0};

	if (test_snapshot == NULL) {
		return;
	}

	sender_test_get_snapshot(&sender_test);
	test_snapshot->sender_running = sender_test.running;
	test_snapshot->sender_spiffs_ready = sender_test.spiffs_ready;
	strlcpy(test_snapshot->sender_status,
		sender_test.status,
		sizeof(test_snapshot->sender_status));
}

static void app_snapshot_fill_ota(app_ota_snapshot_t *ota_snapshot)
{
	ota_snapshot_t ota = {0};

	if (ota_snapshot == NULL) {
		return;
	}

	ota_get_snapshot(&ota);
	ota_snapshot->state = ota.state;
	ota_snapshot->running = ota.running;
	ota_snapshot->progress_percent = ota.progress_percent;
	ota_snapshot->bytes_read = ota.bytes_read;
	ota_snapshot->total_size = ota.total_size;
	ota_snapshot->last_error = ota.last_error;
	strlcpy(ota_snapshot->current_version, ota.current_version, sizeof(ota_snapshot->current_version));
	strlcpy(ota_snapshot->target_version, ota.target_version, sizeof(ota_snapshot->target_version));
	strlcpy(ota_snapshot->url, ota.url, sizeof(ota_snapshot->url));
	strlcpy(ota_snapshot->message, ota.message, sizeof(ota_snapshot->message));
}

static void app_snapshot_fill_ai_chat(app_ai_chat_snapshot_t *ai_snapshot)
{
	ai_chat_snapshot_t *ai = app_snapshot_ai_chat_buffer();

	if (ai_snapshot == NULL || ai == NULL) {
		return;
	}

	memset(ai, 0, sizeof(*ai));
	ai_chat_get_snapshot(ai);
	ai_snapshot->state = (uint8_t)ai->state;
	ai_snapshot->active = ai->active;
	ai_snapshot->listening = ai->listening;
	ai_snapshot->cloud_speaking = ai->cloud_speaking;
	ai_snapshot->video_active = ai->video_active;
	ai_snapshot->tx_audio_frames = ai->tx_audio_frames;
	ai_snapshot->tx_video_frames = ai->tx_video_frames;
	ai_snapshot->tx_video_failures = ai->tx_video_failures;
	ai_snapshot->rx_commands = ai->rx_commands;
	ai_snapshot->last_error = ai->last_error;
	strlcpy(ai_snapshot->asr_caption, ai->asr_caption, sizeof(ai_snapshot->asr_caption));
	strlcpy(ai_snapshot->tts_caption, ai->tts_caption, sizeof(ai_snapshot->tts_caption));
	ai_snapshot->message_count = ai->message_count > APP_AI_CHAT_MESSAGE_MAX ?
		APP_AI_CHAT_MESSAGE_MAX : ai->message_count;
	for (uint8_t index = 0; index < ai_snapshot->message_count; ++index) {
		ai_snapshot->messages[index].caption_type = ai->messages[index].caption_type;
		ai_snapshot->messages[index].utterance_id = ai->messages[index].utterance_id;
		strlcpy(ai_snapshot->messages[index].text,
			ai->messages[index].text,
			sizeof(ai_snapshot->messages[index].text));
	}
	strlcpy(ai_snapshot->status, ai->status, sizeof(ai_snapshot->status));
	ai_snapshot->avatar = app_ai_chat_config_get_avatar();
}

static void app_snapshot_fill_binding(app_device_binding_snapshot_t *binding_snapshot)
{
	device_binding_snapshot_t binding = {0};

	if (binding_snapshot == NULL) {
		return;
	}

	device_binding_get_snapshot(&binding);
	binding_snapshot->state = binding.state;
	binding_snapshot->running = binding.running;
	strlcpy(binding_snapshot->mac, binding.mac, sizeof(binding_snapshot->mac));
	strlcpy(binding_snapshot->code, binding.code, sizeof(binding_snapshot->code));
	strlcpy(binding_snapshot->device_id, binding.device_id, sizeof(binding_snapshot->device_id));
	binding_snapshot->last_error = binding.last_error;
	strlcpy(binding_snapshot->message, binding.message, sizeof(binding_snapshot->message));
}

static void app_snapshot_fill_online(app_device_online_snapshot_t *online_snapshot)
{
	device_online_snapshot_t online = {0};

	if (online_snapshot == NULL) {
		return;
	}

	device_online_get_snapshot(&online);
	online_snapshot->state = online.state;
	online_snapshot->running = online.running;
	online_snapshot->network_ready = online.network_ready;
	online_snapshot->bound = online.bound;
	online_snapshot->mqtt_connected = online.mqtt_connected;
	strlcpy(online_snapshot->device_id, online.device_id, sizeof(online_snapshot->device_id));
	online_snapshot->last_error = online.last_error;
	strlcpy(online_snapshot->message, online.message, sizeof(online_snapshot->message));
}

static void app_snapshot_fill_call_contacts(app_call_contacts_snapshot_t *contacts_snapshot)
{
	if (contacts_snapshot == NULL) {
		return;
	}

	app_get_call_contacts(contacts_snapshot);
}

static app_call_state_t app_snapshot_call_state_from_service(device_call_state_t state)
{
	switch (state) {
	case DEVICE_CALL_STATE_OUTGOING:
		return APP_CALL_STATE_OUTGOING;
	case DEVICE_CALL_STATE_INCOMING:
		return APP_CALL_STATE_INCOMING;
	case DEVICE_CALL_STATE_CONNECTING:
		return APP_CALL_STATE_CONNECTING;
	case DEVICE_CALL_STATE_IN_CALL:
		return APP_CALL_STATE_IN_CALL;
	case DEVICE_CALL_STATE_ERROR:
		return APP_CALL_STATE_ERROR;
	case DEVICE_CALL_STATE_IDLE:
	default:
		return APP_CALL_STATE_IDLE;
	}
}

static void app_snapshot_fill_call(app_snapshot_t *snapshot)
{
	device_call_snapshot_t call = {0};

	if (snapshot == NULL) {
		return;
	}

	device_call_get_snapshot(&call);
	snapshot->call.state = app_snapshot_call_state_from_service(call.state);
	snapshot->call.type = strcmp(call.call_type, "video") == 0 ?
		APP_CALL_TYPE_VIDEO : APP_CALL_TYPE_AUDIO;
	snapshot->call.pending_incoming = call.pending_incoming;
	snapshot->call.last_error = call.last_error;
	strlcpy(snapshot->call.peer_device_id,
		call.peer_device_id,
		sizeof(snapshot->call.peer_device_id));
	strlcpy(snapshot->call.room_id, call.room_id, sizeof(snapshot->call.room_id));
	strlcpy(snapshot->call.message, call.message, sizeof(snapshot->call.message));
	app_call_start_apply_snapshot(&snapshot->call);
	/* Device-call UI only consumes the ThingConnect incoming-call state. */
	snapshot->rtc.incoming_call_pending = snapshot->call.pending_incoming;
}

static app_wechat_call_state_t app_snapshot_wechat_call_state_from_service(wechat_voip_call_state_t state)
{
	switch (state) {
	case WECHAT_VOIP_CALL_STATE_INCOMING:
		return APP_WECHAT_CALL_STATE_INCOMING;
	case WECHAT_VOIP_CALL_STATE_CALLING:
		return APP_WECHAT_CALL_STATE_CALLING;
	case WECHAT_VOIP_CALL_STATE_CONNECTING:
		return APP_WECHAT_CALL_STATE_CONNECTING;
	case WECHAT_VOIP_CALL_STATE_IN_CALL:
		return APP_WECHAT_CALL_STATE_IN_CALL;
	case WECHAT_VOIP_CALL_STATE_CLOSING:
		return APP_WECHAT_CALL_STATE_CLOSING;
	case WECHAT_VOIP_CALL_STATE_IDLE:
	default:
		return APP_WECHAT_CALL_STATE_IDLE;
	}
}

static void app_snapshot_fill_wechat(app_wechat_snapshot_t *wechat_snapshot)
{
	wechat_voip_contacts_snapshot_t contacts = {0};
	uint8_t count = 0;

	if (wechat_snapshot == NULL) {
		return;
	}

	wechat_voip_service_get_contacts(&contacts);
	wechat_snapshot->contacts_ready = contacts.ready;
	wechat_snapshot->contacts_server_synced = contacts.server_synced;
	wechat_snapshot->contacts_last_error = contacts.last_error;
	wechat_snapshot->incoming_call_pending = wechat_voip_service_has_incoming_call();
	wechat_snapshot->call_state =
		app_snapshot_wechat_call_state_from_service(wechat_voip_service_get_call_state());
	count = contacts.count > APP_WECHAT_CONTACT_MAX ? APP_WECHAT_CONTACT_MAX : contacts.count;
	wechat_snapshot->count = count;
	for (uint8_t index = 0; index < count; ++index) {
		strlcpy(wechat_snapshot->contacts[index].open_id,
			contacts.contacts[index].open_id,
			sizeof(wechat_snapshot->contacts[index].open_id));
		strlcpy(wechat_snapshot->contacts[index].remark,
			contacts.contacts[index].remark,
			sizeof(wechat_snapshot->contacts[index].remark));
	}
}

void app_snapshot_get(app_snapshot_t *snapshot)
{
	app_control_state_t control = {0};

	if (snapshot == NULL) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));

	app_snapshot_fill_network(&snapshot->network);
	app_snapshot_fill_device(&snapshot->device);
	app_snapshot_fill_rtc(snapshot, &control);
	app_get_rtc_config_snapshot(&snapshot->rtc_config);
	app_snapshot_fill_binding(&snapshot->binding);
	app_snapshot_fill_online(&snapshot->online);
	app_snapshot_fill_audio(&snapshot->audio);
	app_snapshot_fill_test(&snapshot->test);
	app_snapshot_fill_ota(&snapshot->ota);
	app_snapshot_fill_ai_chat(&snapshot->ai_chat);
	app_snapshot_fill_call(snapshot);
	app_snapshot_fill_call_contacts(&snapshot->call_contacts);
	app_snapshot_fill_wechat(&snapshot->wechat);
}
