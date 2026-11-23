// SPDX-License-Identifier: MIT
#include "xiaotai/port/mqtt.h"
#include "xiaotai_mem_host.h"
#include "mqtt_client.h"
#include "../esp-idf/mqtt_owner_api.h"
#include "../esp-idf/mqtt_native_api.h"
#include "../esp-idf/mqtt_owner_mailbox.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct esp_mqtt_client {
    esp_mqtt_client_config_t config;
    esp_event_handler_t handler;
    void *ctx;
    bool running, connected, queued;
    int outbox;
    xt_mqtt_owner_mailbox_t wake;
    uint32_t epoch;
    unsigned start_count, stop_count, wake_calls, enqueue_calls, publish_calls;
};
static struct esp_mqtt_client *current;
static bool fail_init, fail_registration, fail_start, fail_stop, fail_write_after_enqueue;
static bool worker, force_full;
static int application_task, worker_task;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return worker ? &worker_task : &application_task; }
esp_err_t esp_crt_bundle_attach(void *cfg) { (void)cfg; return XIAOTAI_OK; }

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    if (fail_init) return NULL;
    assert(!current);
    current = calloc(1, sizeof(*current));
    assert(current);
    current->config = *cfg;
    return current;
}

esp_err_t xiaotai_idf_mqtt_owner_attach(esp_mqtt_client_handle_t c, esp_event_handler_t handler, void *ctx)
{
    assert(c == current);
    if (fail_registration) return XIAOTAI_ERR_NO_MEM;
    c->handler = handler;
    c->ctx = ctx;
    xt_mqtt_mailbox_init(&c->wake);
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c)
{
    assert(c == current && !c->running);
    if (fail_start) return XIAOTAI_FAIL;
    c->running = true;
    ++c->start_count;
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c)
{
    assert(c == current && c->running && !worker);
    if (fail_stop) return XIAOTAI_FAIL;
    c->running = c->connected = false;
    c->outbox = 0; /* Locked esp-mqtt stops by deleting its whole outbox. */
    ++c->stop_count;
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c)
{
    assert(c == current && !c->running);
    free(c);
    current = NULL;
    return XIAOTAI_OK;
}

esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(esp_mqtt_client_handle_t c)
{
    return c->connected ? MQTT_CLIENT_STATE_CONNECTED : MQTT_CLIENT_STATE_DISCONNECTED;
}
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t c) { return c->outbox; }
int esp_mqtt_client_subscribe_multiple(esp_mqtt_client_handle_t c, const esp_mqtt_topic_t *topics, int count)
{
    assert(c->connected && count == 4);
    assert(strcmp(topics[0].filter, "first") == 0 && topics[0].qos == 0);
    assert(strcmp(topics[3].filter, "fourth") == 0 && topics[3].qos == 2);
    if (force_full) return -2;
    return 55;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic, const char *data,
                            int len, int qos, int retain, bool store)
{
    assert(c->connected && strcmp(topic, "a") == 0 && !retain && store);
    ++c->enqueue_calls;
    if (force_full) return -2;
    assert(len == 0 && data == NULL); /* Vendor len=0 plus non-NULL would call strlen(). */
    c->outbox += 5 + (qos ? 2 : 0);
    return qos ? 55 : 0;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data,
                            int len, int qos, int retain)
{
    assert(c->connected && strcmp(topic, "a") == 0 && len == 3 && !memcmp(data, "x\0z", 3) && !retain);
    ++c->publish_calls;
    if (qos && fail_write_after_enqueue) {
        /* Locked esp-mqtt enqueues QoS1 before trying the transport write. */
        c->outbox += 10;
        return -1;
    }
    return qos ? 55 : 0;
}
bool xiaotai_idf_mqtt_owner_current(esp_mqtt_client_handle_t c)
{
    assert(c == current);
    return worker;
}
esp_err_t xiaotai_idf_mqtt_owner_start(esp_mqtt_client_handle_t c)
{
    esp_err_t rc = esp_mqtt_client_start(c);
    if (rc == XIAOTAI_OK) c->epoch = xt_mqtt_mailbox_start(&c->wake);
    return rc;
}
esp_err_t xiaotai_idf_mqtt_owner_stop(esp_mqtt_client_handle_t c)
{
    esp_err_t rc = esp_mqtt_client_stop(c);
    if (rc == XIAOTAI_OK) xt_mqtt_mailbox_stop(&c->wake);
    return rc;
}
esp_err_t xiaotai_idf_mqtt_owner_destroy(esp_mqtt_client_handle_t c)
{
    return esp_mqtt_client_destroy(c);
}
bool xiaotai_idf_mqtt_owner_wake(esp_mqtt_client_handle_t c)
{
    const unsigned previous = atomic_load(&c->wake.state);
    bool accepted = xt_mqtt_mailbox_request(&c->wake);
    if (accepted && !(previous & 2U)) ++c->wake_calls;
    return accepted;
}

