#include "tirtc_session_internal.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "system_time.h"

static const char *TAG = "tirtc_session_cmd";

#define TIRTC_SESSION_CALL_ALLOW_TEXT  "ALLOW"
#define TIRTC_SESSION_CALL_REJECT_TEXT "REJECT"
#define TIRTC_SESSION_CALL_BUSY_TEXT   "BUSY"
#define TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT      "OK"
#define TIRTC_SESSION_MEDIA_TOGGLE_NO_CALL_TEXT "NO_CALL"
#define TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE 24U
#define TIRTC_SESSION_CLOCK_SYNC_REQUEST  1U
#define TIRTC_SESSION_CLOCK_SYNC_RESPONSE 2U
#define TIRTC_SESSION_WEB_CONTROL_DUMP_PAYLOAD_MAX 32U
#define TIRTC_SESSION_WEB_CONTROL_DUMP_WIRE_MAX    (8U + TIRTC_SESSION_WEB_CONTROL_DUMP_PAYLOAD_MAX)

static bool s_clock_sync_wall_clock_warned;

typedef struct {
    const uint8_t *data;
    size_t data_len;
    size_t raw_len;
    size_t trimmed_len;
} tirtc_session_web_payload_view_t;

static bool tirtc_session_parse_web_bool_assignment(const void *data,
                                                    size_t data_len,
                                                    char key,
                                                    bool *value);

static uint32_t tirtc_session_load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void tirtc_session_store_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
    data[2] = (uint8_t)((value >> 16) & 0xFFU);
    data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void tirtc_session_append_hex_byte(char *out, size_t out_len, size_t *offset, uint8_t value)
{
    static const char hex[] = "0123456789abcdef";

    if (out == NULL || out_len == 0 || offset == NULL || *offset >= out_len) {
        return;
    }

    if (*offset > 0 && *offset + 1U < out_len) {
        out[(*offset)++] = ' ';
    }

    if (*offset + 2U < out_len) {
        out[(*offset)++] = hex[(value >> 4) & 0x0FU];
        out[(*offset)++] = hex[value & 0x0FU];
    }

    out[*offset < out_len ? *offset : out_len - 1U] = '\0';
}

static void tirtc_session_log_web_control_packet(const char *name,
                                                uint32_t cmdw,
                                                const tirtc_session_web_payload_view_t *view)
{
    uint8_t header[8] = {0};
    char wire_hex[(TIRTC_SESSION_WEB_CONTROL_DUMP_WIRE_MAX * 3U) + 1U] = {0};
    char payload_ascii[TIRTC_SESSION_WEB_CONTROL_DUMP_PAYLOAD_MAX + 1U] = {0};
    const uint8_t *payload = view != NULL ? view->data : NULL;
    size_t data_len = view != NULL ? view->data_len : 0;
    size_t raw_len = view != NULL ? view->raw_len : 0;
    size_t trimmed_len = view != NULL ? view->trimmed_len : 0;
    size_t payload_dump_len = data_len < TIRTC_SESSION_WEB_CONTROL_DUMP_PAYLOAD_MAX ?
                                  data_len :
                                  TIRTC_SESSION_WEB_CONTROL_DUMP_PAYLOAD_MAX;
    size_t hex_offset = 0;

    tirtc_session_store_le32(header, cmdw);
    tirtc_session_store_le32(header + 4, (uint32_t)data_len);

    for (size_t index = 0; index < sizeof(header); ++index) {
        tirtc_session_append_hex_byte(wire_hex, sizeof(wire_hex), &hex_offset, header[index]);
    }

    if (payload == NULL) {
        payload_dump_len = 0;
    }

    for (size_t index = 0; index < payload_dump_len; ++index) {
        uint8_t value = payload[index];
        tirtc_session_append_hex_byte(wire_hex, sizeof(wire_hex), &hex_offset, value);
        payload_ascii[index] = (value >= 0x20U && value <= 0x7EU) ? (char)value : '.';
    }

    ESP_LOGD(TAG,
             "web %s packet: cmdw=0x%08lx payload_len=%lu raw_len=%lu trim=%lu wire_len=%lu bytes=%s payload=\"%s\"%s",
             name != NULL ? name : "control",
             (unsigned long)cmdw,
             (unsigned long)data_len,
             (unsigned long)raw_len,
             (unsigned long)trimmed_len,
             (unsigned long)(8U + data_len),
             wire_hex,
             payload_ascii,
             data_len > payload_dump_len ? " ..." : "");
}

static bool tirtc_session_make_web_payload_view(const void *data,
                                                size_t data_len,
                                                tirtc_session_web_payload_view_t *view)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (view == NULL) {
        return false;
    }

    view->data = bytes;
    view->data_len = data_len;
    view->raw_len = data_len;
    view->trimmed_len = 0;

    if (bytes == NULL) {
        return false;
    }

    if (data_len >= sizeof(uint32_t)) {
        uint32_t embedded_len = tirtc_session_load_le32(bytes);

        if (embedded_len <= data_len - sizeof(uint32_t)) {
            view->data = bytes + sizeof(uint32_t);
            view->data_len = embedded_len;
            view->trimmed_len = sizeof(uint32_t);
        }
    }

    return true;
}

