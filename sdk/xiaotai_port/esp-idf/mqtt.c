// SPDX-License-Identifier: MIT
#include "../common/mqtt_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "sdkconfig.h"
#include "mqtt_client.h"
#if defined(CONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY) && CONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY
#define MQTT_NATIVE_OVERLAY 1
#include "mqtt_owner_api.h"
#include "mqtt_native_api.h"
#else
#define MQTT_NATIVE_OVERLAY 0
#endif
#if CONFIG_MQTT_TRANSPORT_SSL && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#define NATIVE_TLS 1
#else
#define NATIVE_TLS 0
#endif
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#if CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY
#error "XiaoTai MQTT requires an internal client task stack"
#endif
#if !CONFIG_MQTT_PROTOCOL_311
#error "XiaoTai MQTT requires MQTT 3.1.1"
#endif

enum { STOPPED, STARTING, STARTED, STOPPING, DESTROYING };

#if !MQTT_NATIVE_OVERLAY
typedef struct mqtt_stock mqtt_stock_t;
#endif

struct xiaotai_mqtt {
    mqtt_port_config_t config;
    esp_mqtt_client_handle_t native;
    xiaotai_mutex_t management;
    atomic_uint state;
    atomic_bool connected;
    atomic_uint connects, disconnects, errors, messages, fragments, published, publish_failures, outbox_full;
    char topic[XIAOTAI_MQTT_TOPIC_MAX];
    size_t topic_size, total, next;
    int32_t packet;
    uint8_t qos;
    bool retained, duplicate;
#if !MQTT_NATIVE_OVERLAY
    mqtt_stock_t *stock;
#endif
};

#define ADD(c, key) atomic_fetch_add_explicit(&(c)->key, 1, memory_order_relaxed)

static void emit(xiaotai_mqtt_t *c, const xiaotai_mqtt_event_t *event)
{
    c->config.value.handler(c->config.value.ctx, event);
}

static void protocol_error(xiaotai_mqtt_t *c, int detail, int32_t message_id)
{
    ADD(c, errors);
    emit(c, &(xiaotai_mqtt_event_t){
        .kind = XIAOTAI_MQTT_ERROR, .error = XIAOTAI_MQTT_ERROR_PROTOCOL, .detail = detail,
        .message_id = message_id,
    });
}

static void report_error(xiaotai_mqtt_t *c, const esp_mqtt_error_codes_t *err)
{
    if (!err || err->error_type == MQTT_ERROR_TYPE_NONE || err->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
        protocol_error(c, err ? err->error_type : 0, 0);
        return;
    }
    xiaotai_mqtt_event_t event = {.kind = XIAOTAI_MQTT_ERROR};
    if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        event.error = XIAOTAI_MQTT_ERROR_REFUSED;
        event.detail = err->connect_return_code;
    } else if (err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        if (err->esp_tls_stack_err || err->esp_tls_cert_verify_flags) {
            event.error = XIAOTAI_MQTT_ERROR_TLS;
            event.detail = err->esp_tls_stack_err ? err->esp_tls_stack_err : err->esp_tls_cert_verify_flags;
        } else {
            event.error = XIAOTAI_MQTT_ERROR_TRANSPORT;
            event.detail = err->esp_transport_sock_errno ? err->esp_transport_sock_errno : err->esp_tls_last_esp_err;
        }
    } else {
        event.error = XIAOTAI_MQTT_ERROR_PROTOCOL;
        event.detail = err->error_type;
    }
    ADD(c, errors);
    emit(c, &event);
}

static void subscribed(xiaotai_mqtt_t *c, const esp_mqtt_event_t *native)
{
    if (!native->data || native->data_len < 1 || native->data_len > (int)XIAOTAI_MQTT_SUBSCRIBE_MAX) {
        protocol_error(c, MQTT_EVENT_SUBSCRIBED, native->msg_id > 0 ? native->msg_id : 0);
        return;
    }
    const uint8_t *codes = (const uint8_t *)native->data;
    for (int i = 0; i < native->data_len; ++i) {
        if (codes[i] > 2 && codes[i] != XIAOTAI_MQTT_REFUSED) {
            protocol_error(c, MQTT_EVENT_SUBSCRIBED, native->msg_id > 0 ? native->msg_id : 0);
            return;
        }
    }
    emit(c, &(xiaotai_mqtt_event_t){
        .kind = XIAOTAI_MQTT_SUBSCRIBED, .message_id = native->msg_id,
        .granted = codes, .granted_count = (size_t)native->data_len,
    });
}

