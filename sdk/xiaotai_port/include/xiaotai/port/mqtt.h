// SPDX-License-Identifier: MIT
#pragma once
/* MQTT port: an MQTT 3.1.1 client with automatic reconnection.
 *
 * The port owns the connection, the keep-alive, the reconnect timer and the
 * client's task. What is subscribed, when a subscription counts as ready, how
 * incoming messages are reassembled and acknowledged, and every deadline stay
 * above the port: the port reports the protocol facts those decisions need
 * (SUBACK return codes, PUBACK packet identifiers, message fragments).
 *
 * Every event is delivered on the client's owner task, one at a time. That task
 * has an internal-RAM stack because it runs while flash writes are in progress;
 * its handler must return quickly: copy, queue, count. It must not block on
 * storage, audio or the network, and must not log on a path that runs per
 * message. xiaotai_mqtt_wake lets another task have a deadline evaluated on the
 * owner task, so state touched by the handler needs no lock. The ESP stock
 * backend copies borrowed native events into a bounded mailbox before delivery.
 * The optional overlay delivers on the native MQTT task. Mailbox saturation
 * reports a protocol error, not a synthetic acknowledgment.
 *
 * mqtts:// is verified against the platform's certificate store.
 *
 * Memory over a client's life. Products that release the connection while
 * memory is short (a call that needs the internal RAM, a start-up step) run
 * create -> start -> stop -> destroy again and again, so what each step holds
 * is part of the contract:
 *
 *   create   the client object and the copies of the configuration's strings
 *            (internal RAM), the receive and transmit packet buffers
 *            (rx_buffer_bytes, tx_buffer_bytes), the outbox bookkeeping. The
 *            stock ESP backend also reserves bounded callback copy slots.
 *            When payloads are allocated on demand, the slots stay bounded;
 *            each DATA copy is owned until its handler returns, and failure
 *            is reported in order as a protocol gap.
 *   start    adds the client's task stack (task_stack_bytes, internal RAM)
 *            and, on stock ESP only, a second stack (event_stack_bytes,
 *            defaulting to task_stack_bytes) for the serial owner.
 *            Every connection attempt adds the socket and, for
 *            mqtts://, the TLS session with its record buffers.
 *   stop     closes the connection: the socket and the TLS session with its
 *            buffers are released before it returns. Ends the task; its stack
 *            is returned when the platform reaps the ended task (on FreeRTOS
 *            the idle task, the next time it runs), not necessarily before
 *            stop returns. Keeps the client object, the string copies, the
 *            packet buffers and empty outbox bookkeeping. Discards queued
 *            publishes, including QoS 1 messages still awaiting PUBACK; a later
 *            start does not replay them. Automatic reconnect while started
 *            retains pending publishes; explicit stop ends that lifetime.
 *   destroy  releases everything create allocated and wipes port-owned string
 *            copies. The stock library controls erasure of its own copies.
 *
 * Cycle guarantee: once the first create -> start -> (connect) -> stop ->
 * destroy has run (it may leave one-time state the platform keeps for good:
 * the TLS library's and the socket layer's lazily created tables), every
 * further identical cycle returns all internal RAM it took. Measured after the
 * ended task was reaped and once no closed connection is waiting out its TCP
 * close timer (the platform keeps one small control block per connection the
 * device closed, for up to twice the stack's maximum segment lifetime; that
 * is the socket layer's and not counted), the free internal RAM after cycle
 * n + 1 equals that after cycle n for n >= 1, and the largest free internal
 * block is not smaller. The port's host test checks it with allocation
 * counts, the on-board self-check with 10 cycles. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xiaotai_mqtt xiaotai_mqtt_t;

/* Topics of one SUBSCRIBE request. */
#define XIAOTAI_MQTT_SUBSCRIBE_MAX 4U
/* Longest topic the port keeps for an incoming message, without terminator. */
#define XIAOTAI_MQTT_TOPIC_MAX 160U
/* SUBACK return code for a refused topic; 0, 1 and 2 are the granted QoS. */
#define XIAOTAI_MQTT_REFUSED 0x80U