static bool tirtc_session_parse_clock_sync_request(const void *data,
                                                  size_t data_len,
                                                  uint32_t *seq,
                                                  uint32_t *t1_ms)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (bytes == NULL || seq == NULL || t1_ms == NULL ||
        data_len < TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE) {
        return false;
    }

    if (bytes[0] != 'T' || bytes[1] != 'C' ||
        bytes[2] != 'L' || bytes[3] != 'K' ||
        bytes[4] != 1U || bytes[5] != TIRTC_SESSION_CLOCK_SYNC_REQUEST) {
        return false;
    }

    *seq = tirtc_session_load_le32(bytes + 8);
    *t1_ms = tirtc_session_load_le32(bytes + 12);
    return true;
}

static bool tirtc_session_parse_clock_sync_response(const void *data,
                                                   size_t data_len,
                                                   uint32_t *seq,
                                                   uint32_t *t1_ms,
                                                   uint32_t *t2_ms,
                                                   uint32_t *t3_ms)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (bytes == NULL || seq == NULL || t1_ms == NULL ||
        t2_ms == NULL || t3_ms == NULL ||
        data_len < TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE) {
        return false;
    }

    if (bytes[0] != 'T' || bytes[1] != 'C' ||
        bytes[2] != 'L' || bytes[3] != 'K' ||
        bytes[4] != 1U || bytes[5] != TIRTC_SESSION_CLOCK_SYNC_RESPONSE) {
        return false;
    }

    *seq = tirtc_session_load_le32(bytes + 8);
    *t1_ms = tirtc_session_load_le32(bytes + 12);
    *t2_ms = tirtc_session_load_le32(bytes + 16);
    *t3_ms = tirtc_session_load_le32(bytes + 20);
    return true;
}

static void tirtc_session_build_clock_sync_response(uint8_t payload[TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE],
                                                   uint32_t seq,
                                                   uint32_t t1_ms,
                                                   uint32_t t2_ms,
                                                   uint32_t t3_ms)
{
    memset(payload, 0, TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE);
    payload[0] = 'T';
    payload[1] = 'C';
    payload[2] = 'L';
    payload[3] = 'K';
    payload[4] = 1U;
    payload[5] = TIRTC_SESSION_CLOCK_SYNC_RESPONSE;
    tirtc_session_store_le32(payload + 8, seq);
    tirtc_session_store_le32(payload + 12, t1_ms);
    tirtc_session_store_le32(payload + 16, t2_ms);
    tirtc_session_store_le32(payload + 20, t3_ms);
}

static uint32_t tirtc_session_get_unix_time_ms_low32_for_command(bool *valid)
{
    struct timeval tv = {0};
    uint64_t unix_ms = 0;
    bool clock_valid = false;

    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= 0 && tv.tv_usec >= 0) {
        unix_ms = ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
        clock_valid = system_time_has_valid_time();
    }

    if (valid != NULL) {
        *valid = clock_valid;
    }

    return (uint32_t)(unix_ms & 0xFFFFFFFFULL);
}

static bool tirtc_session_is_test_media_command_active(void)
{
    return tirtc_session_is_test_media_active();
}

static uint32_t tirtc_session_get_unix_time_s_for_command(void)
{
    time_t now = 0;

    time(&now);
    if (now < 0) {
        return 0;
    }

    return (uint32_t)now;
}

static void tirtc_session_pack_unix_time_le(uint32_t unix_time_s, uint8_t payload[4])
{
    payload[0] = (uint8_t)(unix_time_s & 0xFFU);
    payload[1] = (uint8_t)((unix_time_s >> 8) & 0xFFU);
    payload[2] = (uint8_t)((unix_time_s >> 16) & 0xFFU);
    payload[3] = (uint8_t)((unix_time_s >> 24) & 0xFFU);
}

static bool tirtc_session_is_showcase_command_id(uint16_t cmd_id)
{
    return cmd_id == TIRTC_SESSION_CMD_CALL || cmd_id == TIRTC_SESSION_CMD_VOLUME ||
           cmd_id == TIRTC_SESSION_CMD_DOOR || cmd_id == TIRTC_SESSION_CMD_HANGUP ||
           cmd_id == TIRTC_SESSION_CMD_REQ_VIDEO || cmd_id == TIRTC_SESSION_CMD_REQ_AUDIO ||
           cmd_id == TIRTC_SESSION_CMD_SET_SEND_VIDEO || cmd_id == TIRTC_SESSION_CMD_SET_SEND_AUDIO ||
           cmd_id == TIRTC_SESSION_CMD_RGB_LEGACY || cmd_id == TIRTC_SESSION_CMD_STATE_LEGACY ||
           cmd_id == TIRTC_SESSION_CMD_TIME_QUERY;
}

static bool tirtc_session_is_showcase_direct_cmdw(uint32_t cmdw)
{
    return tirtc_session_is_showcase_command_id((uint16_t)(cmdw & 0x7FFFU));
}

static uint16_t tirtc_session_get_command_id(uint32_t cmdw)
{
    if (tirtc_session_is_showcase_direct_cmdw(cmdw)) {
        return (uint16_t)(cmdw & 0x7FFFU);
    }

    return (uint16_t)GET_CMD(cmdw);
}

