#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AI_CHAT_SESSION_ID_MAX   128U
#define AI_CHAT_CAPTION_TEXT_MAX 256U
#define AI_CHAT_EVENT_DATA_MAX   256U
#define AI_CHAT_ERROR_TEXT_MAX   128U
#define AI_CHAT_CODEC_MAX        16U
#define AI_CHAT_JSONRPC_ID_MAX   96U
#define AI_CHAT_DEVICE_ACTION_NAME_MAX          64U
#define AI_CHAT_DEVICE_ACTION_TARGET_MAX        128U
#define AI_CHAT_DEVICE_ACTION_CALL_TYPE_MAX     16U
#define AI_CHAT_DEVICE_ACTION_CONTACT_TYPE_MAX  16U
#define AI_CHAT_DEVICE_ACTION_STATUS_FILTER_MAX 16U

typedef enum {
    AI_CHAT_EVENT_UNKNOWN = 0,
    AI_CHAT_EVENT_START_OK,
    AI_CHAT_EVENT_START_ERROR,
    AI_CHAT_EVENT_CAPTION,
    AI_CHAT_EVENT_ROUND_START,
    AI_CHAT_EVENT_ROUND_END,
    AI_CHAT_EVENT_HEARTBEAT,
    AI_CHAT_EVENT_INTERRUPT,
    AI_CHAT_EVENT_CUSTOM_EVENT,
    AI_CHAT_EVENT_END_SESSION,
    AI_CHAT_EVENT_DEVICE_ACTION,
} ai_chat_event_type_t;

typedef struct {
    char codec[AI_CHAT_CODEC_MAX];
    uint32_t sample_rate;
    uint8_t channels;
    bool valid;
} ai_chat_audio_spec_t;

typedef struct {
    ai_chat_event_type_t type;
    int jsonrpc_id;
    bool jsonrpc_id_valid;
    int error_code;
    int caption_type;
    int mode;
    int seq_num;
    int64_t utterance_id;
    bool is_final;
    char session_id[AI_CHAT_SESSION_ID_MAX];
    char jsonrpc_id_json[AI_CHAT_JSONRPC_ID_MAX];
    char action[AI_CHAT_DEVICE_ACTION_NAME_MAX];
    char target[AI_CHAT_DEVICE_ACTION_TARGET_MAX];
    char call_type[AI_CHAT_DEVICE_ACTION_CALL_TYPE_MAX];
    char contact_type[AI_CHAT_DEVICE_ACTION_CONTACT_TYPE_MAX];
    char status_filter[AI_CHAT_DEVICE_ACTION_STATUS_FILTER_MAX];
    char text[AI_CHAT_CAPTION_TEXT_MAX];
    char event_data[AI_CHAT_EVENT_DATA_MAX];
    char error_message[AI_CHAT_ERROR_TEXT_MAX];
    ai_chat_audio_spec_t input_audio;
    ai_chat_audio_spec_t output_audio;
} ai_chat_event_t;

esp_err_t ai_chat_events_parse(const void *data, uint32_t len, ai_chat_event_t *event);
const char *ai_chat_event_type_name(ai_chat_event_type_t type);