typedef enum {
    /* The broker accepted the connection. Subscriptions are gone after every
     * reconnect: subscribe again. */
    XIAOTAI_MQTT_CONNECTED = 0,
    /* The connection closed or broke. The client reconnects by itself after
     * reconnect_ms until xiaotai_mqtt_stop. */
    XIAOTAI_MQTT_DISCONNECTED,
    /* SUBACK: message_id of the request, one return code per topic. */
    XIAOTAI_MQTT_SUBSCRIBED,
    /* PUBACK for a QoS 1 publish: message_id. */
    XIAOTAI_MQTT_PUBLISHED,
    /* One fragment of an incoming message. */
    XIAOTAI_MQTT_DATA,
    /* A failure the client will recover from by reconnecting, or a refusal. */
    XIAOTAI_MQTT_ERROR,
    /* xiaotai_mqtt_wake was called. */
    XIAOTAI_MQTT_WAKE,
} xiaotai_mqtt_event_kind_t;

typedef enum {
    XIAOTAI_MQTT_ERROR_NONE = 0,
    XIAOTAI_MQTT_ERROR_TRANSPORT,   /* resolve, connect, read or write; detail: socket errno */
    XIAOTAI_MQTT_ERROR_TLS,         /* handshake or certificate; detail: TLS code */
    XIAOTAI_MQTT_ERROR_REFUSED,     /* CONNACK refused; detail: the CONNACK return code */
    XIAOTAI_MQTT_ERROR_PROTOCOL,    /* malformed or unexpected packet */
} xiaotai_mqtt_error_t;

/* Valid during the handler call only; copy what must outlive it. */
typedef struct {
    xiaotai_mqtt_event_kind_t kind;

    /* SUBSCRIBED, PUBLISHED, DATA (QoS 1 and 2): packet identifier. Identifiers
     * may repeat within one connection: match them against the request that is
     * outstanding, not against history. On a protocol ERROR a positive ID
     * identifies a malformed SUBACK observed by the port; other errors have ID 0. */
    int32_t message_id;

    /* SUBSCRIBED: return codes in the order of the request's topics. */
    const uint8_t *granted;
    size_t granted_count;

    /* DATA. A message arrives as one or more fragments with increasing offset;
     * offset + size == total on the last one. topic is present and identical on
     * every fragment of a message and is not terminated. A fragment may be
     * empty (size 0). Fragments of different messages are never interleaved. */
    const char *topic;
    size_t topic_size;
    const uint8_t *data;
    size_t size;
    size_t offset;
    size_t total;
    uint8_t qos;
    bool retained;
    bool duplicate;

    /* ERROR. */
    xiaotai_mqtt_error_t error;
    int32_t detail;
} xiaotai_mqtt_event_t;

typedef void (*xiaotai_mqtt_handler_t)(void *ctx, const xiaotai_mqtt_event_t *event);

typedef struct {
    /* mqtt://host[:port] or mqtts://host[:port]. */
    const char *uri;
    const char *client_id;
    const char *username;         /* may be NULL */
    const char *password;         /* may be NULL */
    uint16_t keepalive_s;         /* 0: the port's default (60) */
    uint32_t reconnect_ms;        /* 0: the port's default (3000) */
    /* Packet buffers; 0: the port's default. An incoming message longer than
     * the receive buffer arrives as several fragments. */
    uint16_t rx_buffer_bytes;
    uint16_t tx_buffer_bytes;
    /* Most bytes queued for sending (queued publishes, QoS 1 awaiting PUBACK);
     * 0: the port's default. A publish beyond it fails with XIAOTAI_ERR_NO_MEM. */
    uint32_t outbox_limit_bytes;
    /* Stack of the client's task; 0: the port's default. Always internal RAM. */
    uint32_t task_stack_bytes;
    xiaotai_mqtt_handler_t handler;
    void *ctx;
    /* Stock ESP serial owner only, in internal RAM. Zero inherits
     * task_stack_bytes; otherwise 2560..16384 bytes. */
    uint32_t event_stack_bytes;
} xiaotai_mqtt_config_t;

/* [any task that may block] Creates a stopped client. Every string of config is
 * copied; the caller may wipe its own copy (tokens) right after the call. Port
 * copies are wiped when the client is destroyed. *client is NULL on failure.
 * XIAOTAI_ERR_INVALID_ARG: missing uri, client_id or handler, a scheme other
 * than mqtt:// and mqtts://. XIAOTAI_ERR_NOT_SUPPORTED: mqtts:// in a build
 * without TLS. XIAOTAI_ERR_NO_MEM. */