static bool tirtc_session_is_command_response(uint32_t cmdw)
{
    if (tirtc_session_is_showcase_direct_cmdw(cmdw)) {
        return (cmdw & TIRTC_SESSION_CMD_RESP_BIT) != 0U;
    }

    return (cmdw & RESPONSE_BIT) != 0U;
}

static uint32_t tirtc_session_make_showcase_cmdw(uint16_t cmd_id, uint16_t sn, bool response)
{
    return ((uint32_t)sn << 16) | ((uint32_t)cmd_id & 0x7FFFU) |
           (response ? TIRTC_SESSION_CMD_RESP_BIT : 0U);
}

static uint32_t tirtc_session_make_command_response_word(uint32_t request_cmdw)
{
    if (tirtc_session_is_showcase_direct_cmdw(request_cmdw)) {
        return request_cmdw | TIRTC_SESSION_CMD_RESP_BIT;
    }

    return request_cmdw | RESPONSE_BIT;
}

static void tirtc_session_peer_state_to_payload(const tirtc_session_peer_state_t *state,
                                               tirtc_session_peer_state_payload_t *payload)
{
    if (state == NULL || payload == NULL) {
        return;
    }

    memset(payload, 0, sizeof(*payload));
    payload->call_active = state->call_active;
    payload->local_video_send_enabled = state->local_video_send_enabled;
    payload->local_audio_send_enabled = state->local_audio_send_enabled;
    payload->video_stream_active = state->video_stream_active;
    payload->audio_stream_active = state->audio_stream_active;
    memcpy(payload->rgb, state->rgb, sizeof(payload->rgb));
}

static void tirtc_session_payload_to_peer_state(const tirtc_session_peer_state_payload_t *payload,
                                               tirtc_session_peer_state_t *state)
{
    if (payload == NULL || state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->call_active = payload->call_active != 0;
    state->local_video_send_enabled = payload->local_video_send_enabled != 0;
    state->local_audio_send_enabled = payload->local_audio_send_enabled != 0;
    state->video_stream_active = payload->video_stream_active != 0;
    state->audio_stream_active = payload->audio_stream_active != 0;
    memcpy(state->rgb, payload->rgb, sizeof(state->rgb));
}

static bool tirtc_session_toggle_from_payload(const void *data, size_t data_len, bool default_enabled)
{
    if (data == NULL || data_len < sizeof(tirtc_session_toggle_payload_t)) {
        return default_enabled;
    }

    const tirtc_session_toggle_payload_t *payload = (const tirtc_session_toggle_payload_t *)data;
    return payload->enabled != 0;
}

static bool tirtc_session_text_equals(const uint8_t *data,
                                      size_t data_len,
                                      const char *text)
{
    size_t text_len = text != NULL ? strlen(text) : 0;

    return data != NULL && text != NULL && data_len == text_len &&
           memcmp(data, text, text_len) == 0;
}

static bool tirtc_session_toggle_from_command_payload(const void *data,
                                                     size_t data_len,
                                                     bool default_enabled,
                                                     char assignment_key)
{
    tirtc_session_web_payload_view_t payload = {0};
    bool enabled = default_enabled;

    if (!tirtc_session_make_web_payload_view(data, data_len, &payload) ||
        payload.data == NULL || payload.data_len == 0) {
        return default_enabled;
    }

    if (assignment_key != '\0' &&
        tirtc_session_parse_web_bool_assignment(payload.data,
                                                payload.data_len,
                                                assignment_key,
                                                &enabled)) {
        return enabled;
    }
    if (tirtc_session_parse_web_bool_assignment(payload.data,
                                                payload.data_len,
                                                'e',
                                                &enabled)) {
        return enabled;
    }

    if (payload.data_len == 1) {
        if (payload.data[0] == '0' || payload.data[0] == 0U) {
            return false;
        }
        if (payload.data[0] == '1' || payload.data[0] == 1U) {
            return true;
        }
    }

    if (tirtc_session_text_equals(payload.data, payload.data_len, "true") ||
        tirtc_session_text_equals(payload.data, payload.data_len, "on")) {
        return true;
    }
    if (tirtc_session_text_equals(payload.data, payload.data_len, "false") ||
        tirtc_session_text_equals(payload.data, payload.data_len, "off")) {
        return false;
    }

    return tirtc_session_toggle_from_payload(payload.data, payload.data_len, default_enabled);
}

static bool tirtc_session_text_response_equals(const void *data, size_t data_len, const char *text)
{
    size_t text_len = text != NULL ? strlen(text) : 0;

    return data != NULL && text != NULL && data_len == text_len && memcmp(data, text, text_len) == 0;
}

static bool tirtc_session_media_toggle_response_is_no_call(const void *data, size_t data_len)
{
    return tirtc_session_text_response_equals(data, data_len, TIRTC_SESSION_MEDIA_TOGGLE_NO_CALL_TEXT);
}

static bool tirtc_session_media_toggle_response_enabled(const void *data, size_t data_len)
{
    if (tirtc_session_text_response_equals(data, data_len, TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT)) {
        return true;
    }

    if (tirtc_session_media_toggle_response_is_no_call(data, data_len)) {
        return false;
    }

    return tirtc_session_toggle_from_payload(data, data_len, true);
}

static bool tirtc_session_is_payload_tail_space_or_nul(const uint8_t *bytes, size_t offset, size_t data_len)
{
    while (offset < data_len) {
        if (bytes[offset] != '\0' && bytes[offset] != ' ' &&
            bytes[offset] != '\r' && bytes[offset] != '\n' &&
            bytes[offset] != '\t') {
            return false;
        }
        ++offset;
    }
    return true;
}

static bool tirtc_session_parse_decimal_u8(const uint8_t *bytes,
                                           size_t *offset,
                                           size_t data_len,
                                           uint8_t max_value,
                                           uint8_t *value)
{
    uint32_t parsed = 0;
    bool has_digit = false;
    size_t pos = offset != NULL ? *offset : 0;

    if (bytes == NULL || offset == NULL || value == NULL) {
        return false;
    }

    while (pos < data_len && bytes[pos] >= '0' && bytes[pos] <= '9') {
        has_digit = true;
        parsed = (parsed * 10U) + (uint32_t)(bytes[pos] - '0');
        if (parsed > max_value) {
            return false;
        }
        ++pos;
    }

    if (!has_digit) {
        return false;
    }

    *offset = pos;
    *value = (uint8_t)parsed;
    return true;
}

static bool tirtc_session_parse_web_u8_assignment(const void *data,
                                                 size_t data_len,
                                                 char key,
                                                 uint8_t max_value,
                                                 uint8_t *value)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 2;

    if (bytes == NULL || value == NULL) {
        return false;
    }

    if (data_len < 3 || bytes[0] != (uint8_t)key || bytes[1] != '=' ||
        !tirtc_session_parse_decimal_u8(bytes, &offset, data_len, max_value, value)) {
        return false;
    }

    return tirtc_session_is_payload_tail_space_or_nul(bytes, offset, data_len);
}

