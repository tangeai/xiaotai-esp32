#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_CALL_STATE_IDLE = 0,
    DEVICE_CALL_STATE_OUTGOING,
    DEVICE_CALL_STATE_INCOMING,
    DEVICE_CALL_STATE_CONNECTING,
    DEVICE_CALL_STATE_IN_CALL,
    DEVICE_CALL_STATE_ERROR,
} device_call_state_t;

typedef enum {
    DEVICE_CALL_TYPE_AUDIO = 0,
    DEVICE_CALL_TYPE_VIDEO,
} device_call_type_t;

typedef void (*device_call_session_ended_cb_t)(void *ctx);
typedef bool (*device_call_can_accept_incoming_cb_t)(void *ctx);

typedef struct {
    bool enabled;
    const char *api_base;
    device_call_can_accept_incoming_cb_t can_accept_incoming;
    device_call_session_ended_cb_t on_session_ended;
    void *ctx;
} device_call_config_t;

typedef struct {
    device_call_state_t state;
    bool pending_incoming;
    char room_id[96];
    char peer_device_id[128];
    char call_type[16];
    esp_err_t last_error;
    char message[96];
} device_call_snapshot_t;

#define DEVICE_CALL_CONTACT_MAX 8

typedef enum {
    DEVICE_CALL_CONTACT_SOURCE_UNKNOWN = 0,
    DEVICE_CALL_CONTACT_SOURCE_MANUAL,
    DEVICE_CALL_CONTACT_SOURCE_AUTO,
} device_call_contact_source_t;

typedef struct {
    char device_id[128];
    char remark[64];
    bool online;
    device_call_contact_source_t source;
} device_call_contact_t;

typedef struct {
    char peer_device_id[128];
    char created_at[48];
} device_call_pending_contact_t;

typedef struct {
    bool ready;
    bool refreshing;
    uint8_t count;
    uint8_t pending_count;
    esp_err_t last_error;
    device_call_contact_t contacts[DEVICE_CALL_CONTACT_MAX];
    device_call_pending_contact_t pending[DEVICE_CALL_CONTACT_MAX];
} device_call_contacts_snapshot_t;

esp_err_t device_call_init(const device_call_config_t *config);
esp_err_t device_call_set_api_base(const char *api_base);
esp_err_t device_call_start(void);
esp_err_t device_call_reconcile_room_async(void);
void device_call_reset_identity_state(void);
esp_err_t device_call_request(const char *target_device_id, device_call_type_t call_type);
esp_err_t device_call_accept_pending(void);
esp_err_t device_call_reject_pending(void);
esp_err_t device_call_hangup(void);
bool device_call_has_pending_incoming(void);
void device_call_get_snapshot(device_call_snapshot_t *snapshot);
esp_err_t device_call_refresh_contacts_async(void);
esp_err_t device_call_request_contact_async(const char *target_device_id);
esp_err_t device_call_respond_contact_async(const char *peer_device_id, bool accept);
esp_err_t device_call_update_contact_remark_async(const char *peer_id, const char *remark);
esp_err_t device_call_delete_contact_async(const char *peer_device_id);
void device_call_get_contacts_snapshot(device_call_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