/* Typed boundary doubles: these tests check wrapper parameters/results. The
 * independent native barrier/OOM/ID tests exercise the real implementation. */
esp_err_t xiaotai_idf_mqtt_subscribe_connected(esp_mqtt_client_handle_t c,
                                             const esp_mqtt_topic_t *items, int count, int *id)
{
    *id = 0;
    if (!c->connected) return XIAOTAI_ERR_INVALID_STATE;
    const int native = esp_mqtt_client_subscribe_multiple(c, items, count);
    if (native <= 0) return XIAOTAI_FAIL;
    *id = native;
    return XIAOTAI_OK;
}
esp_err_t xiaotai_idf_mqtt_enqueue_connected(esp_mqtt_client_handle_t c, const char *topic,
                                           const char *body, int size, int qos, size_t bytes, int *id)
{
    *id = 0;
    if (!c->connected) return XIAOTAI_ERR_INVALID_STATE;
    const size_t cap = c->config.outbox.limit;
    if (cap && (bytes > cap || (size_t)c->outbox > cap - bytes)) return XIAOTAI_ERR_NO_MEM;
    int native = esp_mqtt_client_enqueue(c, topic, body, size, qos, 0, true);
    if (native < 0) return native == -2 ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL;
    *id = qos ? native : 0;
    return XIAOTAI_OK;
}
esp_err_t xiaotai_idf_mqtt_publish_now(esp_mqtt_client_handle_t c, const char *topic,
                                     const char *body, int size, int qos, size_t bytes, int *id)
{
    *id = 0;
    if (!c->connected) return XIAOTAI_ERR_INVALID_STATE;
    const size_t cap = c->config.outbox.limit;
    if (qos && cap && (bytes > cap || (size_t)c->outbox > cap - bytes)) return XIAOTAI_ERR_NO_MEM;
    const int before = c->outbox;
    int native = esp_mqtt_client_publish(c, topic, body, size, qos, 0);
    if (native < 0) {
        c->outbox = before; /* The real helper rolls back only its own node. */
        return native == -2 ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL;
    }
    *id = qos ? native : 0;
    return XIAOTAI_OK;
}

static const xiaotai_mqtt_topic_t topics[] = {
    {"first", 0}, {"second", 1}, {"third", 2}, {"fourth", 2},
};
static struct {
    xiaotai_mqtt_t *client;
    int counts[7], acked, topics_checked, errors[5];
    char bytes[4];
    size_t received;
} f;

static void handler(void *ctx, const xiaotai_mqtt_event_t *event)
{
    assert(ctx == &f && worker && event->kind <= XIAOTAI_MQTT_WAKE);
    ++f.counts[event->kind];
    if (event->kind == XIAOTAI_MQTT_CONNECTED) {
        int32_t id = -1;
        assert(xiaotai_mqtt_subscribe(f.client, topics, 4, &id) == XIAOTAI_OK && id == 55);
        assert(xiaotai_mqtt_start(f.client) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (event->kind == XIAOTAI_MQTT_SUBSCRIBED) {
        const uint8_t expected[] = {1, 0x80, 2, 0};
        assert(event->message_id == 55 && event->granted_count == 4 && !memcmp(event->granted, expected, 4));
        ++f.topics_checked;
    }
    if (event->kind == XIAOTAI_MQTT_DATA) {
        assert(event->topic_size == 4 && !memcmp(event->topic, "test", 4));
        assert(event->total == 3 && event->qos == 2 && event->duplicate && event->retained);
        assert(event->message_id == 77 && event->offset == f.received);
        if (event->size) memcpy(f.bytes + f.received, event->data, event->size);
        f.received += event->size;
    }
    if (event->kind == XIAOTAI_MQTT_PUBLISHED) f.acked = event->message_id;
    if (event->kind == XIAOTAI_MQTT_ERROR) f.errors[event->error] = event->detail;
}

static void deliver(esp_mqtt_event_t event, bool native_worker)
{
    worker = native_worker;
    current->handler(current->ctx, "mqtt", event.event_id, &event);
    worker = false;
}

static void online(void)
{
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_BEFORE_CONNECT}, true);
    current->connected = true;
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_CONNECTED}, true);
}

static void native_wake(uint32_t epoch, bool native_worker)
{
    if (native_worker && xt_mqtt_mailbox_take(&current->wake, epoch))
        deliver((esp_mqtt_event_t){.event_id = MQTT_USER_EVENT}, true);
}

