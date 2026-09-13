#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wechat_voip_media.h"
#include "wechat_voip_thing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WECHAT_VOIP_CALL_STATE_IDLE = 0,
    WECHAT_VOIP_CALL_STATE_INCOMING,
    WECHAT_VOIP_CALL_STATE_CALLING,
    WECHAT_VOIP_CALL_STATE_CONNECTING,
    WECHAT_VOIP_CALL_STATE_IN_CALL,
    WECHAT_VOIP_CALL_STATE_CLOSING,
} wechat_voip_call_state_t;

esp_err_t wechat_voip_service_configure_media_lifecycle(
    const wechat_voip_media_lifecycle_t *lifecycle,
    void *ctx);
esp_err_t wechat_voip_service_set_incoming_policy(
    wechat_voip_incoming_allowed_cb_t callback,
    void *ctx);
esp_err_t wechat_voip_service_start_ingress(void);
void wechat_voip_service_suspend_ingress(void);
esp_err_t wechat_voip_service_start(void);
void wechat_voip_service_stop(void);
esp_err_t wechat_voip_service_answer(void);
esp_err_t wechat_voip_service_reject_or_hangup(void);
esp_err_t wechat_voip_service_request_call(const char *open_id,
                                           wechat_voip_call_media_t call_media);
esp_err_t wechat_voip_service_refresh_contacts_async(void);
esp_err_t wechat_voip_service_update_contact_remark(const char *open_id,
                                                    const char *remark);
bool wechat_voip_service_is_enabled(void);
bool wechat_voip_service_is_connected(void);
esp_err_t wechat_voip_service_add_contact(const char *open_id);
esp_err_t wechat_voip_service_remove_contact(const char *open_id);
bool wechat_voip_service_has_incoming_call(void);
wechat_voip_call_state_t wechat_voip_service_get_call_state(void);
void wechat_voip_service_maintenance(void);
void wechat_voip_service_get_contacts(wechat_voip_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
