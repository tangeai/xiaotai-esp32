// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include "mqtt_client_priv.h"

#define TAG outbox_tag
#include "native_outbox.c"
#undef TAG

static const char *TAG = "mqtt_client";
static unsigned writes, aborts;
static uint8_t wire[256];
static size_t wire_size;
static unsigned random_value = 35;

int platform_random(int max) { assert(max == UINT16_MAX); return (int)random_value; }
static int64_t platform_tick_get_ms(void) { return 123; }
static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t client) { assert(client); ++aborts; }

static esp_err_t esp_mqtt_write(esp_mqtt_client_handle_t client)
{
    const mqtt_message_t *msg = &client->mqtt_state.connection.outbound_message;
    assert(msg->length <= sizeof(wire));
    memcpy(wire, msg->data, msg->length);
    wire_size = msg->length;
    ++writes;
    return ESP_OK;
}

#include "native_client.inc"

static struct esp_mqtt_client client;
static mqtt_config_storage_t config;
static pthread_mutex_t mutex;

static void seed(uint16_t value)
{
    assert(value);
#if MQTT_MSG_ID_INCREMENTAL
    client.mqtt_state.connection.last_message_id = value - 1U;
#else
    random_value = value;
#endif
}

static void init(void)
{
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&mutex, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    memset(&client, 0, sizeof(client));
    memset(&config, 0, sizeof(config));
    client.api_lock = &mutex;
    client.config = &config;
    client.state = MQTT_STATE_CONNECTED;
    client.outbox = outbox_init();
    assert(client.outbox);
    assert(mqtt_msg_buffer_init(&client.mqtt_state.connection, 128) == ESP_OK);
    client.mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
#if !TEST_STOCK
    client.mqtt_state.connection.message_id_available = xt_mqtt_outbox_available_id;
    client.mqtt_state.connection.message_id_ctx = client.outbox;
#endif
}

static void finish(void)
{
    mqtt_msg_buffer_destroy(&client.mqtt_state.connection);
    outbox_destroy(client.outbox);
    assert(!pthread_mutex_destroy(&mutex));
}

static int publish(void)
{
    return esp_mqtt_client_enqueue(&client, "a", "hello", 5, 1, 0, true);
}

static int subscribe(void)
{
    const esp_mqtt_topic_t topics[] = {{.filter = "b", .qos = 1}, {.filter = "c", .qos = 0}};
    return esp_mqtt_client_subscribe_multiple(&client, topics, 2);
}

static unsigned transactions(int id)
{
    unsigned count = 0;
    outbox_item_t *item;
    STAILQ_FOREACH(item, client.outbox->list, next) if (item->msg_id == id) ++count;
    return count;
}

#if TEST_STOCK
static void collision(void)
{
    init();
    seed(35);
    const int id = publish();
    assert(id == 35);
    seed(35); /* force a real random repeat, or a full incremental wrap */
    assert(subscribe() == id);
    assert(transactions(id) == 2);
    assert(outbox_get(client.outbox, id)->msg_type == MQTT_MSG_TYPE_PUBLISH);
    finish();
    puts("COUNTEREXAMPLE: stock codec reused an active PUBLISH ID for SUBSCRIBE");
}
#else
static void reserved_until_ack(void)
{
    init();
    seed(UINT16_MAX);
    const int pub_id = publish();
    assert(pub_id == UINT16_MAX);
    seed(UINT16_MAX);
    const int sub_id = subscribe();
    assert(sub_id == 1 && transactions(pub_id) == 1 && transactions(sub_id) == 1);
    seed(UINT16_MAX);
    const int unsub_id = esp_mqtt_client_unsubscribe(&client, "b");
    assert(unsub_id == 2 && transactions(unsub_id) == 1);
    seed(UINT16_MAX);
    assert(publish() == 3); /* all outgoing transaction kinds share the reservation */
    assert(outbox_get(client.outbox, pub_id)->pending == QUEUED);
    assert(outbox_get(client.outbox, sub_id)->pending == TRANSMITTED);
    assert(outbox_get(client.outbox, unsub_id)->pending == TRANSMITTED);

    /* Receiving another peer's PUBLISH can legally have our outgoing packet
     * ID. The response encoder must echo it, never consult our allocator. */
    MQTT_API_LOCK((&client));
    mqtt_message_t *ack = mqtt_msg_puback(&client.mqtt_state.connection, (uint16_t)sub_id);
    assert(ack->length == 4 && ack->data[0] == 0x40 && mqtt_get_id(ack->data, ack->length) == sub_id);
    MQTT_API_UNLOCK((&client));
    assert(transactions(sub_id) == 1 && transactions(pub_id) == 1);

    assert(outbox_set_pending(client.outbox, pub_id, TRANSMITTED) == ESP_OK);
    outbox_item_handle_t original = outbox_get(client.outbox, pub_id);
    const uint64_t original_size = outbox_get_size(client.outbox);
    const int sub_bytes = outbox_get(client.outbox, sub_id)->len;
    const int unsub_bytes = outbox_get(client.outbox, unsub_id)->len;
    MQTT_API_LOCK((&client));
    assert(remove_initiator_message(&client, MQTT_MSG_TYPE_SUBSCRIBE, sub_id));
    assert(remove_initiator_message(&client, MQTT_MSG_TYPE_UNSUBSCRIBE, unsub_id));
    MQTT_API_UNLOCK((&client));
    assert(outbox_get(client.outbox, pub_id) == original && transactions(pub_id) == 1);
    assert(!transactions(sub_id) && !transactions(unsub_id));
    assert(outbox_get_size(client.outbox) == original_size - (uint64_t)sub_bytes - (uint64_t)unsub_bytes);
    seed(UINT16_MAX);
    assert(subscribe() == 1); /* only the acknowledged control IDs are free */
    assert(transactions(pub_id) == 1 && transactions(1) == 1);

    MQTT_API_LOCK((&client));
    assert(mqtt_resend_queued(&client, original) == ESP_OK);
    assert(mqtt_get_type(wire) == MQTT_MSG_TYPE_PUBLISH && mqtt_get_dup(wire) == 1 && mqtt_get_qos(wire) == 1);
    assert(mqtt_get_id(wire, wire_size) == pub_id);
    size_t length = wire_size;
    char *payload = mqtt_get_publish_data(wire, &length);
    assert(length == 5 && !memcmp(payload, "hello", 5));
    assert(!remove_initiator_message(&client, MQTT_MSG_TYPE_SUBSCRIBE, pub_id));
    assert(remove_initiator_message(&client, MQTT_MSG_TYPE_PUBLISH, pub_id));
    assert(!remove_initiator_message(&client, MQTT_MSG_TYPE_PUBLISH, pub_id));
    MQTT_API_UNLOCK((&client));
    seed(UINT16_MAX);
    assert(publish() == pub_id && transactions(pub_id) == 1); /* only after real typed removal */
    /* Explicit stop uses the native clear operation, with no retained queue. */
    outbox_delete_all_items(client.outbox);
    assert(!outbox_get_size(client.outbox) && !transactions(pub_id));
    seed(UINT16_MAX);
    assert(publish() == pub_id && transactions(pub_id) == 1);
    assert(!aborts);
    finish();
}