static xiaotai_mqtt_config_t cfg(const char *uri)
{
    return (xiaotai_mqtt_config_t){.uri = uri, .client_id = "unit", .username = "tester",
                                  .password = "example", .ctx = &f, .handler = handler};
}

static void test_native(void)
{
    xiaotai_mem_host_clear();
    memset(&f, 0, sizeof(f));
    char pwd[] = "example";
    xiaotai_mqtt_config_t input = cfg("mqtt://host");
    input.password = pwd;
    input.outbox_limit_bytes = 12;
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
    memset(pwd, 0, sizeof(pwd));
    assert(strcmp(current->config.credentials.authentication.password, "example") == 0);
    assert(current->config.credentials.authentication.password != pwd);
    assert(current->config.session.protocol_ver == MQTT_PROTOCOL_V_3_1_1 && !current->config.session.disable_clean_session);
    assert(current->config.session.keepalive == 60 && current->config.network.reconnect_timeout_ms == 3000);
    assert(!current->config.network.disable_auto_reconnect && current->config.task.stack_size == 6144);
    assert(current->config.buffer.size == 1024 && current->config.buffer.out_size == 1024);
    assert(current->config.outbox.limit == 12 && !current->config.broker.verification.crt_bundle_attach);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_ERR_INVALID_STATE);
    int32_t id = 55;
    assert(xiaotai_mqtt_publish(f.client, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_ERR_INVALID_STATE);
    assert(id == 0 && !current->enqueue_calls);
    online();
    force_full = true;
    assert(xiaotai_mqtt_subscribe(f.client, topics, 4, &id) == XIAOTAI_FAIL && !id);
    force_full = false;
    const uint8_t codes[] = {1, 0x80, 2, 0};
    esp_mqtt_event_t ack = {.event_id = MQTT_EVENT_SUBSCRIBED, .msg_id = 55, .data = (char *)codes, .data_len = 4};
    deliver(ack, true);
    deliver(ack, true);
    assert(f.topics_checked == 2);
    ack.data_len = 5;
    deliver(ack, true);
    assert(f.errors[XIAOTAI_MQTT_ERROR_PROTOCOL] == MQTT_EVENT_SUBSCRIBED);
    esp_mqtt_event_t fragment = {.event_id = MQTT_EVENT_DATA, .topic = "test", .topic_len = 4,
        .msg_id = 77, .total_data_len = 3, .qos = 2, .dup = true, .retain = true};
    deliver(fragment, true);
    fragment.topic = NULL;
    fragment.topic_len = 0;
    fragment.data = "X"; fragment.data_len = 1;
    deliver(fragment, true);
    fragment.data = "\0Z"; fragment.data_len = 2; fragment.current_data_offset = 1;
    deliver(fragment, true);
    assert(f.received == 3 && memcmp(f.bytes, "X\0Z", 3) == 0);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK && current->wake_calls == 1);
    native_wake(current->epoch, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 1);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK && current->wake_calls == 2);
    native_wake(current->epoch, true);
    assert(xiaotai_mqtt_publish(f.client, "a", "x\0z", 3, 0, XIAOTAI_MQTT_SEND_NOW, &id) == XIAOTAI_OK && !id);
    assert(current->publish_calls == 1);
    force_full = true;
    assert(xiaotai_mqtt_publish(f.client, "a", "", 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_ERR_NO_MEM);
    force_full = false;
    assert(xiaotai_mqtt_publish(f.client, "a", "", 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_OK && id == 55);
    assert(xiaotai_mqtt_publish(f.client, "a", "", 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_ERR_NO_MEM);
    assert(!id && current->enqueue_calls == 2);
    current->outbox = 0;
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_PUBLISHED, .msg_id = 55}, true);
    assert(f.acked == 55);
    esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT, .esp_transport_sock_errno = ECONNRESET};
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_ERROR, .error_handle = &error}, true);
    error.esp_tls_stack_err = -0x1234;
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_ERROR, .error_handle = &error}, true);
    error = (esp_mqtt_error_codes_t){.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED, .connect_return_code = 5};
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_ERROR, .error_handle = &error}, true);
    assert(f.errors[XIAOTAI_MQTT_ERROR_TRANSPORT] == ECONNRESET &&
           f.errors[XIAOTAI_MQTT_ERROR_TLS] == -0x1234 && f.errors[XIAOTAI_MQTT_ERROR_REFUSED] == 5);
    current->connected = false;
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_DISCONNECTED}, true);
    online();
    const unsigned errors = (unsigned)f.counts[XIAOTAI_MQTT_ERROR];
    /* Foreign failures are admitted/copied by the real native bridge; they
     * never call this wrapper directly. The real-task suite covers that path. */
    fail_stop = true;
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_FAIL && current->running && current->connected);
    fail_stop = false;
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_ERROR, .error_handle = &error}, true);
    assert((unsigned)f.counts[XIAOTAI_MQTT_ERROR] == errors);
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(f.client, &stats);
    assert(stats.connects == 2 && stats.disconnects == 1 && stats.errors == 4 && stats.messages == 1 &&
           stats.fragments == 3 && stats.published == 2 && stats.publish_failures == 3 &&
           stats.outbox_full == 2 && !stats.connected);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == 0);
}

