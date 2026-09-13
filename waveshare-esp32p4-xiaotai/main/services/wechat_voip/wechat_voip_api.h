#pragma once

#include "esp_err.h"
#include "wechat_voip_contacts.h"
#include "wechat_voip_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wechat_voip_api_caller_cb_t)(const wechat_voip_auth_user_t *caller, void *ctx);

esp_err_t wechat_voip_api_report_profile(const char *api_base, const char *mqtt_token);
esp_err_t wechat_voip_api_fetch_callers(const char *api_base,
                                        const char *mqtt_token,
                                        wechat_voip_api_caller_cb_t caller_cb,
                                        void *ctx,
                                        int *caller_count);
esp_err_t wechat_voip_api_request_call(const char *api_base,
                                       const char *mqtt_token,
                                       const char *device_id,
                                       const wechat_voip_auth_user_t *target,
                                       wechat_voip_call_media_t call_media,
                                       int wx_version_type);
esp_err_t wechat_voip_api_update_contact_remark(const char *api_base,
                                                const char *mqtt_token,
                                                const char *peer_id,
                                                const char *remark);

#ifdef __cplusplus
}
#endif
