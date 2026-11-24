// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_mqtt_readiness.py
// Replaces legacy: tests/regression/test_p4_mqtt_commands.py
// Replaces legacy: tests/regression/test_c3_mqtt_frames.py
#include "xiaotai/port/mqtt.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "xiaotai_mem_host.h"
#include "mqtt_client.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct esp_mqtt_client {
    esp_mqtt_client_config_t config;
    esp_event_handler_t callback;
    void *arg;
    atomic_bool running, connected;
    int outbox, enqueues, publishes;
};

typedef struct {
    xiaotai_mqtt_event_kind_t kind;
    int32_t id, detail;
    xiaotai_mqtt_error_t error;
    size_t size, offset, total, topic_size, granted_count;
    char topic[8], data[16];
    uint8_t granted[4];
} observation_t;

static struct esp_mqtt_client *fake;
static xiaotai_mqtt_t *port;
static xiaotai_queue_t observed;
static xiaotai_signal_t held, release;
static pthread_t producer;
static pthread_t owner;
static bool have_owner, fail_registration, fail_start, fail_stop, fail_destroy;
static int subscribe_result = 71;
static atomic_bool hold_wake;
static unsigned owner_stack = 6144U;
static unsigned owner_creates;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    assert(!strcmp(cfg->name, "xt_mqtt_events"));
    assert(cfg->stack == XIAOTAI_MEM_INTERNAL && cfg->stack_bytes == owner_stack);
    ++owner_creates;
    return __real_xiaotai_task_create(cfg, task);
}

esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return XIAOTAI_OK; }

static void deliver(esp_mqtt_event_id_t id, esp_mqtt_event_t *event)
{
    event->client = fake;
    fake->callback(fake->arg, "MQTT_EVENTS", id, event);
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(!fake);
    fake = calloc(1, sizeof(*fake));
    assert(fake);
    fake->config = *config;
    atomic_init(&fake->connected, false);
    atomic_init(&fake->running, false);
    return fake;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t id,
                                          esp_event_handler_t callback, void *arg)
{
    assert(client == fake && id == MQTT_EVENT_ANY && callback && arg);
    if (fail_registration) return XIAOTAI_ERR_NO_MEM;
    fake->callback = callback;
    fake->arg = arg;
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    assert(client == fake && !atomic_load(&fake->running));
    if (fail_start) return XIAOTAI_FAIL;
    atomic_store(&fake->running, true);
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    assert(client == fake && atomic_load(&fake->running));
    if (fail_stop) return XIAOTAI_FAIL;
    atomic_store(&fake->running, false);
    atomic_store(&fake->connected, false);
    fake->outbox = 0;
    return XIAOTAI_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    assert(client == fake && !atomic_load(&fake->running));
    if (fail_destroy) return XIAOTAI_FAIL;
    free(fake);
    fake = NULL;
    return XIAOTAI_OK;
}

esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(esp_mqtt_client_handle_t client)
{
    assert(client == fake);
    return atomic_load(&fake->connected) ? MQTT_CLIENT_STATE_CONNECTED : MQTT_CLIENT_STATE_DISCONNECTED;
}

int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client)
{
    assert(client == fake);
    return fake->outbox;
}

int esp_mqtt_client_subscribe_multiple(esp_mqtt_client_handle_t client, const esp_mqtt_topic_t *topics, int count)
{
    assert(client == fake && atomic_load(&fake->connected));
    assert(count == 2 && !strcmp(topics[0].filter, "first") && topics[1].qos == 2);
    if (subscribe_result <= 0) return subscribe_result;
    uint8_t stack_codes[] = {1, 0x80};
    deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = subscribe_result, .data = (char *)stack_codes,
                                                     .data_len = sizeof(stack_codes)});
    memset(stack_codes, 0xff, sizeof(stack_codes));
    return subscribe_result;
}

