#pragma once

/* thing-connect VoIP business coordinator. */

#include <stdbool.h>

#include "esp_err.h"
#include "wechat_voip_contacts.h"
#include "wechat_voip_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*wechat_voip_incoming_allowed_cb_t)(void *ctx);

esp_err_t wechat_voip_thing_set_incoming_policy(
    wechat_voip_incoming_allowed_cb_t callback,
    void *ctx);
esp_err_t wechat_voip_thing_start(void);
void wechat_voip_thing_stop(void);
bool wechat_voip_thing_is_connected(void);
esp_err_t wechat_voip_thing_refresh_contacts_async(void);
esp_err_t wechat_voip_thing_update_contact_remark_async(const char *open_id,
                                                        const char *remark);
esp_err_t wechat_voip_thing_request_call(const char *open_id,
                                         wechat_voip_call_media_t call_media);
esp_err_t wechat_voip_thing_add_contact(const char *open_id);
esp_err_t wechat_voip_thing_remove_contact(const char *open_id);
bool wechat_voip_thing_request_call_busy(void);
bool wechat_voip_thing_request_call_cancelling(void);
bool wechat_voip_thing_cancel_pending_call(void);
void wechat_voip_thing_maintenance(void);
void wechat_voip_thing_get_contacts(wechat_voip_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
