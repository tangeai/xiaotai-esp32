// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
typedef enum {
    MQTT_EVENT_ANY = -1, MQTT_EVENT_ERROR, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_UNSUBSCRIBED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_DATA,
    MQTT_EVENT_BEFORE_CONNECT, MQTT_EVENT_DELETED, MQTT_USER_EVENT,
} esp_mqtt_event_id_t;
typedef enum {
    MQTT_ERROR_TYPE_NONE, MQTT_ERROR_TYPE_TCP_TRANSPORT,
    MQTT_ERROR_TYPE_CONNECTION_REFUSED, MQTT_ERROR_TYPE_SUBSCRIBE_FAILED,
} esp_mqtt_error_type_t;
typedef enum {
    MQTT_CLIENT_STATE_NOT_INITIALIZED, MQTT_CLIENT_STATE_NOT_STARTED, MQTT_CLIENT_STATE_DISCONNECTED,
    MQTT_CLIENT_STATE_CONNECTED, MQTT_CLIENT_STATE_WAITING_RECONNECT,
} esp_mqtt_client_connection_state_t;
typedef struct {
    int esp_tls_last_esp_err, esp_tls_stack_err, esp_tls_cert_verify_flags;
    esp_mqtt_error_type_t error_type;
    int connect_return_code, esp_transport_sock_errno;
} esp_mqtt_error_codes_t;
typedef struct {
    esp_mqtt_event_id_t event_id;
    esp_mqtt_client_handle_t client;
    char *data, *topic;
    int data_len, total_data_len, current_data_offset, topic_len, msg_id, qos;
    bool retain, dup;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;
enum { MQTT_PROTOCOL_V_3_1_1 = 2 };
typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *); bool skip_cert_common_name_check; } verification;
    } broker;
    struct {
        const char *client_id, *username;
        struct { const char *password; } authentication;
    } credentials;
    struct { int protocol_ver, keepalive; bool disable_clean_session; } session;
    struct { int reconnect_timeout_ms; bool disable_auto_reconnect; } network;
    struct { int stack_size; } task;
    struct { int size, out_size; } buffer;
    struct { uint64_t limit; } outbox;
} esp_mqtt_client_config_t;
typedef struct { const char *filter; int qos; } esp_mqtt_topic_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, esp_mqtt_event_id_t, esp_event_handler_t, void *);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(esp_mqtt_client_handle_t);
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe_multiple(esp_mqtt_client_handle_t, const esp_mqtt_topic_t *, int);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, int, bool);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, int);
esp_err_t esp_mqtt_dispatch_custom_event(esp_mqtt_client_handle_t, esp_mqtt_event_t *);