int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic, const char *data,
                            int len, int qos, int retain, bool store)
{
    assert(client == fake && atomic_load(&fake->connected));
    assert(!strcmp(topic, "a") && store && !retain && qos == 1 && !data && !len);
    ++fake->enqueues;
    fake->outbox += 7;
    return 72;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic, const char *data,
                            int len, int qos, int retain)
{
    assert(client == fake && atomic_load(&fake->connected));
    assert(!strcmp(topic, "a") && !retain && qos == 0 && len == 3 && !memcmp(data, "a\0b", 3));
    ++fake->publishes;
    esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT, .esp_transport_sock_errno = 33};
    deliver(MQTT_EVENT_ERROR, &(esp_mqtt_event_t){.error_handle = &error});
    error.esp_transport_sock_errno = 55;
    return -1;
}

static void handle(void *arg, const xiaotai_mqtt_event_t *event)
{
    assert(arg == &port && !pthread_equal(pthread_self(), producer));
    if (!have_owner) { owner = pthread_self(); have_owner = true; }
    assert(pthread_equal(pthread_self(), owner));
    assert(xiaotai_mqtt_stop(port) == XIAOTAI_ERR_INVALID_STATE);

    observation_t note = {.kind = event->kind, .id = event->message_id, .detail = event->detail,
                          .error = event->error, .size = event->size, .offset = event->offset,
                          .total = event->total, .topic_size = event->topic_size,
                          .granted_count = event->granted_count};
    if (event->topic_size) {
        assert(event->topic_size <= sizeof(note.topic));
        memcpy(note.topic, event->topic, event->topic_size);
    }
    if (event->size) {
        assert(event->size <= sizeof(note.data));
        memcpy(note.data, event->data, event->size);
    }
    if (event->granted_count) memcpy(note.granted, event->granted, event->granted_count);
    if (event->kind == XIAOTAI_MQTT_CONNECTED) {
        const xiaotai_mqtt_topic_t topics[] = {{"first", 1}, {"second", 2}};
        int32_t id;
        assert(xiaotai_mqtt_subscribe(port, topics, 2, &id) == XIAOTAI_OK && id == 71);
    }
    if (event->kind == XIAOTAI_MQTT_WAKE && atomic_exchange(&hold_wake, false)) {
        xiaotai_signal_give(&held);
        assert(xiaotai_signal_take(&release, 1000));
    }
    assert(xiaotai_queue_send(&observed, &note, 0));
}

static observation_t next(xiaotai_mqtt_event_kind_t expected)
{
    observation_t note;
    assert(xiaotai_queue_receive(&observed, &note, 1000));
    assert(note.kind == expected);
    return note;
}

static unsigned bulk_allocations_of(size_t bytes)
{
    xiaotai_mem_host_ledger_entry_t entries[32];
    size_t count = 0;
    assert(xiaotai_mem_host_ledger_snapshot(entries, 32, &count));
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].task && entries[i].kind == XIAOTAI_MEM_BULK && entries[i].bytes == bytes) ++found;
    }
    return found;
}

