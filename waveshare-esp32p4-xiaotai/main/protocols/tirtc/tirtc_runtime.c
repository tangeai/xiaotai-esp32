#include "tirtc_session_internal.h"

#include "esp_log.h"

static const char *TAG = "tirtc_session_rt";

static void tirtc_session_handle_network_changed(const tirtc_session_event_t *event)
{
    if (!event->payload.network.state.connected) {
        tirtc_conn_t active_conn = NULL;

        tirtc_session_note_event("network down");
        if (tirtc_session_try_get_active_conn(&active_conn)) {
            esp_err_t ret = tirtc_session_disconnect();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "rtc disconnect on network down failed: %s", esp_err_to_name(ret));
            }
        }
        /* TiRtcStart owns listener threads and sockets. Merely clearing the
         * application-side started flag lets the next network-up event call
         * TiRtcStart again without the required TiRtcStop/TiRtcUninit pair,
         * leaving the old runtime alive and eventually exhausting lwIP
         * sockets. Reuse the deferred full-reset path so callbacks drain
         * outside the network callback and restart only after link recovery. */
        if (!tirtc_session_schedule_deferred_full_reset()) {
            ESP_LOGE(TAG, "rtc full reset schedule failed on network down");
            tirtc_session_mark_sdk_network_offline();
        }
    } else {
        tirtc_session_note_event("network up");
        (void)tirtc_session_prepare_sdk();
    }
}

static void tirtc_session_handle_conn_accepted(const tirtc_session_event_t *event)
{
    bool auto_media = true;
    bool media_deferred = false;

    /* Producers accept or reject the handle before queuing this event. If
     * ownership changed while it waited, the handle is already closing or
     * closed; another disconnect would double-destroy the SDK connection. */
    if (!tirtc_session_is_connection_usable(event->payload.conn.conn)) {
        tirtc_session_note_event("stale conn accept");
        ESP_LOGI(TAG,
                 "rtc accepted event ignored after lifecycle changed: hconn=%p",
                 event->payload.conn.conn);
        return;
    }

    auto_media = tirtc_session_connection_auto_media_enabled(event->payload.conn.conn) ||
                 tirtc_session_is_test_media_active();
    media_deferred = tirtc_session_connection_media_deferred(event->payload.conn.conn);
    ESP_LOGI(TAG,
             "rtc connection accepted: hconn=%p auto_media=%d defer_media=%d",
             event->payload.conn.conn,
             auto_media ? 1 : 0,
             media_deferred ? 1 : 0);
    esp_err_t bitrate_ret =
        tirtc_session_apply_video_bitrate_params(event->payload.conn.conn);
    if (bitrate_ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG,
                 "TGMP unavailable for this build/connection; continue with normal video profile");
    } else if (bitrate_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "rtc video bitrate control registration failed; continue with normal profile: %s",
                 esp_err_to_name(bitrate_ret));
    }

    /* Capture the connection policy before notifying application observers.
     * A device-call observer may promote deferred media immediately when the
     * cloud answer arrived first. That promotion clears the live defer flag;
     * it must not make this generic path reinterpret the session as AV and
     * force the camera on for an audio-only call. */
    tirtc_session_notify_connection_accepted(event->payload.conn.conn);
    if (!tirtc_session_is_connection_usable(event->payload.conn.conn)) {
        tirtc_session_note_event("conn closed by owner");
        return;
    }

    if (!auto_media) {
        if (tirtc_session_get_session_mode() == TIRTC_SESSION_MODE_CONNECT) {
            esp_err_t call_ret = tirtc_session_request_call();
            if (call_ret != ESP_OK) {
                tirtc_session_note_event("call req fail");
                ESP_LOGW(TAG, "rtc outgoing call request failed: %s", esp_err_to_name(call_ret));
                (void)tirtc_session_disconnect();
                return;
            }
            tirtc_session_note_event("call req");
            ESP_LOGI(TAG, "rtc outgoing call request sent");
            return;
        }

        tirtc_session_note_event("connected");
        return;
    }

    if (media_deferred) {
        if (tirtc_session_connection_media_deferred(event->payload.conn.conn)) {
            tirtc_session_complete_call_response_without_media(true);
            tirtc_session_note_event("connected wait cmd");
            ESP_LOGI(TAG,
                     "rtc connection accepted: hconn=%p media deferred until device-call command",
                     event->payload.conn.conn);
        } else {
            tirtc_session_note_event("connected owner media");
            ESP_LOGI(TAG,
                     "rtc connection accepted: hconn=%p deferred media already activated by owner",
                     event->payload.conn.conn);
        }
        return;
    }

    tirtc_session_complete_call_response(true);
    (void)tirtc_session_set_local_video_send_enabled(true);
    (void)tirtc_session_set_local_audio_send_enabled(true);
    tirtc_session_apply_local_media_policy();
    bool audio_test_only = tirtc_session_is_test_audio_active() &&
                           !tirtc_session_is_test_video_active();
    if (audio_test_only) {
        tirtc_session_stop_time_stream_messages();
    }
    tirtc_session_request_test_audio_restart();
    tirtc_session_note_event("connected");
    if (!audio_test_only) {
        tirtc_session_start_time_stream_messages();
    }
}