static void test_wake_worker_and_generation(void)
{
    xiaotai_mem_host_clear();
    memset(&f, 0, sizeof(f));
    xiaotai_mqtt_config_t input = cfg("mqtt://host");
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);

    /* The bridge knows its task before the first BEFORE_CONNECT. */
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    native_wake(current->epoch, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 1);
    online();
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 1);

    /* The native API can dispatch on the publishing caller on write failure. */
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    uint32_t previous = current->epoch;
    native_wake(previous, false);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 1);
    native_wake(current->epoch, true);
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_PUBLISHED, .msg_id = 23}, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 2 && f.acked == 23);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    native_wake(current->epoch, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 3);

    /* stop ends the mailbox epoch; a stale consumer cannot take a new wake. */
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    previous = current->epoch;
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    online();
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK && current->epoch != previous);
    native_wake(previous, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 3);
    native_wake(current->epoch, true);
    assert(f.counts[XIAOTAI_MQTT_WAKE] == 4);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == 0);
}

static void test_no_hidden_retry_on_failed_write(void)
{
    xiaotai_mem_host_clear();
    memset(&f, 0, sizeof(f));
    xiaotai_mqtt_config_t input = cfg("mqtt://host");
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    online();

    /* The locked vendor path can fail while retaining data for retransmit. */
    fail_write_after_enqueue = true;
    int32_t id = 99;
    const xiaotai_err_t rc = xiaotai_mqtt_publish(f.client, "a", "x\0z", 3, 1, XIAOTAI_MQTT_SEND_NOW, &id);
    assert(!current->outbox && current->publish_calls == 1 && !current->enqueue_calls);
    assert(rc == XIAOTAI_FAIL && !id && !f.acked);
    fail_write_after_enqueue = false;

    /* Supported admission still uses the native queue and broker message ID. */
    assert(xiaotai_mqtt_publish(f.client, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_OK);
    assert(id == 55 && current->outbox == 7 && !f.acked);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK && current->stop_count == 1 && !current->outbox);
    /* Explicit shutdown abandons pending messages without forging a PUBACK. */
    deliver((esp_mqtt_event_t){.event_id = MQTT_EVENT_PUBLISHED, .msg_id = 55}, true);
    assert(!f.acked && !f.counts[XIAOTAI_MQTT_PUBLISHED]);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    online();
    assert(!current->outbox && !current->enqueue_calls && !f.acked);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == 0);
}

static void test_lifecycle(void)
{
    xiaotai_mem_host_clear();
    memset(&f, 0, sizeof(f));
    xiaotai_mqtt_config_t input = cfg("mqtt://host");
    for (unsigned n = 1; n <= 2; ++n) {
        xiaotai_mem_host_fail_nth(n);
        assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_ERR_NO_MEM);
        assert(!f.client && !current && xiaotai_mem_host_outstanding() == 0);
    }
    fail_init = true;
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_ERR_NO_MEM && !f.client);
    fail_init = false;
    fail_registration = true;
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_ERR_NO_MEM && !f.client && !current);
    fail_registration = false;
    assert(xiaotai_mem_host_outstanding() == 0);

    for (unsigned n = 0; n < 11; ++n) {
        assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
        fail_start = true;
        assert(xiaotai_mqtt_start(f.client) == XIAOTAI_FAIL);
        fail_start = false;
        assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
        online();
        assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK && current->stop_count == 1);
        assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
        online();
        assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK && current->stop_count == 2);
        assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
        assert(!current && xiaotai_mem_host_outstanding() == 0);
    }
    input.uri = "mqtts://verified";
#if CONFIG_MQTT_TRANSPORT_SSL && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_OK);
    assert(current->config.broker.verification.crt_bundle_attach == esp_crt_bundle_attach);
    assert(!current->config.broker.verification.skip_cert_common_name_check);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
#else
    assert(xiaotai_mqtt_create(&input, &f.client) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(!f.client && !current);
#endif
    assert(xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    test_native();
    test_wake_worker_and_generation();
    test_no_hidden_retry_on_failed_write();
    test_lifecycle();
    return 0;
}