static void payload_lifetime(void)
{
    const bool demand = XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND;
    atomic_store(&hold_wake, true);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&held, 1000));
    const uint32_t before = xiaotai_mem_host_allocations();
    const int64_t outstanding = xiaotai_mem_host_outstanding();

    char data[] = "value";
    esp_mqtt_event_t packet = {.topic = "abc", .topic_len = 3, .data = data, .data_len = 5,
                               .total_data_len = 5};
    deliver(MQTT_EVENT_DATA, &packet);
    memset(data, 'X', 5);
    assert(xiaotai_mem_host_allocations() == before + demand);
    assert(xiaotai_mem_host_outstanding() == outstanding + demand);
    assert(bulk_allocations_of(5) == (unsigned)demand);
    if (demand) {
        xiaotai_mem_host_fail_nth(1);
        deliver(MQTT_EVENT_DATA, &packet);
        assert(!xiaotai_mem_host_fail_pending());
        xiaotai_mem_failure_t failure;
        xiaotai_mem_failure(&failure);
        assert(failure.failures == 1 && failure.requested_size == 5);
        const uint32_t after_failure = xiaotai_mem_host_allocations();
        deliver(MQTT_EVENT_DATA, &packet);
        assert(xiaotai_mem_host_allocations() == after_failure);
    } else {
        packet.data_len = fake->config.buffer.size + 1;
        deliver(MQTT_EVENT_DATA, &packet);
        assert(xiaotai_mem_host_allocations() == before);
    }
    /* Two tiny control records make progress even with the data path gapped. */
    uint8_t qos[] = {2, 0x80};
    deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = 89, .data = (char *)qos, .data_len = 2});
    memset(qos, 0xff, sizeof(qos));
    deliver(MQTT_EVENT_PUBLISHED, &(esp_mqtt_event_t){.msg_id = 90});
    xiaotai_signal_give(&release);
    next(XIAOTAI_MQTT_WAKE);
    observation_t note = next(XIAOTAI_MQTT_DATA);
    assert(note.size == 5 && !memcmp(note.data, "value", 5));
    note = next(XIAOTAI_MQTT_ERROR);
    assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == (demand ? XIAOTAI_ERR_NO_MEM : MQTT_EVENT_DATA));
    note = next(XIAOTAI_MQTT_SUBSCRIBED);
    assert(note.id == 89 && note.granted_count == 2 && note.granted[0] == 2 && note.granted[1] == 0x80);
    assert(next(XIAOTAI_MQTT_PUBLISHED).id == 90);
    assert(xiaotai_mem_host_outstanding() == outstanding && !bulk_allocations_of(5));

    packet = (esp_mqtt_event_t){.topic = "abc", .topic_len = 3, .data = "end", .data_len = 3,
                                .total_data_len = 3};
    deliver(MQTT_EVENT_DATA, &packet);
    assert(!memcmp(next(XIAOTAI_MQTT_DATA).data, "end", 3));
}

static void fragment_identity(void)
{
    enum { CHANGED_ID, CHANGED_TOTAL, CHANGED_TOPIC, LINK_LOST, CASE_COUNT };
    for (int i = 0; i < CASE_COUNT; ++i) {
        const int id = 10 + i;
        esp_mqtt_event_t first = {.topic = "cmd", .topic_len = 3, .data = "old", .data_len = 3,
                                  .total_data_len = 6, .msg_id = id, .qos = 1};
        esp_mqtt_event_t tail = {.data = "bad", .data_len = 3, .current_data_offset = 3,
                                 .total_data_len = 6, .msg_id = id, .qos = 1};
        deliver(MQTT_EVENT_DATA, &first);
        observation_t note = next(XIAOTAI_MQTT_DATA);
        assert(note.id == id && note.offset == 0 && note.size == 3 && note.total == 6);
        if (i == CHANGED_ID) ++tail.msg_id;
        if (i == CHANGED_TOTAL) ++tail.total_data_len;
        if (i == CHANGED_TOPIC) { tail.topic = "ntf"; tail.topic_len = 3; }
        if (i == LINK_LOST) {
            atomic_store(&fake->connected, false);
            deliver(MQTT_EVENT_DISCONNECTED, &(esp_mqtt_event_t){0});
            next(XIAOTAI_MQTT_DISCONNECTED);
            atomic_store(&fake->connected, true);
            deliver(MQTT_EVENT_CONNECTED, &(esp_mqtt_event_t){0});
            next(XIAOTAI_MQTT_CONNECTED);
            next(XIAOTAI_MQTT_SUBSCRIBED);
        }
        deliver(MQTT_EVENT_DATA, &tail);
        note = next(XIAOTAI_MQTT_ERROR);
        assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == MQTT_EVENT_DATA);

        /* Even a matching late tail can't complete the discarded publication. */
        tail = (esp_mqtt_event_t){.data = "bad", .data_len = 3, .current_data_offset = 3,
                                  .total_data_len = 6, .msg_id = id, .qos = 1};
        deliver(MQTT_EVENT_DATA, &tail);
        note = next(XIAOTAI_MQTT_ERROR);
        assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == MQTT_EVENT_DATA);

        /* A new publication with a reused id accepts a topic-only prefix at offset zero. */
        first = (esp_mqtt_event_t){.topic = "cmd", .topic_len = 3, .total_data_len = 3,
                                   .msg_id = id, .qos = 1};
        deliver(MQTT_EVENT_DATA, &first);
        note = next(XIAOTAI_MQTT_DATA);
        assert(note.id == id && note.offset == 0 && note.size == 0 && note.total == 3);
        tail = (esp_mqtt_event_t){.data = "new", .data_len = 3, .total_data_len = 3, .msg_id = id, .qos = 1};
        deliver(MQTT_EVENT_DATA, &tail);
        note = next(XIAOTAI_MQTT_DATA);
        assert(note.id == id && note.offset == 0 && note.size == 3 && note.total == 3 && note.topic_size == 3);
        assert(!memcmp(note.topic, "cmd", 3) && !memcmp(note.data, "new", 3));
    }
}

