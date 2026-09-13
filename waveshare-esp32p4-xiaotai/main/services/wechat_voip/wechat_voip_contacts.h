#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WECHAT_VOIP_CONTACT_MAX 4
#define WECHAT_VOIP_OPEN_ID_MAX 96
#define WECHAT_VOIP_MODEL_ID_MAX 64
#define WECHAT_VOIP_APP_ID_MAX 64
#define WECHAT_VOIP_REMARK_MAX_CHARS 64
#define WECHAT_VOIP_REMARK_MAX ((WECHAT_VOIP_REMARK_MAX_CHARS * 4) + 1)

typedef struct {
    char open_id[WECHAT_VOIP_OPEN_ID_MAX];
    char remark[WECHAT_VOIP_REMARK_MAX];
} wechat_voip_contact_t;

typedef struct {
    bool ready;
    bool server_synced;
    esp_err_t last_error;
    uint8_t count;
    wechat_voip_contact_t contacts[WECHAT_VOIP_CONTACT_MAX];
} wechat_voip_contacts_snapshot_t;

typedef struct {
    char openid[WECHAT_VOIP_OPEN_ID_MAX];
    char model_id[WECHAT_VOIP_MODEL_ID_MAX];
    char app_id[WECHAT_VOIP_APP_ID_MAX];
    char remark[WECHAT_VOIP_REMARK_MAX];
} wechat_voip_auth_user_t;

esp_err_t wechat_voip_contacts_init(void);
void wechat_voip_contacts_reset_for_device(const char *device_id);
void wechat_voip_contacts_load(const char *device_id);
void wechat_voip_contacts_replace(const wechat_voip_auth_user_t *users,
                                  size_t user_count,
                                  const char *source);
void wechat_voip_contacts_note_sync_error(esp_err_t error);
bool wechat_voip_contacts_remember(const wechat_voip_auth_user_t *user, const char *source);
bool wechat_voip_contacts_remove(const char *openid, wechat_voip_auth_user_t *removed);
esp_err_t wechat_voip_contacts_update_remark(const char *openid,
                                             const char *remark,
                                             const char *source);
void wechat_voip_contacts_find(const char *openid, wechat_voip_auth_user_t *target);
void wechat_voip_contacts_get_snapshot(wechat_voip_contacts_snapshot_t *snapshot);
bool wechat_voip_remark_is_valid(const char *remark);

#ifdef __cplusplus
}
#endif
