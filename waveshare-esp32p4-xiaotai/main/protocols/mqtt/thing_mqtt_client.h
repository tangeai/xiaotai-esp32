#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*thing_mqtt_message_cb_t)(const char *topic,
                                        const char *payload,
                                        size_t payload_len,
                                        void *ctx);

typedef void (*thing_mqtt_disconnect_cb_t)(uint8_t reason_code, void *ctx);

typedef esp_err_t (*thing_mqtt_heartbeat_payload_cb_t)(char *buffer,
                                                       size_t buffer_size,
                                                       uint32_t seq,
                                                       void *ctx);

typedef int thing_mqtt_listener_handle_t;

typedef struct {
    const char *broker_uri;
    const char *device_id;
    const char *mqtt_token;
    uint32_t heartbeat_interval_ms;
    thing_mqtt_message_cb_t on_message;
    void *ctx;
    thing_mqtt_disconnect_cb_t on_disconnect;
    void *disconnect_ctx;
    thing_mqtt_heartbeat_payload_cb_t build_heartbeat;
    void *heartbeat_ctx;
} thing_mqtt_client_config_t;

esp_err_t thing_mqtt_client_start(const thing_mqtt_client_config_t *config);
void thing_mqtt_client_stop(void);
bool thing_mqtt_client_is_started(void);
bool thing_mqtt_client_is_connected(void);
esp_err_t thing_mqtt_client_wait_last_command_ack(uint32_t timeout_ms);
esp_err_t thing_mqtt_client_publish_up(const char *payload, int qos);
esp_err_t thing_mqtt_client_add_listener(thing_mqtt_message_cb_t on_message,
                                         void *ctx,
                                         thing_mqtt_listener_handle_t *handle);
void thing_mqtt_client_remove_listener(thing_mqtt_listener_handle_t handle);

#ifdef __cplusplus
}
#endif