static bool tirtc_session_parse_web_bool_assignment(const void *data, size_t data_len, char key, bool *value)
{
    uint8_t parsed = 0;

    if (value == NULL || !tirtc_session_parse_web_u8_assignment(data, data_len, key, 1U, &parsed)) {
        return false;
    }

    *value = parsed != 0U;
    return true;
}

static bool tirtc_session_call_response_is_allow(const void *data, size_t data_len)
{
    size_t allow_len = strlen(TIRTC_SESSION_CALL_ALLOW_TEXT);

    if (data != NULL && data_len == allow_len && memcmp(data, TIRTC_SESSION_CALL_ALLOW_TEXT, allow_len) == 0) {
        return true;
    }

    return data != NULL && data_len >= sizeof(tirtc_session_call_reply_t) &&
           ((const tirtc_session_call_reply_t *)data)->accepted != 0;
}

static esp_err_t tirtc_session_send_request(uint16_t cmd, const void *data, size_t data_len);

esp_err_t tirtc_session_send_media_toggle_request(uint16_t cmd, bool enabled)
{
    tirtc_session_toggle_payload_t payload = {
        .enabled = enabled ? 1U : 0U,
    };

    return tirtc_session_send_request(cmd, &payload, sizeof(payload));
}

esp_err_t tirtc_session_request_call(void)
{
    return tirtc_session_send_request(TIRTC_SESSION_CMD_CALL, NULL, 0);
}

static esp_err_t tirtc_session_send_request(uint16_t cmd, const void *data, size_t data_len)
{
    tirtc_conn_t conn = NULL;
    uint32_t cmdw = 0;

    ESP_RETURN_ON_FALSE(tirtc_session_try_get_active_conn(&conn), ESP_ERR_INVALID_STATE, TAG, "rtc connection not ready");
    cmdw = tirtc_session_make_showcase_cmdw(cmd, (uint16_t)atomic_get_cmd_sn(), false);
    return tirtc_session_send_command_raw(conn, cmdw, data, data_len);
}