xiaotai_err_t xiaotai_mqtt_create(const xiaotai_mqtt_config_t *config, xiaotai_mqtt_t **client);

/* [any task that may block; not the handler] Starts the client's task and the
 * first connection attempt. XIAOTAI_ERR_INVALID_STATE when already started. */
xiaotai_err_t xiaotai_mqtt_start(xiaotai_mqtt_t *client);

/* [any task that may block; not the handler] Disconnects and stops the client's
 * task. When it returns XIAOTAI_OK no handler call is running and none will
 * follow until the next start, the socket and TLS session are released, and
 * pending publishes are discarded (see "Memory over a client's life" above
 * for what stays). On failure the
 * client is unchanged and still owned by the caller. */
xiaotai_err_t xiaotai_mqtt_stop(xiaotai_mqtt_t *client);

/* [any task that may block; not the handler] Frees a stopped client: its
 * object, packet buffers and outbox with every queued message, and wipes the
 * port-owned credentials. XIAOTAI_ERR_INVALID_STATE when it is still
 * started; the client then stays valid. NULL is accepted. */
xiaotai_err_t xiaotai_mqtt_destroy(xiaotai_mqtt_t *client);

typedef struct {
    const char *topic;
    uint8_t qos;     /* 0..2 requested */
} xiaotai_mqtt_topic_t;

/* [any, including the handler] Sends one SUBSCRIBE packet for count topics
 * (1..XIAOTAI_MQTT_SUBSCRIBE_MAX). *message_id identifies the SUBACK.
 * XIAOTAI_ERR_INVALID_STATE when not connected; XIAOTAI_FAIL when the packet
 * could not be written. Success means sent, not acknowledged. */
xiaotai_err_t xiaotai_mqtt_subscribe(xiaotai_mqtt_t *client, const xiaotai_mqtt_topic_t *topics,
                                     size_t count, int32_t *message_id);

typedef enum {
    /* Write the packet on the calling task before returning. From the handler
     * this delays every other event by the write. */
    XIAOTAI_MQTT_SEND_NOW = 0,
    /* Copy the packet to the outbox; the client task sends it. Stock esp-mqtt
     * may wait for its native API lock but does not send on the caller. */
    XIAOTAI_MQTT_SEND_QUEUED,
} xiaotai_mqtt_send_t;

/* [any, including the handler] Publishes data (copied) to topic with QoS 0 or 1.
 * message_id may be NULL; for QoS 1 it receives the identifier the PUBLISHED
 * event will carry, for QoS 0 it receives 0.
 * XIAOTAI_ERR_INVALID_STATE: already observed disconnected on admission. In
 * stock esp-mqtt, a concurrent native disconnect can race with enqueue and
 * a failing QoS 1 NOW can leave an item awaiting automatic retransmission.
 * XIAOTAI_ERR_NO_MEM: the outbox is full. XIAOTAI_FAIL: the write failed.
 * XIAOTAI_ERR_INVALID_ARG: qos above 1, empty topic. */
xiaotai_err_t xiaotai_mqtt_publish(xiaotai_mqtt_t *client, const char *topic, const void *data,
                                   size_t size, uint8_t qos, xiaotai_mqtt_send_t send,
                                   int32_t *message_id);

/* [any task, not an interrupt] Queues one XIAOTAI_MQTT_WAKE event to the
 * owner task. Wakes that are still queued are not counted: several calls may
 * produce one event. XIAOTAI_ERR_INVALID_STATE when the client is not started. */
xiaotai_err_t xiaotai_mqtt_wake(xiaotai_mqtt_t *client);

typedef struct {
    uint32_t connects;        /* accepted connections */
    uint32_t disconnects;
    uint32_t errors;
    uint32_t messages;        /* complete incoming messages */
    uint32_t fragments;
    uint32_t published;
    uint32_t publish_failures;
    uint32_t outbox_full;
    bool connected;
} xiaotai_mqtt_stats_t;

/* [any] Counters of one client since it was created. */
void xiaotai_mqtt_stats(xiaotai_mqtt_t *client, xiaotai_mqtt_stats_t *stats);

#ifdef __cplusplus
}
#endif