static void tirtc_session_handle_video_subscription(const tirtc_session_event_t *event, bool subscribed)
{
    tirtc_session_update_local_video_subscription(event->payload.subscribe.conn,
                                                 event->payload.subscribe.stream_id,
                                                 subscribed);
    tirtc_session_note_event(subscribed ? "video subscribed" : "video unsubscribed");
    ESP_LOGI(TAG,
             "remote local video subscription: stream=%u subscribed=%d",
             event->payload.subscribe.stream_id,
             subscribed);
    tirtc_session_apply_local_media_policy();
}

static void tirtc_session_handle_audio_subscription(const tirtc_session_event_t *event, bool subscribed)
{
    tirtc_session_update_local_audio_subscription(event->payload.subscribe.conn,
                                                 event->payload.subscribe.stream_id,
                                                 subscribed);
    tirtc_session_note_event(subscribed ? "audio subscribed" : "audio unsubscribed");
    ESP_LOGI(TAG,
             "remote local audio subscription: stream=%u subscribed=%d",
             event->payload.subscribe.stream_id,
             subscribed);
    tirtc_session_apply_local_media_policy();
}

void tirtc_session_handle_runtime_event(const tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TIRTC_SESSION_EVENT_NETWORK_CHANGED:
        tirtc_session_handle_network_changed(event);
        break;
    case TIRTC_SESSION_EVENT_SYS_STARTED:
        tirtc_session_mark_sdk_started();
        tirtc_session_note_event("rtc started");
        ESP_LOGI(TAG, "rtc system started");
        if (tirtc_session_get_session_mode() == TIRTC_SESSION_MODE_CONNECT) {
            (void)tirtc_session_start_configured_peer_connect();
        }
        break;
    case TIRTC_SESSION_EVENT_CONNECT_PEER:
        (void)tirtc_session_start_configured_peer_connect();
        break;
    case TIRTC_SESSION_EVENT_SYS_STOPPED:
        if (!tirtc_session_mark_sdk_stopped(event->payload.system.generation)) {
            break;
        }
        tirtc_session_note_event("rtc stopped");
        (void)tirtc_session_consume_runtime_restart_request(NULL);
        break;
    case TIRTC_SESSION_EVENT_DEFERRED_FULL_RESET:
        tirtc_session_handle_deferred_full_reset();
        break;
    case TIRTC_SESSION_EVENT_DEFERRED_START_AFTER_FULL_RESET:
        tirtc_session_handle_deferred_start_after_full_reset();
        break;
    case TIRTC_SESSION_EVENT_ACCESS_HIJACKING:
    {
        tirtc_conn_t active_conn = NULL;

        tirtc_session_mark_access_hijacking_detected();
        if (tirtc_session_try_get_active_conn(&active_conn)) {
            (void)tirtc_session_disconnect();
        }
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        tirtc_session_note_event("access hijack");
        ESP_LOGE(TAG, "rtc access hijacking detected");
        break;
    }
    case TIRTC_SESSION_EVENT_CONN_ACCEPTED:
        tirtc_session_handle_conn_accepted(event);
        break;
    case TIRTC_SESSION_EVENT_MEDIA_BOOTSTRAP:
        tirtc_session_run_media_bootstrap();
        break;
    case TIRTC_SESSION_EVENT_CONN_ERROR:
        tirtc_session_handle_connection_loss(event->payload.conn.conn, event->payload.conn.error);
        break;
    case TIRTC_SESSION_EVENT_DISCONNECTED:
        tirtc_session_handle_connection_loss(event->payload.conn.conn, 0);
        break;
    case TIRTC_SESSION_EVENT_SUBSCRIBE_VIDEO:
        tirtc_session_handle_video_subscription(event, true);
        break;
    case TIRTC_SESSION_EVENT_UNSUBSCRIBE_VIDEO:
        tirtc_session_handle_video_subscription(event, false);
        break;
    case TIRTC_SESSION_EVENT_SUBSCRIBE_AUDIO:
        tirtc_session_handle_audio_subscription(event, true);
        break;
    case TIRTC_SESSION_EVENT_UNSUBSCRIBE_AUDIO:
        tirtc_session_handle_audio_subscription(event, false);
        break;
    default:
        break;
    }
}
