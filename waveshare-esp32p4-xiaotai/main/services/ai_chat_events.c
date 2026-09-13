#include "ai_chat_events.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "app_memory_policy.h"

static char *ai_chat_events_alloc_json_copy(uint32_t len)
{
    char *json = (char *)app_memory_alloc_psram((size_t)len + 1U);
    return json;
}

static void ai_chat_trim_utf8_tail(char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    size_t len = strlen(text);
    while (len > 0U) {
        char tail = text[len - 1U];
        if (tail != '\r' && tail != '\n' && tail != '\t' && tail != ' ') {
            break;
        }
        text[--len] = '\0';
    }
    if (len == 0U) {
        return;
    }

    size_t lead_pos = len - 1U;
    while (lead_pos > 0U && (((uint8_t)text[lead_pos] & 0xC0U) == 0x80U)) {
        lead_pos--;
    }

    uint8_t lead = (uint8_t)text[lead_pos];
    size_t seq_len = len - lead_pos;
    size_t expected_len = 0U;

    if ((lead & 0x80U) == 0U) {
        expected_len = 1U;
    } else if ((lead & 0xE0U) == 0xC0U) {
        expected_len = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        expected_len = 3U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        expected_len = 4U;
    }

    if (expected_len == 0U || seq_len < expected_len) {
        text[lead_pos] = '\0';
    }
}

static void ai_chat_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
    ai_chat_trim_utf8_tail(dst);
}

const char *ai_chat_event_type_name(ai_chat_event_type_t type)
{
    switch (type) {
    case AI_CHAT_EVENT_START_OK:
        return "start_ok";
    case AI_CHAT_EVENT_START_ERROR:
        return "start_error";
    case AI_CHAT_EVENT_CAPTION:
        return "caption";
    case AI_CHAT_EVENT_ROUND_START:
        return "round_start";
    case AI_CHAT_EVENT_ROUND_END:
        return "round_end";
    case AI_CHAT_EVENT_HEARTBEAT:
        return "heartbeat";
    case AI_CHAT_EVENT_INTERRUPT:
        return "interrupt";
    case AI_CHAT_EVENT_CUSTOM_EVENT:
        return "event";
    case AI_CHAT_EVENT_END_SESSION:
        return "end_session";
    case AI_CHAT_EVENT_DEVICE_ACTION:
        return "device_action";
    default:
        return "unknown";
    }
}

static void ai_chat_parse_jsonrpc_id(cJSON *root, ai_chat_event_t *event)
{
    cJSON *id = NULL;
    char *json = NULL;

    if (!cJSON_IsObject(root) || event == NULL) {
        return;
    }

    id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsNumber(id) && !cJSON_IsString(id)) {
        return;
    }
    if (cJSON_IsNumber(id)) {
        event->jsonrpc_id = id->valueint;
    }

    json = cJSON_PrintUnformatted(id);
    if (json == NULL) {
        return;
    }
    ai_chat_copy_str(event->jsonrpc_id_json,
                     sizeof(event->jsonrpc_id_json),
                     json);
    cJSON_free(json);
    event->jsonrpc_id_valid = event->jsonrpc_id_json[0] != '\0';
}

static void ai_chat_parse_audio_spec(cJSON *object, ai_chat_audio_spec_t *spec)
{
    cJSON *sample_rate = NULL;
    cJSON *channels = NULL;

    if (!cJSON_IsObject(object) || spec == NULL) {
        return;
    }

    ai_chat_copy_str(spec->codec,
                     sizeof(spec->codec),
                     cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "codec")));
    sample_rate = cJSON_GetObjectItemCaseSensitive(object, "sample_rate");
    channels = cJSON_GetObjectItemCaseSensitive(object, "channels");
    if (cJSON_IsNumber(sample_rate)) {
        spec->sample_rate = (uint32_t)sample_rate->valuedouble;
    }
    if (cJSON_IsNumber(channels)) {
        spec->channels = (uint8_t)channels->valueint;
    }
    spec->valid = spec->codec[0] != '\0' && spec->sample_rate > 0U && spec->channels > 0U;
}