static void saturated(void)
{
    init();
    uint8_t message[] = {0x32, 5, 0, 1, 'a', 0, 0};
    outbox_item_handle_t gap = NULL;
    for (uint32_t id = 1; id <= UINT16_MAX; ++id) {
        message[5] = id >> 8;
        message[6] = id & 255;
        outbox_message_t entry = {.data = message, .len = sizeof(message), .msg_id = (int)id,
            .msg_type = MQTT_MSG_TYPE_PUBLISH, .msg_qos = 1};
        outbox_item_handle_t item = outbox_enqueue(client.outbox, &entry, 123);
        assert(item);
        if (id == 30001) gap = item;
    }
    const uint64_t full_size = outbox_get_size(client.outbox);
    const unsigned before = writes;
    seed(10000);
    assert(publish() == -1 && subscribe() == -1 && esp_mqtt_client_unsubscribe(&client, "a") == -1);
    assert(client.mqtt_state.connection.outbound_message.length == 0 && writes == before);
    assert(outbox_get_size(client.outbox) == full_size);
    assert(outbox_delete_item(client.outbox, gap) == ESP_OK);
    assert(xt_mqtt_outbox_available_id(client.outbox, 10000) == 30001);
    seed(10000);
    assert(publish() == 30001 && transactions(30001) == 1);
    assert(!xt_mqtt_outbox_available_id(client.outbox, 10000));
    finish();
}

static void tiny_buffer_and_direction(void)
{
    init();
    mqtt_connection_t *connection = &client.mqtt_state.connection;
    const size_t capacity = connection->buffer_length;
    connection->buffer_length = 8;
    seed(17);
    assert(publish() == -1 && outbox_get_size(client.outbox) == 0);
    connection->buffer_length = capacity;
    seed(17);
    assert(publish() == 17);
    const uint64_t size = outbox_get_size(client.outbox);
    seed(17);
    assert(esp_mqtt_client_enqueue(&client, "a", "hello", 5, 0, 0, true) == 0);
    assert(outbox_get_size(client.outbox) > size && transactions(17) == 1 && transactions(0) == 1);
    assert(!xt_mqtt_outbox_available_id(NULL, 17) && !xt_mqtt_outbox_available_id(client.outbox, 0));
    finish();
}

static pthread_barrier_t admission_barrier;
static void *competing_publisher(void *unused)
{
    (void)unused;
    const int result = pthread_barrier_wait(&admission_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < 64; ++i) assert(publish() > 0);
    return NULL;
}

static void concurrent_native_admission(void)
{
    init();
    seed(31);
    pthread_t publishers[8];
    assert(!pthread_barrier_init(&admission_barrier, NULL, 8));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&publishers[i], NULL, competing_publisher, NULL));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(publishers[i], NULL));
    assert(!pthread_barrier_destroy(&admission_barrier));
    bool occupied[UINT16_MAX + 1U] = {0};
    unsigned count = 0;
    outbox_item_t *item;
    STAILQ_FOREACH(item, client.outbox->list, next) {
        assert(item->msg_id > 0 && item->msg_id <= UINT16_MAX && !occupied[item->msg_id]);
        occupied[item->msg_id] = true;
        ++count;
    }
    assert(count == 8 * 64);
    finish();
}
#endif

int main(void)
{
#if TEST_STOCK
    collision();
#else
    reserved_until_ack();
    saturated();
    tiny_buffer_and_direction();
    concurrent_native_admission();
    puts("PASS: native packet-ID reservation, stop clears queue, precise ACK, DUP and finite exhaustion");
#endif
    return 0;
}