esp_err_t tirtc_session_send_active_command(uint32_t cmdw, const void *data, size_t data_len)
{
    tirtc_conn_t conn = NULL;

    ESP_RETURN_ON_FALSE(tirtc_session_try_get_active_conn(&conn),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc connection not ready");
    return tirtc_session_send_command_raw(conn, cmdw, data, data_len);
}

static esp_err_t tirtc_session_send_response(tirtc_conn_t conn, uint32_t request_cmdw, const void *data, size_t data_len)
{
    return tirtc_session_send_command_raw(conn,
                                         tirtc_session_make_command_response_word(request_cmdw),
                                         data,
                                         data_len);
}

static esp_err_t tirtc_session_send_call_response_text(tirtc_conn_t conn, uint32_t request_cmdw, const char *text)
{
    size_t text_len = text != NULL ? strlen(text) : 0;

    return tirtc_session_send_response(conn, request_cmdw, text, text_len);
}

void tirtc_session_handle_remote_command(const tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    tirtc_conn_t active_conn = NULL;
    if (!tirtc_session_try_get_active_conn(&active_conn) || active_conn != event->payload.command.conn) {
        ESP_LOGW(TAG,
                 "remote command ignored: inactive connection hconn=%p active=%p cmdw=0x%08lx",
                 event->payload.command.conn,
                 active_conn,
                 (unsigned long)event->payload.command.cmdw);
        return;
    }

    uint16_t cmd = tirtc_session_get_command_id(event->payload.command.cmdw);
    bool is_response = tirtc_session_is_command_response(event->payload.command.cmdw);

    switch (cmd) {
    case TIRTC_SESSION_CMD_CALL:
        if (is_response) {
            bool accepted = tirtc_session_call_response_is_allow(event->payload.command.data,
                                                                event->payload.command.data_len);
            tirtc_session_complete_call_response(accepted);
            tirtc_session_note_event(accepted ? "call accepted" : "call rejected");
            if (accepted) {
                (void)tirtc_session_set_local_video_send_enabled(true);
                (void)tirtc_session_set_local_audio_send_enabled(true);
                tirtc_session_apply_local_media_policy();
            } else {
                tirtc_session_apply_local_media_policy();
                if (tirtc_session_get_session_mode() == TIRTC_SESSION_MODE_CONNECT) {
                    (void)tirtc_session_disconnect();
                }
            }
        } else {
            tirtc_session_peer_state_t local_state = {0};
            uint32_t pending_cmdw = 0;

            tirtc_session_get_local_peer_state(&local_state);
            tirtc_session_get_pending_call(NULL, &pending_cmdw);

            if (local_state.call_active) {
                if (tirtc_session_send_call_response_text(event->payload.command.conn,
                                                         event->payload.command.cmdw,
                                                         TIRTC_SESSION_CALL_ALLOW_TEXT) != ESP_OK) {
                    ESP_LOGW(TAG, "incoming call response failed: already active");
                }
                tirtc_session_note_event("call already active");
                break;
            }
            if (pending_cmdw != 0) {
                if (tirtc_session_send_call_response_text(event->payload.command.conn,
                                                         event->payload.command.cmdw,
                                                         TIRTC_SESSION_CALL_BUSY_TEXT) != ESP_OK) {
                    ESP_LOGW(TAG, "incoming call busy response failed");
                }
                tirtc_session_note_event("call reject busy");
                break;
            }

            tirtc_session_mark_incoming_call(event->payload.command.cmdw);
            tirtc_session_note_event("call incoming");
        }
        break;
    case TIRTC_SESSION_CMD_VOLUME: {
        tirtc_session_web_payload_view_t payload = {0};
        uint8_t volume = 0;

        if (is_response) {
            tirtc_session_note_event("volume rsp");
            break;
        }

        if (!tirtc_session_make_web_payload_view(event->payload.command.data,
                                                 event->payload.command.data_len,
                                                 &payload)) {
            ESP_LOGW(TAG,
                     "web volume command ignored: empty payload cmdw=0x%08lx len=%lu",
                     (unsigned long)event->payload.command.cmdw,
                     (unsigned long)event->payload.command.data_len);
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("volume invalid");
            break;
        }

        if (!tirtc_session_parse_web_u8_assignment(payload.data,
                                                  payload.data_len,
                                                  'v',
                                                  100U,
                                                  &volume)) {
            ESP_LOGW(TAG,
                     "web volume command ignored: invalid payload cmdw=0x%08lx payload_len=%lu raw_len=%lu",
                     (unsigned long)event->payload.command.cmdw,
                     (unsigned long)payload.data_len,
                     (unsigned long)payload.raw_len);
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("volume invalid");
            break;
        }

        esp_err_t ret = tirtc_session_apply_remote_volume_command(volume);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "web volume command failed: volume=%u err=%s", volume, esp_err_to_name(ret));
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("volume failed");
            break;
        }

        (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                   event->payload.command.cmdw,
                                                   TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT);
        ESP_LOGI(TAG, "web volume command accepted: volume=%u", volume);
        tirtc_session_note_event("volume rx");
        break;
    }
    case TIRTC_SESSION_CMD_DOOR: {
        tirtc_session_web_payload_view_t payload = {0};
        bool door_open = false;

        if (is_response) {
            tirtc_session_note_event("door rsp");
            break;
        }

        if (!tirtc_session_make_web_payload_view(event->payload.command.data,
                                                 event->payload.command.data_len,
                                                 &payload)) {
            ESP_LOGW(TAG,
                     "web door command ignored: empty payload cmdw=0x%08lx len=%lu",
                     (unsigned long)event->payload.command.cmdw,
                     (unsigned long)event->payload.command.data_len);
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("door invalid");
            break;
        }

        tirtc_session_log_web_control_packet("door",
                                             event->payload.command.cmdw,
                                             &payload);

        if (!tirtc_session_parse_web_bool_assignment(payload.data,
                                                    payload.data_len,
                                                    'd',
                                                    &door_open)) {
            ESP_LOGW(TAG,
                     "web door command ignored: invalid payload cmdw=0x%08lx payload_len=%lu raw_len=%lu",
                     (unsigned long)event->payload.command.cmdw,
                     (unsigned long)payload.data_len,
                     (unsigned long)payload.raw_len);
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("door invalid");
            break;
        }

        esp_err_t ret = tirtc_session_apply_remote_door_command(door_open);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "web door command failed: open=%d err=%s", door_open ? 1 : 0, esp_err_to_name(ret));
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       "ERR");
            tirtc_session_note_event("door failed");
            break;
        }

        (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                   event->payload.command.cmdw,
                                                   TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT);
        ESP_LOGI(TAG,
                 "web door command: open=%d cmdw=0x%08lx",
                 door_open ? 1 : 0,
                 (unsigned long)event->payload.command.cmdw);
        tirtc_session_note_event(door_open ? "door open" : "door locked");
        break;
    }
    case TIRTC_SESSION_CMD_RGB_LEGACY:
        if (event->payload.command.data_len >= sizeof(tirtc_session_rgb_payload_t)) {
            const tirtc_session_rgb_payload_t *payload = (const tirtc_session_rgb_payload_t *)event->payload.command.data;

            tirtc_session_set_peer_rgb(payload->red, payload->green, payload->blue);
            tirtc_session_note_event("peer rgb");
        }
        break;
    case TIRTC_SESSION_CMD_STATE_LEGACY:
        if (is_response) {
            if (event->payload.command.data_len >= sizeof(tirtc_session_peer_state_payload_t)) {
                tirtc_session_peer_state_t peer_state = {0};

                tirtc_session_payload_to_peer_state((const tirtc_session_peer_state_payload_t *)event->payload.command.data,
                                                   &peer_state);
                tirtc_session_set_last_peer_state(&peer_state);
                tirtc_session_note_event("peer state");
            }
        } else {
            tirtc_session_peer_state_t local_state = {0};
            tirtc_session_peer_state_payload_t payload = {0};

            tirtc_session_get_local_peer_state(&local_state);
            tirtc_session_peer_state_to_payload(&local_state, &payload);
            if (tirtc_session_send_response(event->payload.command.conn,
                                           event->payload.command.cmdw,
                                           &payload,
                                           sizeof(payload)) == ESP_OK) {
                tirtc_session_note_event("state resp");
            }
        }
        break;
    case TIRTC_SESSION_CMD_HANGUP:
        tirtc_session_apply_hangup_local_state();
        (void)tirtc_session_disconnect();
        tirtc_session_note_event("hangup rx");
        break;
    case TIRTC_SESSION_CMD_REQ_VIDEO: {
        bool enabled = tirtc_session_toggle_from_command_payload(event->payload.command.data,
                                                                 event->payload.command.data_len,
                                                                 true,
                                                                 'v');

        if (is_response) {
            bool no_call = tirtc_session_media_toggle_response_is_no_call(event->payload.command.data,
                                                                         event->payload.command.data_len);
            enabled = tirtc_session_media_toggle_response_enabled(event->payload.command.data,
                                                                 event->payload.command.data_len);
            ESP_LOGD(TAG, "remote video request response: enabled=%d no_call=%d", enabled, no_call);
            if (no_call) {
                tirtc_session_retry_remote_media_request(true, false, "req video nocall");
                tirtc_session_note_event("req video nocall");
            } else {
                tirtc_session_note_event(enabled ? "req video ack" : "req video rsp");
            }
            break;
        }

        tirtc_session_peer_state_t local_state = {0};
        tirtc_session_get_local_peer_state(&local_state);
        ESP_LOGI(TAG, "remote video request: enabled=%d call_active=%d", enabled, local_state.call_active);
        if (enabled && !local_state.call_active) {
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       TIRTC_SESSION_MEDIA_TOGGLE_NO_CALL_TEXT);
            tirtc_session_note_event("peer req video nocall");
            break;
        }

        tirtc_session_set_peer_video_requested(enabled);

        (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                   event->payload.command.cmdw,
                                                   TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT);
        tirtc_session_note_event(enabled ? "peer req video on" : "peer req video off");
        break;
    }
    case TIRTC_SESSION_CMD_REQ_AUDIO: {
        bool enabled = tirtc_session_toggle_from_command_payload(event->payload.command.data,
                                                                 event->payload.command.data_len,
                                                                 true,
                                                                 'a');

        if (is_response) {
            bool no_call = tirtc_session_media_toggle_response_is_no_call(event->payload.command.data,
                                                                         event->payload.command.data_len);
            enabled = tirtc_session_media_toggle_response_enabled(event->payload.command.data,
                                                                 event->payload.command.data_len);
            ESP_LOGD(TAG, "remote audio request response: enabled=%d no_call=%d", enabled, no_call);
            if (no_call) {
                tirtc_session_retry_remote_media_request(false, true, "req audio nocall");
                tirtc_session_note_event("req audio nocall");
            } else {
                tirtc_session_note_event(enabled ? "req audio ack" : "req audio rsp");
            }
            break;
        }

        tirtc_session_peer_state_t local_state = {0};
        tirtc_session_get_local_peer_state(&local_state);
        ESP_LOGI(TAG, "remote audio request: enabled=%d call_active=%d", enabled, local_state.call_active);
        if (enabled && !local_state.call_active) {
            (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                       event->payload.command.cmdw,
                                                       TIRTC_SESSION_MEDIA_TOGGLE_NO_CALL_TEXT);
            tirtc_session_note_event("peer req audio nocall");
            break;
        }

        tirtc_session_set_peer_audio_requested(enabled);

        (void)tirtc_session_send_call_response_text(event->payload.command.conn,
                                                   event->payload.command.cmdw,
                                                   TIRTC_SESSION_MEDIA_TOGGLE_OK_TEXT);
        tirtc_session_note_event(enabled ? "peer req audio on" : "peer req audio off");
        break;
    }
    case TIRTC_SESSION_CMD_SET_SEND_VIDEO: {
        bool enabled = tirtc_session_toggle_from_command_payload(event->payload.command.data,
                                                                 event->payload.command.data_len,
                                                                 true,
                                                                 'v');

        (void)tirtc_session_set_local_video_send_enabled(enabled);
        ESP_LOGI(TAG, "remote set local video send: enabled=%d", enabled);
        tirtc_session_note_event(enabled ? "peer video on" : "peer video off");
        break;
    }
    case TIRTC_SESSION_CMD_SET_SEND_AUDIO: {
        bool enabled = tirtc_session_toggle_from_command_payload(event->payload.command.data,
                                                                 event->payload.command.data_len,
                                                                 true,
                                                                 'a');

        (void)tirtc_session_set_local_audio_send_enabled(enabled);
        ESP_LOGI(TAG, "remote set local audio send: enabled=%d", enabled);
        tirtc_session_note_event(enabled ? "peer audio on" : "peer audio off");
        break;
    }
    case TIRTC_SESSION_CMD_TIME_QUERY: {
        if (is_response) {
            tirtc_session_note_event("time rsp");
            break;
        }

        uint8_t payload[4] = {0};
        tirtc_session_pack_unix_time_le(tirtc_session_get_unix_time_s_for_command(), payload);
        if (tirtc_session_send_response(event->payload.command.conn,
                                       event->payload.command.cmdw,
                                       payload,
                                       sizeof(payload)) == ESP_OK) {
            tirtc_session_note_event("time resp");
        }
        break;
    }
    case TIRTC_SESSION_CMD_CLOCK_SYNC: {
        uint8_t payload[TIRTC_SESSION_CLOCK_SYNC_WIRE_SIZE] = {0};
        uint32_t seq = 0;
        uint32_t t1_ms = 0;
        uint32_t t2_ms = 0;
        uint32_t t3_ms = 0;
        bool wall_clock_valid = false;

        if (!tirtc_session_is_test_media_command_active()) {
            break;
        }

        if (is_response) {
            if (tirtc_session_parse_clock_sync_response(event->payload.command.data,
                                                       event->payload.command.data_len,
                                                       &seq,
                                                       &t1_ms,
                                                       &t2_ms,
                                                       &t3_ms)) {
                ESP_LOGI(TAG,
                         "clock sync rx rsp: cmdw=0x%08lx seq=%lu t1=%lu t2=%lu t3=%lu len=%lu",
                         (unsigned long)event->payload.command.cmdw,
                         (unsigned long)seq,
                         (unsigned long)t1_ms,
                         (unsigned long)t2_ms,
                         (unsigned long)t3_ms,
                         (unsigned long)event->payload.command.data_len);
            } else {
                ESP_LOGW(TAG,
                         "clock sync rx invalid rsp: cmdw=0x%08lx len=%lu",
                         (unsigned long)event->payload.command.cmdw,
                         (unsigned long)event->payload.command.data_len);
            }
            tirtc_session_note_event("clock sync rsp");
            break;
        }

        if (!tirtc_session_parse_clock_sync_request(event->payload.command.data,
                                                   event->payload.command.data_len,
                                                   &seq,
                                                   &t1_ms)) {
            ESP_LOGW(TAG,
                     "ignore invalid clock sync request len=%lu",
                     (unsigned long)event->payload.command.data_len);
            break;
        }

        ESP_LOGI(TAG,
                 "clock sync rx req: cmdw=0x%08lx seq=%lu t1=%lu len=%lu",
                 (unsigned long)event->payload.command.cmdw,
                 (unsigned long)seq,
                 (unsigned long)t1_ms,
                 (unsigned long)event->payload.command.data_len);

        t2_ms = tirtc_session_get_unix_time_ms_low32_for_command(&wall_clock_valid);
        t3_ms = tirtc_session_get_unix_time_ms_low32_for_command(&wall_clock_valid);
        if (!wall_clock_valid) {
            if (!s_clock_sync_wall_clock_warned) {
                ESP_LOGW(TAG,
                         "clock sync response uses unsynced wall clock: t2=%lu t3=%lu; SNTP/time sync not ready",
                         (unsigned long)t2_ms,
                         (unsigned long)t3_ms);
                s_clock_sync_wall_clock_warned = true;
            }
        } else {
            s_clock_sync_wall_clock_warned = false;
        }
        tirtc_session_build_clock_sync_response(payload, seq, t1_ms, t2_ms, t3_ms);
        ESP_LOGI(TAG,
                 "clock sync tx rsp: req_cmdw=0x%08lx rsp_cmdw=0x%08lx seq=%lu t1=%lu t2=%lu t3=%lu",
                 (unsigned long)event->payload.command.cmdw,
                 (unsigned long)tirtc_session_make_command_response_word(event->payload.command.cmdw),
                 (unsigned long)seq,
                 (unsigned long)t1_ms,
                 (unsigned long)t2_ms,
                 (unsigned long)t3_ms);
        if (tirtc_session_send_response(event->payload.command.conn,
                                       event->payload.command.cmdw,
                                       payload,
                                       sizeof(payload)) == ESP_OK) {
            tirtc_session_note_event("clock sync resp");
        } else {
            ESP_LOGW(TAG,
                     "clock sync response failed seq=%lu",
                     (unsigned long)seq);
        }
        break;
    }
    default:
        ESP_LOGW(TAG,
                 "unknown remote command: cmd=0x%04x raw=0x%08lx len=%lu",
                 cmd,
                 (unsigned long)event->payload.command.cmdw,
                 (unsigned long)event->payload.command.data_len);
        break;
    }
}