static void ai_chat_parse_start_result(cJSON *root, ai_chat_event_t *event)
{
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");

    ai_chat_parse_jsonrpc_id(root, event);

    if (cJSON_IsObject(result)) {
        event->type = AI_CHAT_EVENT_START_OK;
        ai_chat_copy_str(event->session_id,
                         sizeof(event->session_id),
                         cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "session_id")));
        ai_chat_parse_audio_spec(cJSON_GetObjectItemCaseSensitive(result, "input_audio"),
                                 &event->input_audio);
        ai_chat_parse_audio_spec(cJSON_GetObjectItemCaseSensitive(result, "output_audio"),
                                 &event->output_audio);
        return;
    }

    if (cJSON_IsObject(error)) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");

        event->type = AI_CHAT_EVENT_START_ERROR;
        if (cJSON_IsNumber(code)) {
            event->error_code = code->valueint;
        }
        ai_chat_copy_str(event->error_message,
                         sizeof(event->error_message),
                         cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message")));
    }
}

static void ai_chat_parse_caption(cJSON *params, ai_chat_event_t *event)
{
    cJSON *caption_type = cJSON_GetObjectItemCaseSensitive(params, "caption_type");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "mode");
    cJSON *seq_num = cJSON_GetObjectItemCaseSensitive(params, "seq_num");
    cJSON *utterance_id = cJSON_GetObjectItemCaseSensitive(params, "utterance_id");
    cJSON *is_final = cJSON_GetObjectItemCaseSensitive(params, "is_final");

    event->type = AI_CHAT_EVENT_CAPTION;
    if (cJSON_IsNumber(caption_type)) {
        event->caption_type = caption_type->valueint;
    }
    if (cJSON_IsNumber(mode)) {
        event->mode = mode->valueint;
    }
    if (cJSON_IsNumber(seq_num)) {
        event->seq_num = seq_num->valueint;
    }
    if (cJSON_IsNumber(utterance_id)) {
        event->utterance_id = (int64_t)utterance_id->valuedouble;
    }
    if (cJSON_IsBool(is_final)) {
        event->is_final = cJSON_IsTrue(is_final);
    }
    ai_chat_copy_str(event->text,
                     sizeof(event->text),
                     cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "text")));
}

static void ai_chat_parse_event_data(cJSON *params, ai_chat_event_t *event)
{
    cJSON *data = NULL;
    const char *text = NULL;

    if (!cJSON_IsObject(params) || event == NULL) {
        return;
    }

    data = cJSON_GetObjectItemCaseSensitive(params, "data");
    text = cJSON_GetStringValue(data);
    if (text != NULL) {
        ai_chat_copy_str(event->event_data, sizeof(event->event_data), text);
        return;
    }

    if (data != NULL) {
        char *json = cJSON_PrintUnformatted(data);
        if (json != NULL) {
            ai_chat_copy_str(event->event_data, sizeof(event->event_data), json);
            cJSON_free(json);
        }
    }
}

static const char *ai_chat_json_string(cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object) || name == NULL) {
        return NULL;
    }
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static cJSON *ai_chat_pick_device_action_payload(cJSON *params)
{
    static const char *const payload_names[] = {
        "data",
        "arguments",
        "args",
        "input",
        "payload",
        "parameters",
    };

    if (!cJSON_IsObject(params)) {
        return NULL;
    }
    for (size_t index = 0U;
         index < sizeof(payload_names) / sizeof(payload_names[0]);
         ++index) {
        cJSON *payload =
            cJSON_GetObjectItemCaseSensitive(params, payload_names[index]);
        if (cJSON_IsObject(payload)) {
            return payload;
        }
    }
    return params;
}

static const char *ai_chat_pick_device_action_string(cJSON *params,
                                                     cJSON *payload,
                                                     const char *const *names,
                                                     size_t name_count)
{
    for (size_t index = 0U; index < name_count; ++index) {
        const char *value = ai_chat_json_string(payload, names[index]);
        if (value != NULL && value[0] != '\0') {
            return value;
        }
        value = ai_chat_json_string(params, names[index]);
        if (value != NULL && value[0] != '\0') {
            return value;
        }
    }
    return NULL;
}