static void data(xiaotai_mqtt_t *c, const esp_mqtt_event_t *native)
{
    const int offset = native->current_data_offset, size = native->data_len, total = native->total_data_len;
    if (offset < 0 || total < offset || size < 0 || size > total - offset || (size && !native->data) ||
        native->qos < 0 || native->qos > 2) goto invalid;
    /* An empty topic-only first event and its first payload can share offset
     * zero. Only a present topic starts a new cached message. */
    if (!c->topic_size) {
        if (offset || !native->topic || native->topic_len < 1 || native->topic_len > (int)sizeof(c->topic))
            goto invalid;
        c->topic_size = (size_t)native->topic_len;
        memcpy(c->topic, native->topic, c->topic_size);
        c->total = (size_t)total;
        c->next = 0;
        c->packet = native->msg_id;
        c->qos = (uint8_t)native->qos;
        c->retained = native->retain;
        c->duplicate = native->dup;
    } else if ((size_t)offset != c->next || (size_t)total != c->total || native->msg_id != c->packet ||
               (native->topic && ((size_t)native->topic_len != c->topic_size ||
                                  memcmp(native->topic, c->topic, c->topic_size)))) {
        goto invalid;
    }
    c->next = (size_t)(offset + size);
    ADD(c, fragments);
    if (c->next == c->total) ADD(c, messages);
    emit(c, &(xiaotai_mqtt_event_t){
        .kind = XIAOTAI_MQTT_DATA, .message_id = c->qos ? c->packet : 0,
        .topic = c->topic, .topic_size = c->topic_size, .data = (const uint8_t *)native->data,
        .size = (size_t)size, .offset = (size_t)offset, .total = (size_t)total,
        .qos = c->qos, .retained = c->retained, .duplicate = c->duplicate,
    });
    if (c->next == c->total) c->topic_size = 0;
    return;
invalid:
    c->topic_size = 0;
    protocol_error(c, MQTT_EVENT_DATA, 0);
}

static void native_event(void *arg, esp_event_base_t base, int32_t id, void *event_ptr)
{
    (void)base;
    xiaotai_mqtt_t *c = arg;
    const esp_mqtt_event_t *native = event_ptr;
    const unsigned state = atomic_load_explicit(&c->state, memory_order_acquire);
    if (state != STARTING && state != STARTED && state != STOPPING) return;
    /* Both backends deliver serially on their owner. The record is borrowed. */
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        atomic_store_explicit(&c->connected, true, memory_order_release);
        ADD(c, connects);
        emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_CONNECTED});
        break;
    case MQTT_EVENT_DISCONNECTED:
        c->topic_size = 0;
        atomic_store_explicit(&c->connected, false, memory_order_release);
        ADD(c, disconnects);
        emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_DISCONNECTED});
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (native) subscribed(c, native);
        break;
    case MQTT_EVENT_PUBLISHED:
        if (native) emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_PUBLISHED, .message_id = native->msg_id});
        break;
    case MQTT_EVENT_DATA:
        if (native) data(c, native);
        break;
    case MQTT_EVENT_ERROR:
        report_error(c, native ? native->error_handle : NULL);
        break;
    case MQTT_USER_EVENT:
        emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_WAKE});
        break;
    default: break;
    }
}

#if !MQTT_NATIVE_OVERLAY
#include "mqtt_stock.inc"
#endif