esp_err_t tirtc_session_accept_incoming_call(void)
{
    tirtc_conn_t conn = NULL;
    uint32_t pending_cmdw = 0;

    tirtc_session_get_pending_call(&conn, &pending_cmdw);
    ESP_RETURN_ON_FALSE(conn != NULL && pending_cmdw != 0, ESP_ERR_INVALID_STATE, TAG, "no pending call");
    ESP_RETURN_ON_ERROR(tirtc_session_send_call_response_text(conn, pending_cmdw, TIRTC_SESSION_CALL_ALLOW_TEXT),
                        TAG,
                        "accept call failed");

    tirtc_session_complete_call_response(true);
    tirtc_session_note_event("call accept");
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

esp_err_t tirtc_session_reject_incoming_call(void)
{
    tirtc_conn_t conn = NULL;
    uint32_t pending_cmdw = 0;

    tirtc_session_get_pending_call(&conn, &pending_cmdw);
    ESP_RETURN_ON_FALSE(conn != NULL && pending_cmdw != 0, ESP_ERR_INVALID_STATE, TAG, "no pending call");
    ESP_RETURN_ON_ERROR(tirtc_session_send_call_response_text(conn, pending_cmdw, TIRTC_SESSION_CALL_REJECT_TEXT),
                        TAG,
                        "reject call failed");

    tirtc_session_complete_call_response(false);
    tirtc_session_note_event("call reject");
    (void)tirtc_session_disconnect();
    return ESP_OK;
}

esp_err_t tirtc_session_hangup(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_send_request(TIRTC_SESSION_CMD_HANGUP, NULL, 0), TAG, "send hangup failed");
    tirtc_session_apply_hangup_local_state();
    tirtc_session_note_event("hangup tx");
    return ESP_OK;
}