static void ai_chat_parse_device_action(cJSON *root,
                                        cJSON *params,
                                        ai_chat_event_t *event)
{
    static const char *const target_names[] = {
        "target",
        "target_device",
        "target_device_id",
        "device",
        "device_id",
        "device_name",
        "device_alias",
        "contact",
        "contact_name",
        "name",
        "alias",
        "remark",
        "callee",
        "callee_device_id",
        "peer",
        "peer_id",
        "nickname",
    };
    static const char *const call_type_names[] = {
        "call_type", "type", "media", "mode",
    };
    static const char *const contact_type_names[] = {
        "contact_type", "target_type", "contact_source", "route",
    };
    static const char *const status_filter_names[] = {
        "status_filter", "online_status", "presence", "status", "filter",
    };
    cJSON *payload = ai_chat_pick_device_action_payload(params);
    const char *action = NULL;

    event->type = AI_CHAT_EVENT_DEVICE_ACTION;
    ai_chat_parse_jsonrpc_id(root, event);

    /* Tool-call envelopes put the action in params.name while
     * arguments.name may be the contact name. Keep those namespaces apart. */
    action = ai_chat_json_string(params, "action");
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(params, "name");
    }
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(params, "tool");
    }
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(params, "function");
    }
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(payload, "action");
    }
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(payload, "tool");
    }
    if (action == NULL || action[0] == '\0') {
        action = ai_chat_json_string(payload, "function");
    }
    ai_chat_copy_str(event->action, sizeof(event->action), action);
    ai_chat_copy_str(event->target,
                     sizeof(event->target),
                     ai_chat_pick_device_action_string(
                         params,
                         payload,
                         target_names,
                         sizeof(target_names) / sizeof(target_names[0])));
    ai_chat_copy_str(event->call_type,
                     sizeof(event->call_type),
                     ai_chat_pick_device_action_string(
                         params,
                         payload,
                         call_type_names,
                         sizeof(call_type_names) / sizeof(call_type_names[0])));
    ai_chat_copy_str(event->contact_type,
                     sizeof(event->contact_type),
                     ai_chat_pick_device_action_string(
                         params,
                         payload,
                         contact_type_names,
                         sizeof(contact_type_names) / sizeof(contact_type_names[0])));
    ai_chat_copy_str(event->status_filter,
                     sizeof(event->status_filter),
                     ai_chat_pick_device_action_string(
                         params,
                         payload,
                         status_filter_names,
                         sizeof(status_filter_names) / sizeof(status_filter_names[0])));
}

esp_err_t ai_chat_events_parse(const void *data, uint32_t len, ai_chat_event_t *event)
{
    char *json = NULL;
    cJSON *root = NULL;
    const char *method = NULL;
    cJSON *params = NULL;

    if (data == NULL || len == 0U || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(event, 0, sizeof(*event));
    event->type = AI_CHAT_EVENT_UNKNOWN;
    event->caption_type = -1;

    json = ai_chat_events_alloc_json_copy(len);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, data, len);
    json[len] = '\0';

    root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ai_chat_parse_start_result(root, event);
    if (event->type != AI_CHAT_EVENT_UNKNOWN) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "method"));
    params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (method == NULL) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (strcmp(method, "caption") == 0) {
        ai_chat_parse_caption(params, event);
    } else if (strcmp(method, "round_start") == 0) {
        event->type = AI_CHAT_EVENT_ROUND_START;
    } else if (strcmp(method, "round_end") == 0) {
        event->type = AI_CHAT_EVENT_ROUND_END;
    } else if (strcmp(method, "heartbeat") == 0) {
        event->type = AI_CHAT_EVENT_HEARTBEAT;
    } else if (strcmp(method, "interrupt") == 0) {
        event->type = AI_CHAT_EVENT_INTERRUPT;
    } else if (strcmp(method, "event") == 0) {
        event->type = AI_CHAT_EVENT_CUSTOM_EVENT;
        ai_chat_parse_event_data(params, event);
    } else if (strcmp(method, "end_session") == 0) {
        event->type = AI_CHAT_EVENT_END_SESSION;
    } else if (strcmp(method, "device_action") == 0) {
        ai_chat_parse_device_action(root, params, event);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