int main(void)
{
    xiaotai_mem_host_ledger_start();
    producer = pthread_self();
    assert(xiaotai_queue_init(&observed, 16, sizeof(observation_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&held) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&release) == XIAOTAI_OK);
    const xiaotai_mqtt_config_t config = {.uri = "mqtts://broker.example", .client_id = "client",
                                         .password = "example", .rx_buffer_bytes = 16, .outbox_limit_bytes = 12,
                                         .handler = handle, .ctx = &port};
    fail_registration = true;
    assert(xiaotai_mqtt_create(&config, &port) == XIAOTAI_ERR_NO_MEM && !port && !fake);
    fail_registration = false;
    assert(xiaotai_mqtt_create(&config, &port) == XIAOTAI_OK);
    assert(bulk_allocations_of(8U * config.rx_buffer_bytes) == !XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND);
    assert(fake->config.task.stack_size == 6144);
    assert(fake->config.broker.verification.crt_bundle_attach == esp_crt_bundle_attach);
    assert(!fake->config.broker.verification.skip_cert_common_name_check);
    fail_start = true;
    assert(xiaotai_mqtt_start(port) == XIAOTAI_FAIL);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_ERR_INVALID_STATE);
    fail_start = false;
    assert(xiaotai_mqtt_start(port) == XIAOTAI_OK);
    atomic_store(&fake->connected, true);
    deliver(MQTT_EVENT_CONNECTED, &(esp_mqtt_event_t){0});
    next(XIAOTAI_MQTT_CONNECTED);
    observation_t note = next(XIAOTAI_MQTT_SUBSCRIBED);
    assert(note.id == 71 && note.granted_count == 2 && note.granted[0] == 1 && note.granted[1] == 0x80);
    static const int refused_ids[] = {-1, -2, 0};
    const xiaotai_mqtt_topic_t all_topics[] = {{"first", 1}, {"second", 2}};
    for (size_t i = 0; i < sizeof(refused_ids) / sizeof(refused_ids[0]); ++i) {
        subscribe_result = refused_ids[i];
        int32_t id = 71;
        assert(xiaotai_mqtt_subscribe(port, all_topics, 2, &id) == XIAOTAI_FAIL && !id);
    }
    subscribe_result = 81;
    int32_t retry_id = 0;
    assert(xiaotai_mqtt_subscribe(port, all_topics, 2, &retry_id) == XIAOTAI_OK && retry_id == 81);
    assert(next(XIAOTAI_MQTT_SUBSCRIBED).id == retry_id);
    subscribe_result = 71;

    /* Stock must distinguish invalid return codes/shapes from legal MQTT
     * vectors which a one-topic QoS 1 consumer can still decline. */
    uint8_t ack_codes[] = {1, 1};
    for (int len = -1; len <= 2; ++len) {
        deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = retry_id,
            .data = len == 1 ? NULL : (char *)ack_codes, .data_len = len});
        note = next(len == 2 ? XIAOTAI_MQTT_SUBSCRIBED : XIAOTAI_MQTT_ERROR);
        if (len == 2) assert(note.id == retry_id && note.granted_count == 2);
        else assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == MQTT_EVENT_SUBSCRIBED &&
                    note.id == retry_id);
    }
    static const uint8_t return_codes[] = {0x80, 0xff, 0x02, 0x03};
    for (size_t i = 0; i < sizeof(return_codes); ++i) {
        ack_codes[0] = return_codes[i];
        const bool valid_mqtt = return_codes[i] == 0x80 || return_codes[i] == 2;
        deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = retry_id, .data = (char *)ack_codes, .data_len = 1});
        note = next(valid_mqtt ? XIAOTAI_MQTT_SUBSCRIBED : XIAOTAI_MQTT_ERROR);
        if (valid_mqtt) assert(note.id == retry_id && note.granted_count == 1 && note.granted[0] == return_codes[i]);
        else assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == MQTT_EVENT_SUBSCRIBED &&
                    note.id == retry_id);
    }
    payload_lifetime();

    char topic[] = "abc", payload[] = "one";
    deliver(MQTT_EVENT_DATA, &(esp_mqtt_event_t){.topic = topic, .topic_len = 3, .data = payload,
                    .data_len = 3, .total_data_len = 6, .msg_id = 2, .qos = 1});
    memset(topic, 'x', 3);
    memcpy(payload, "two", 3);
    deliver(MQTT_EVENT_DATA, &(esp_mqtt_event_t){.data = payload, .data_len = 3, .current_data_offset = 3,
                                               .total_data_len = 6, .msg_id = 2, .qos = 1});
    memset(payload, 'y', 3);
    note = next(XIAOTAI_MQTT_DATA);
    assert(note.size == 3 && note.offset == 0 && note.total == 6 && note.topic_size == 3 && note.id == 2);
    assert(!memcmp(note.topic, "abc", 3) && !memcmp(note.data, "one", 3));
    note = next(XIAOTAI_MQTT_DATA);
    assert(note.offset == 3 && !memcmp(note.topic, "abc", 3) && !memcmp(note.data, "two", 3));
    fragment_identity();
    int32_t id = -1;
    assert(xiaotai_mqtt_publish(port, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_OK && id == 72);
    assert(xiaotai_mqtt_publish(port, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_ERR_NO_MEM && !id);
    assert(fake->enqueues == 1);
    deliver(MQTT_EVENT_PUBLISHED, &(esp_mqtt_event_t){.msg_id = 72});
    assert(next(XIAOTAI_MQTT_PUBLISHED).id == 72);
    assert(xiaotai_mqtt_publish(port, "a", "a\0b", 3, 0, XIAOTAI_MQTT_SEND_NOW, &id) == XIAOTAI_FAIL && !id);
    note = next(XIAOTAI_MQTT_ERROR);
    assert(note.error == XIAOTAI_MQTT_ERROR_TRANSPORT && note.detail == 33 && fake->publishes == 1);

    atomic_store(&hold_wake, true);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&held, 1000));
    const uint32_t burst_allocations = xiaotai_mem_host_allocations();
    for (int i = 0; i < 12; ++i) {
        deliver(MQTT_EVENT_DATA, &(esp_mqtt_event_t){.topic = "abc", .topic_len = 3,
                                                     .data = "a", .data_len = 1, .total_data_len = 1});
    }
    assert(xiaotai_mem_host_allocations() == burst_allocations + (XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND ? 6U : 0U));
    deliver(MQTT_EVENT_PUBLISHED, &(esp_mqtt_event_t){.msg_id = 73});
    deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = 74, .data = "\x01", .data_len = 1});
    for (int i = 0; i < 3; ++i) assert(xiaotai_mqtt_wake(port) == XIAOTAI_OK);
    xiaotai_signal_give(&release);
    next(XIAOTAI_MQTT_WAKE);
    for (int i = 0; i < 6; ++i) assert(next(XIAOTAI_MQTT_DATA).data[0] == 'a');
    note = next(XIAOTAI_MQTT_ERROR);
    assert(note.error == XIAOTAI_MQTT_ERROR_PROTOCOL && note.detail == XIAOTAI_ERR_NO_MEM && !note.id);
    assert(next(XIAOTAI_MQTT_PUBLISHED).id == 73);
    note = next(XIAOTAI_MQTT_SUBSCRIBED);
    assert(note.id == 74 && note.granted_count == 1 && note.granted[0] == 1);
    next(XIAOTAI_MQTT_WAKE);

    fail_stop = true;
    assert(xiaotai_mqtt_stop(port) == XIAOTAI_FAIL);
    assert(xiaotai_mqtt_destroy(port) == XIAOTAI_ERR_INVALID_STATE);
    char live_qos = 1;
    deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = 71, .data = &live_qos, .data_len = 1});
    assert(next(XIAOTAI_MQTT_SUBSCRIBED).id == 71);
    fail_stop = false;
    assert(xiaotai_mqtt_stop(port) == XIAOTAI_OK && !fake->outbox);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_ERR_INVALID_STATE);
    assert(!xiaotai_queue_count(&observed));
    have_owner = false;
    assert(xiaotai_mqtt_start(port) == XIAOTAI_OK);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_OK);
    next(XIAOTAI_MQTT_WAKE);
    assert(xiaotai_mqtt_stop(port) == XIAOTAI_OK);
    fail_destroy = true;
    esp_mqtt_client_handle_t stopped = fake;
    assert(xiaotai_mqtt_destroy(port) == XIAOTAI_FAIL && fake == stopped);
    assert(xiaotai_mqtt_wake(port) == XIAOTAI_ERR_INVALID_STATE);
    char stale_qos = 1;
    deliver(MQTT_EVENT_SUBSCRIBED, &(esp_mqtt_event_t){.msg_id = 71, .data = &stale_qos, .data_len = 1});
    assert(!xiaotai_queue_count(&observed));
    fail_destroy = false;
    assert(xiaotai_mqtt_destroy(port) == XIAOTAI_OK && !fake);

    xiaotai_mqtt_config_t separate = config;
    separate.task_stack_bytes = 6144U;
    for (size_t i = 0; i < 2; ++i) {
        const unsigned sizes[] = {3072U, 4096U};
        have_owner = false;
        separate.event_stack_bytes = owner_stack = sizes[i];
        assert(xiaotai_mqtt_create(&separate, &port) == XIAOTAI_OK);
        assert(fake->config.task.stack_size == 6144);
        const unsigned before = owner_creates;
        assert(xiaotai_mqtt_start(port) == XIAOTAI_OK && owner_creates == before + 1U);
        assert(xiaotai_mqtt_wake(port) == XIAOTAI_OK);
        next(XIAOTAI_MQTT_WAKE);
        assert(xiaotai_mqtt_stop(port) == XIAOTAI_OK);
        assert(xiaotai_mqtt_destroy(port) == XIAOTAI_OK && !fake);
    }
    separate.event_stack_bytes = 1U;
    const unsigned before = owner_creates;
    assert(xiaotai_mqtt_create(&separate, &port) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(port) == XIAOTAI_ERR_INVALID_ARG && owner_creates == before);
    assert(!atomic_load(&fake->running));
    assert(xiaotai_mqtt_destroy(port) == XIAOTAI_OK && !fake);
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&held);
    xiaotai_queue_deinit(&observed);
}