esp_err_t tirtc_session_send_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    tirtc_session_rgb_payload_t payload = {
        .red = red,
        .green = green,
        .blue = blue,
    };

    tirtc_session_set_local_rgb(red, green, blue);
    ESP_RETURN_ON_ERROR(tirtc_session_send_request(TIRTC_SESSION_CMD_RGB_LEGACY, &payload, sizeof(payload)),
                        TAG,
                        "send rgb failed");
    tirtc_session_note_event("rgb tx");
    return ESP_OK;
}

esp_err_t tirtc_session_query_peer_state(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_send_request(TIRTC_SESSION_CMD_STATE_LEGACY, NULL, 0),
                        TAG,
                        "query peer state failed");
    tirtc_session_note_event("state req");
    return ESP_OK;
}

esp_err_t tirtc_session_send_stream_message(const void *data, size_t data_len)
{
    tirtc_conn_t conn = NULL;
    TIRTCFRAMEINFO frame_info = {
        .stream_id = TIRTC_SESSION_MESSAGE_STREAM_ID,
        .media = TIRTC_MEDIA_MESSAGE,
        .flags = 0,
        .reserved = 0,
        .ts = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .length = (uint32_t)data_len,
    };

    ESP_RETURN_ON_FALSE(tirtc_session_try_get_active_conn(&conn), ESP_ERR_INVALID_STATE, TAG, "rtc connection not ready");

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSendMessageStream(conn, &frame_info, data);
        tirtc_session_give_sdk_api_lock();
    } else {
        tirtc_session_note_event("message tx lock fail");
        ESP_LOGW(TAG, "rtc sdk api lock unavailable for message stream");
        return ESP_FAIL;
    }
    if (ret < 0) {
        if (ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_retry_message_stream_after_invalid_handle(conn, "send message stream")) {
            return ESP_ERR_INVALID_STATE;
        }

        tirtc_session_set_last_error(ret);
        if (tirtc_session_should_reset_after_send_error(ret)) {
            tirtc_session_note_event("message tx lost");
            ESP_LOGW(TAG,
                     "message stream send failed; closing connection: %s (%d)",
                     TiRtcGetErrorStr(ret),
                     ret);
            tirtc_session_handle_connection_loss(conn, ret);
        } else {
            tirtc_session_note_event("message tx error");
            ESP_LOGW(TAG,
                     "message stream send failed: %s (%d)",
                     TiRtcGetErrorStr(ret),
                     ret);
        }
        return ESP_FAIL;
    }

    tirtc_session_note_event("message tx");
    return ESP_OK;
}
