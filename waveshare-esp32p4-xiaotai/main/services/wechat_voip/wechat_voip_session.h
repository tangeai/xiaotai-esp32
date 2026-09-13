#pragma once

/* 微信 VoIP 会话:保存本次入会信息,驱动 WHIP 建连和通话状态. */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "tiRTC.h"
#include "wechat_voip_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WECHAT_VOIP_SESSION_STATE_IDLE = 0,
    WECHAT_VOIP_SESSION_STATE_RINGING,
    WECHAT_VOIP_SESSION_STATE_CONNECTING,
    WECHAT_VOIP_SESSION_STATE_AWAITING_CONNECTED,
    WECHAT_VOIP_SESSION_STATE_IN_CALL,
    WECHAT_VOIP_SESSION_STATE_CLOSING,
} wechat_voip_session_state_t;

esp_err_t wechat_voip_session_handle_join_room(cJSON *payload,
                                               bool auto_answer,
                                               bool cancel_on_connect,
                                               wechat_voip_call_media_t call_media);
esp_err_t wechat_voip_session_reject_join_room_busy(cJSON *payload);
esp_err_t wechat_voip_session_answer(void);
esp_err_t wechat_voip_session_reject_incoming(void);
bool wechat_voip_session_has_incoming_call(void);
wechat_voip_session_state_t wechat_voip_session_get_state(void);
bool wechat_voip_session_is_idle(void);
bool wechat_voip_session_is_closing(void);
bool wechat_voip_session_ready_for_next_call(bool log_detail);
bool wechat_voip_session_is_current_room(const char *room_id);
bool wechat_voip_session_is_recent_room(const char *room_id);
esp_err_t wechat_voip_session_cancel_outbound_on_connect(void);
esp_err_t wechat_voip_session_wait_until_released(uint32_t timeout_ms);
void wechat_voip_session_dump_status(const char *reason);
void wechat_voip_session_maintenance(void);

bool wechat_voip_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len);
bool wechat_voip_session_on_conn_error(tirtc_conn_t hconn, int error);
bool wechat_voip_session_on_disconnected(tirtc_conn_t hconn);

bool wechat_voip_session_cancel_by_room(const char *room_id);
void wechat_voip_session_hangup(void);

#ifdef __cplusplus
}
#endif
