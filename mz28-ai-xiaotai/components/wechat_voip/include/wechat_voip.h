#pragma once
#include "demo_platform.h"
typedef enum {
    VOIP_STATUS,
    VOIP_CONTACTS,
    VOIP_CALL,
    VOIP_ANSWER,
    VOIP_REJECT,
    VOIP_HANGUP,
    VOIP_RX_DUMP,
    VOIP_RX_RECORD,
    VOIP_RETRY,
    VOIP_PRIMARY_ACTION,
    VOIP_DOUBLE_ACTION
} voip_command_t;

typedef enum {
    WECHAT_VOIP_IDLE,
    WECHAT_VOIP_DIALING,
    WECHAT_VOIP_RINGING,
    WECHAT_VOIP_CONNECTING,
    WECHAT_VOIP_WAITING_PEER,
    WECHAT_VOIP_ACTIVE,
    WECHAT_VOIP_CLOSING,
} wechat_voip_state_t;

typedef enum {
    WECHAT_VOIP_DIRECTION_NONE,
    WECHAT_VOIP_DIRECTION_INCOMING,
    WECHAT_VOIP_DIRECTION_OUTGOING,
} wechat_voip_direction_t;

esp_err_t wechat_voip_start(unsigned version_type);
bool wechat_voip_platform_event(const platform_event_t *event);
esp_err_t wechat_voip_command(voip_command_t command, unsigned contact_index);
wechat_voip_state_t wechat_voip_state(void);
wechat_voip_direction_t wechat_voip_direction(void);
bool wechat_voip_last_end_failed(void);
