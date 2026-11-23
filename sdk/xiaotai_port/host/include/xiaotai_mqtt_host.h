// SPDX-License-Identifier: MIT
#pragma once
/* In-process broker controls. No DNS, sockets or TLS are used. These model
 * already decoded broker packets, not a byte-level MQTT transport. */
#include "xiaotai/port/mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Next connection attempts use this outcome; NONE accepts the connection. */
void xiaotai_mqtt_host_connack(xiaotai_mqtt_t *client, xiaotai_mqtt_error_t error, int32_t detail);
xiaotai_err_t xiaotai_mqtt_host_disconnect(xiaotai_mqtt_t *client);
/* Increment only this client's broker clock for deterministic reconnect tests. */
void xiaotai_mqtt_host_advance_ms(xiaotai_mqtt_t *client, uint32_t ms);

/* Future subscriptions: false withholds their SUBACK, and count 0 grants
 * the requested QoS. Nonempty overrides must match the request count. */
xiaotai_err_t xiaotai_mqtt_host_suback(xiaotai_mqtt_t *client, bool send, const uint8_t *codes, size_t count);
xiaotai_err_t xiaotai_mqtt_host_suback_reply(xiaotai_mqtt_t *client, uint16_t id);
/* Next request uses id if no QoS 1 publish is still using it. */
void xiaotai_mqtt_host_next_id(xiaotai_mqtt_t *client, uint16_t id);
size_t xiaotai_mqtt_host_subscriptions(xiaotai_mqtt_t *client);

/* Inject a broker PUBLISH, even if no subscription matches. With no explicit
 * fragmentation, split at rx_buffer_bytes. Explicit lengths may include empty
 * leading fragments and must consume exactly size bytes. Copies inputs. */
xiaotai_err_t xiaotai_mqtt_host_message(xiaotai_mqtt_t *client, const char *topic,
    const void *data, size_t size, uint8_t qos, uint16_t id, bool retained, bool duplicate,
    const size_t *fragment_sizes, size_t fragments);

/* Future sends receive PUBACK automatically by default, or wait for ack(). */
void xiaotai_mqtt_host_puback(xiaotai_mqtt_t *client, bool automatic);
xiaotai_err_t xiaotai_mqtt_host_ack(xiaotai_mqtt_t *client, uint16_t id);
void xiaotai_mqtt_host_fail_write(xiaotai_mqtt_t *client);

typedef struct {
    int32_t message_id;
    uint8_t qos;
    bool duplicate; /* a QoS 1 message resent after reconnect */
    size_t size;
} xiaotai_mqtt_host_publication_t;

/* Peek size and retry after INVALID_SIZE; on success consumes the oldest
 * publication. Observation history is bounded to 32, released on stop. */
xiaotai_err_t xiaotai_mqtt_host_take(xiaotai_mqtt_t *client, xiaotai_mqtt_host_publication_t *out,
    char *topic, size_t topic_capacity, void *data, size_t data_capacity);

/* Wait for presently runnable work to drain, not for a future reconnect or
 * a withheld broker acknowledgement. Never call from the client callback. */
xiaotai_err_t xiaotai_mqtt_host_flush(xiaotai_mqtt_t *client, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
