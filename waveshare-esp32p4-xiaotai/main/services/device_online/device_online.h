#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_auth_http.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_ONLINE_DEVICE_ID_MAX_LEN  128
#define DEVICE_ONLINE_DEVICE_KEY_MAX_LEN 128
#define DEVICE_ONLINE_MESSAGE_MAX_LEN    96
#define DEVICE_ONLINE_STATUS_REASON_MAX  32

typedef enum {
    DEVICE_ONLINE_STATE_DISABLED = 0,
    DEVICE_ONLINE_STATE_OFFLINE,
    DEVICE_ONLINE_STATE_UNBOUND,
    DEVICE_ONLINE_STATE_AUTHENTICATING,
    DEVICE_ONLINE_STATE_MQTT_CONNECTING,
    DEVICE_ONLINE_STATE_ONLINE,
    DEVICE_ONLINE_STATE_ERROR,
} device_online_state_t;

typedef struct {
    char device_id[DEVICE_ONLINE_DEVICE_ID_MAX_LEN];
    char device_key[DEVICE_ONLINE_DEVICE_KEY_MAX_LEN];
} device_online_credentials_t;

typedef esp_err_t (*device_online_load_credentials_cb_t)(device_online_credentials_t *credentials,
                                                         void *ctx);

typedef void (*device_online_message_cb_t)(const char *topic,
                                           const char *payload,
                                           size_t payload_len,
                                           void *ctx);

typedef esp_err_t (*device_online_build_status_cb_t)(char *buffer,
                                                     size_t buffer_size,
                                                     const char *reason,
                                                     uint32_t seq,
                                                     void *ctx);

typedef void (*device_online_rebind_required_cb_t)(void *ctx);
typedef void (*device_online_ready_cb_t)(void *ctx);

typedef struct {
    bool enabled;
    const char *api_base;
    const char *mqtt_uri;
    uint32_t heartbeat_interval_ms;
    device_online_load_credentials_cb_t load_credentials;
    device_online_message_cb_t on_message;
    device_online_build_status_cb_t build_status;
    device_online_rebind_required_cb_t on_rebind_required;
    device_online_ready_cb_t on_online_ready;
    void *ctx;
    void *status_ctx;
    void *rebind_ctx;
    void *online_ready_ctx;
} device_online_config_t;

typedef struct {
    device_online_state_t state;
    bool running;
    bool network_ready;
    bool bound;
    bool mqtt_connected;
    char device_id[DEVICE_ONLINE_DEVICE_ID_MAX_LEN];
    esp_err_t last_error;
    char message[DEVICE_ONLINE_MESSAGE_MAX_LEN];
} device_online_snapshot_t;

esp_err_t device_online_init(const device_online_config_t *config);
void device_online_set_network_ready(bool ready);
void device_online_set_realtime_media_active(bool active);
esp_err_t device_online_start_async(const char *reason);
void device_online_stop(void);
esp_err_t device_online_notify_credentials_changed(const char *reason);
void device_online_notify_credentials_cleared(const char *reason);
void device_online_invalidate_cache(void);
esp_err_t device_online_get_cached_credentials(device_online_credentials_t *credentials);
esp_err_t device_online_get_cached_mqtt_token(device_auth_token_t *token);
esp_err_t device_online_report_state_async(const char *reason);
bool device_online_is_online(void);
void device_online_get_snapshot(device_online_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