xiaotai_err_t xiaotai_mqtt_create(const xiaotai_mqtt_config_t *config, xiaotai_mqtt_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    mqtt_port_config_t copied;
    xiaotai_err_t rc = mqtt_port_config_copy(config, NATIVE_TLS, &copied);
    if (rc != XIAOTAI_OK) return rc;
    if (copied.value.reconnect_ms > INT_MAX || copied.value.task_stack_bytes > INT_MAX ||
        copied.value.outbox_limit_bytes > INT_MAX) {
        mqtt_port_config_clear(&copied);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    xiaotai_mqtt_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) { mqtt_port_config_clear(&copied); return XIAOTAI_ERR_NO_MEM; }
    c->config = copied;
    atomic_init(&c->state, STOPPED);
    atomic_init(&c->connected, false);
    atomic_init(&c->connects, 0); atomic_init(&c->disconnects, 0);
    atomic_init(&c->errors, 0); atomic_init(&c->messages, 0); atomic_init(&c->fragments, 0);
    atomic_init(&c->published, 0); atomic_init(&c->publish_failures, 0); atomic_init(&c->outbox_full, 0);
    rc = xiaotai_mutex_init(&c->management);
    if (rc != XIAOTAI_OK) {
        mqtt_port_config_clear(&c->config);
        xiaotai_mem_free(c);
        return rc;
    }
    const xiaotai_mqtt_config_t *cfg = &c->config.value;
    const esp_mqtt_client_config_t native = {
        .broker.address.uri = cfg->uri,
        .credentials = {.client_id = cfg->client_id, .username = cfg->username, .authentication.password = cfg->password},
        .session = {.protocol_ver = MQTT_PROTOCOL_V_3_1_1, .keepalive = cfg->keepalive_s},
        .network.reconnect_timeout_ms = (int)cfg->reconnect_ms,
        .task.stack_size = (int)cfg->task_stack_bytes,
        .buffer = {.size = cfg->rx_buffer_bytes, .out_size = cfg->tx_buffer_bytes},
        .outbox.limit = cfg->outbox_limit_bytes,
#if NATIVE_TLS
        .broker.verification.crt_bundle_attach = c->config.tls ? esp_crt_bundle_attach : NULL,
#endif
    };
    c->native = esp_mqtt_client_init(&native);
#if MQTT_NATIVE_OVERLAY
    rc = c->native ? xiaotai_idf_mqtt_owner_attach(c->native, native_event, c) : XIAOTAI_ERR_NO_MEM;
#else
    rc = c->native ? stock_attach(c) : XIAOTAI_ERR_NO_MEM;
#endif
    if (rc != XIAOTAI_OK) {
        if (c->native) esp_mqtt_client_destroy(c->native);
        xiaotai_mutex_deinit(&c->management);
        mqtt_port_config_clear(&c->config);
        xiaotai_mem_free(c);
        return rc;
    }
    *out = c;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_mqtt_start(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
#if MQTT_NATIVE_OVERLAY
    if (xiaotai_idf_mqtt_owner_current(c->native)) return XIAOTAI_ERR_INVALID_STATE;
#else
    if (stock_owner_current(c)) return XIAOTAI_ERR_INVALID_STATE;
#endif
    if (!xiaotai_mutex_lock(&c->management, XIAOTAI_WAIT_FOREVER)) return XIAOTAI_FAIL;
    unsigned expect = STOPPED;
    if (!atomic_compare_exchange_strong(&c->state, &expect, STARTING)) {
        xiaotai_mutex_unlock(&c->management);
        return XIAOTAI_ERR_INVALID_STATE;
    }
#if MQTT_NATIVE_OVERLAY
    xiaotai_err_t rc = xiaotai_idf_mqtt_owner_start(c->native);
#else
    xiaotai_err_t rc = stock_start(c);
#endif
    atomic_store(&c->state, rc == XIAOTAI_OK ? STARTED : STOPPED);
    xiaotai_mutex_unlock(&c->management);
    return rc;
}

xiaotai_err_t xiaotai_mqtt_stop(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
#if MQTT_NATIVE_OVERLAY
    if (xiaotai_idf_mqtt_owner_current(c->native)) return XIAOTAI_ERR_INVALID_STATE;
#else
    if (stock_owner_current(c)) return XIAOTAI_ERR_INVALID_STATE;
#endif
    if (!xiaotai_mutex_lock(&c->management, XIAOTAI_WAIT_FOREVER)) return XIAOTAI_FAIL;
    unsigned expect = STARTED;
    if (!atomic_compare_exchange_strong(&c->state, &expect, STOPPING)) {
        xiaotai_mutex_unlock(&c->management);
        return expect == STOPPED ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    }
    /* Explicit lifecycle shutdown discards native pending publishes. A new
     * connection is created by the product's stop/destroy/recreate flow. */
#if MQTT_NATIVE_OVERLAY
    xiaotai_err_t rc = xiaotai_idf_mqtt_owner_stop(c->native);
#else
    xiaotai_err_t rc = stock_stop(c);
#endif
    if (rc == XIAOTAI_OK) {
        atomic_store(&c->connected, false);
        c->topic_size = 0;
    }
    atomic_store(&c->state, rc == XIAOTAI_OK ? STOPPED : STARTED);
    xiaotai_mutex_unlock(&c->management);
    return rc;
}

xiaotai_err_t xiaotai_mqtt_destroy(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_OK;
#if MQTT_NATIVE_OVERLAY
    if (xiaotai_idf_mqtt_owner_current(c->native)) return XIAOTAI_ERR_INVALID_STATE;
#else
    if (stock_owner_current(c)) return XIAOTAI_ERR_INVALID_STATE;
#endif
    if (!xiaotai_mutex_lock(&c->management, XIAOTAI_WAIT_FOREVER)) return XIAOTAI_FAIL;
    unsigned expect = STOPPED;
    if (!atomic_compare_exchange_strong(&c->state, &expect, DESTROYING)) {
        xiaotai_mutex_unlock(&c->management);
        return XIAOTAI_ERR_INVALID_STATE;
    }
#if MQTT_NATIVE_OVERLAY
    xiaotai_err_t rc = xiaotai_idf_mqtt_owner_destroy(c->native);
#else
    xiaotai_err_t rc = stock_destroy(c);
#endif
    if (rc != XIAOTAI_OK) {
        atomic_store(&c->state, STOPPED);
        xiaotai_mutex_unlock(&c->management);
        return rc;
    }
    xiaotai_mutex_unlock(&c->management);
    xiaotai_mutex_deinit(&c->management);
    mqtt_port_config_clear(&c->config);
    xiaotai_mem_free(c);
    return XIAOTAI_OK;
}

static bool running(const xiaotai_mqtt_t *c)
{
    const unsigned state = atomic_load_explicit(&c->state, memory_order_acquire);
    return state == STARTED || state == STARTING;
}

static bool connected(xiaotai_mqtt_t *c)
{
    return running(c) && atomic_load_explicit(&c->connected, memory_order_acquire);
}

xiaotai_err_t xiaotai_mqtt_subscribe(xiaotai_mqtt_t *c, const xiaotai_mqtt_topic_t *topics,
                                     size_t count, int32_t *message_id)
{
    if (!c || !message_id || !mqtt_port_topics_valid(topics, count)) return XIAOTAI_ERR_INVALID_ARG;
    *message_id = 0;
    xiaotai_err_t rc = XIAOTAI_ERR_INVALID_STATE;
    if (connected(c)) {
        esp_mqtt_topic_t native[XIAOTAI_MQTT_SUBSCRIBE_MAX];
        for (size_t i = 0; i < count; ++i)
            native[i] = (esp_mqtt_topic_t){.filter = topics[i].topic, .qos = topics[i].qos};
        int id = 0;
#if MQTT_NATIVE_OVERLAY
        rc = xiaotai_idf_mqtt_subscribe_connected(c->native, native, (int)count, &id);
#else
        rc = stock_subscribe(c, native, (int)count, &id);
#endif
        if (rc == XIAOTAI_OK) *message_id = id;
    }
    return rc;
}

xiaotai_err_t xiaotai_mqtt_publish(xiaotai_mqtt_t *c, const char *topic, const void *data_ptr,
                                   size_t size, uint8_t qos, xiaotai_mqtt_send_t send, int32_t *message_id)
{
    if (message_id) *message_id = 0;
    size_t bytes;
    if (!c || !mqtt_port_publish_size(topic, data_ptr, size, qos, send, &bytes)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t rc = XIAOTAI_ERR_INVALID_STATE;
    if (connected(c)) {
        const char *body = size ? data_ptr : NULL;
        int id = 0;
#if MQTT_NATIVE_OVERLAY
        /* The native boundary owns the final connection/admission decision.
         * In particular QUEUED must not take get_state/outbox_size's API lock
         * here: that lock can be held across DNS/TLS or synchronous writes. */
        rc = send == XIAOTAI_MQTT_SEND_QUEUED
            ? xiaotai_idf_mqtt_enqueue_connected(c->native, topic, body, (int)size, qos, bytes, &id)
            : xiaotai_idf_mqtt_publish_now(c->native, topic, body, (int)size, qos, bytes, &id);
#else
        rc = stock_publish(c, topic, body, (int)size, qos, send, bytes, &id);
#endif
        if (rc == XIAOTAI_OK) {
            if (message_id && qos) *message_id = id;
            ADD(c, published);
        }
    }
    if (rc != XIAOTAI_OK) ADD(c, publish_failures);
    if (rc == XIAOTAI_ERR_NO_MEM) ADD(c, outbox_full);
    return rc;
}

xiaotai_err_t xiaotai_mqtt_wake(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_STATE;
    const unsigned state = atomic_load_explicit(&c->state, memory_order_acquire);
#if MQTT_NATIVE_OVERLAY
    return (state == STARTED || state == STARTING) && xiaotai_idf_mqtt_owner_wake(c->native)
        ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
#else
    return (state == STARTED || state == STARTING) && stock_wake(c)
        ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
#endif
}

void xiaotai_mqtt_stats(xiaotai_mqtt_t *c, xiaotai_mqtt_stats_t *out)
{
    if (!out) return;
    *out = (xiaotai_mqtt_stats_t){0};
    if (!c) return;
#define READ(key) out->key = atomic_load_explicit(&c->key, memory_order_relaxed)
    READ(connects); READ(disconnects); READ(errors); READ(messages); READ(fragments);
    READ(published); READ(publish_failures); READ(outbox_full); READ(connected);
#undef READ
}
