// SPDX-License-Identifier: MIT
#include "mqtt_client_priv.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t api;
static _Thread_local unsigned short_depth;
static atomic_int fail_allocation = -1;
static atomic_bool fail_write;
static const char *TAG = "mqtt_client";
static mqtt_config_storage_t config;
static struct esp_mqtt_client client;
static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_changed = PTHREAD_COND_INITIALIZER;
static bool blocked_write, blocked_close, in_write, release_write;
static atomic_uint writes;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
size_t host_outbox_bookkeeping(void);
static bool allocation_fails(void)
{
    int n = atomic_load(&fail_allocation);
    if (n < 0) return false;
    return atomic_fetch_sub(&fail_allocation, 1) == 0;
}
void *__wrap_malloc(size_t size) { return allocation_fails() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t count, size_t size) { return allocation_fails() ? NULL : __real_calloc(count, size); }

void host_log(void) { assert(!short_depth); }
int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, int ticks)
{
    int rc = ticks ? pthread_mutex_lock(lock) : pthread_mutex_trylock(lock);
    assert(!rc || (!ticks && rc == EBUSY));
    if (!rc && lock != &api) ++short_depth;
    return rc ? 0 : pdTRUE;
}
void xSemaphoreGiveRecursive(SemaphoreHandle_t lock)
{
    if (lock != &api) { assert(short_depth); --short_depth; }
    assert(!pthread_mutex_unlock(lock));
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    pthread_mutex_t *lock = calloc(1, sizeof(*lock));
    if (!lock) return NULL;
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(lock, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    return lock;
}
void vSemaphoreDelete(SemaphoreHandle_t lock) { assert(!pthread_mutex_destroy(lock)); free(lock); }

static int64_t platform_tick_get_ms(void) { return 123; }
static void host_network_barrier(void)
{
    assert(!pthread_mutex_lock(&barrier_lock));
    in_write = true;
    assert(!pthread_cond_broadcast(&barrier_changed));
    while (!release_write) assert(!pthread_cond_wait(&barrier_changed, &barrier_lock));
    assert(!pthread_mutex_unlock(&barrier_lock));
}
static void esp_transport_close(esp_transport_handle_t transport)
{
    assert(transport && !short_depth);
    if (blocked_close) host_network_barrier();
}
static int esp_transport_write(esp_transport_handle_t transport, const char *data, int size, int timeout)
{
    assert(transport && data && size > 0 && timeout >= 0 && !short_depth);
    atomic_fetch_add(&writes, 1);
    uint8_t before[256];
    assert((size_t)size <= sizeof(before));
    memcpy(before, data, (size_t)size);
    if (blocked_write) {
        host_network_barrier();
    }
    assert(!memcmp(before, data, (size_t)size)); /* QUEUED cannot touch native TX */
    return atomic_load(&fail_write) ? -1 : size;
}
static void esp_mqtt_client_dispatch_transport_error(esp_mqtt_client_handle_t c) { assert(c && !short_depth); }
static esp_err_t esp_mqtt_dispatch_event_with_msgid(esp_mqtt_client_handle_t c)
{ assert(c && !short_depth); return ESP_OK; }

/* Pinned native functions, not mock enqueue/encoder/outbox implementations. */
#include "mqtt_queue_native.inc"

static esp_err_t enqueue(size_t size, int qos, int *id)
{
    static const char payload[256] = {1, 0, 3};
    size_t wire;
    assert(size <= sizeof(payload) && xt_mqtt_queue_wire_size("a", (int)size, qos, &wire));
    return xiaotai_idf_mqtt_enqueue_connected(&client, "a", size ? payload : NULL, (int)size, qos, wire, id);
}

static void reset(void)
{
    assert(!short_depth);
    outbox_delete_all_items(client.outbox);
    config.outbox_limit = 0;
    atomic_store(&fail_allocation, -1);
    atomic_store(&fail_write, false);
    xt_mqtt_queue_state(&client, MQTT_STATE_CONNECTED);
}

static void *subscribe_thread(void *ctx)
{
    int *id = ctx;
    esp_mqtt_topic_t topic = {.filter = "x", .qos = 1};
    *id = esp_mqtt_client_subscribe_multiple(&client, &topic, 1);
    return NULL;
}

static void network_barrier(void)
{
    reset();
    pthread_t subscriber;
    int sub_id = 0;
    assert(!pthread_mutex_lock(&barrier_lock));
    blocked_write = true; in_write = release_write = false;
    assert(!pthread_mutex_unlock(&barrier_lock));
    assert(!pthread_create(&subscriber, NULL, subscribe_thread, &sub_id));
    assert(!pthread_mutex_lock(&barrier_lock));
    while (!in_write) assert(!pthread_cond_wait(&barrier_changed, &barrier_lock));
    assert(!pthread_mutex_unlock(&barrier_lock));
    /* The actual synchronous native SUBSCRIBE holds api_lock in socket write.
     * QUEUED must already succeed, not TIMEOUT/NO_MEM/FAIL, before release. */
    int queued_id = -1;
    assert(enqueue(125, 1, &queued_id) == ESP_OK && queued_id > 0);
    assert(outbox_get(client.outbox, queued_id));
    assert(outbox_get_size(client.outbox) == 8 + 133);
    assert(!pthread_mutex_lock(&barrier_lock));
    assert(!release_write);
    release_write = true;
    assert(!pthread_cond_broadcast(&barrier_changed));
    assert(!pthread_mutex_unlock(&barrier_lock));
    assert(!pthread_join(subscriber, NULL));
    assert(sub_id > 0 && sub_id != queued_id && outbox_get(client.outbox, sub_id));
    blocked_write = false;
    puts("PASS network barrier: real native write held api_lock; queued accepted before release");
}

static void *abort_thread(void *ctx)
{
    esp_mqtt_abort_connection(ctx);
    return NULL;
}

static void close_barrier(void)
{
    reset();
    int id;
    assert(enqueue(0, 1, &id) == ESP_OK && id > 0);
    blocked_close = true; in_write = release_write = false;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, abort_thread, &client));
    assert(!pthread_mutex_lock(&barrier_lock));
    while (!in_write) assert(!pthread_cond_wait(&barrier_changed, &barrier_lock));
    assert(!pthread_mutex_unlock(&barrier_lock));
    int rejected = -1;
    assert(enqueue(0, 1, &rejected) == ESP_ERR_INVALID_STATE && !rejected);
    assert(outbox_get_size(client.outbox) == 7 && outbox_get(client.outbox, id));
    assert(!pthread_mutex_lock(&barrier_lock));
    release_write = true;
    assert(!pthread_cond_broadcast(&barrier_changed));
    assert(!pthread_mutex_unlock(&barrier_lock));
    assert(!pthread_join(thread, NULL));
    blocked_close = false;
    puts("PASS close barrier: native invalidation precedes blocking transport close");
}

typedef struct { pthread_barrier_t *barrier; esp_err_t result; int id; } contender_t;
static void *contend(void *ctx)
{
    contender_t *value = ctx;
    int rc = pthread_barrier_wait(value->barrier);
    assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    value->result = enqueue(125, 1, &value->id);
    return NULL;
}

static void quota_and_state(void)
{
    reset();
    config.outbox_limit = 133;
    pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 3));
    contender_t values[2] = {{.barrier = &barrier}, {.barrier = &barrier}};
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, contend, &values[i]));
    int rc = pthread_barrier_wait(&barrier);
    assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    unsigned accepted = 0, full = 0;
    for (unsigned i = 0; i < 2; ++i) {
        assert(!pthread_join(threads[i], NULL));
        accepted += values[i].result == ESP_OK && values[i].id > 0;
        full += values[i].result == ESP_ERR_NO_MEM && !values[i].id;
    }
    assert(accepted == 1 && full == 1 && outbox_get_size(client.outbox) == 133);
    assert(!pthread_barrier_destroy(&barrier));
    esp_mqtt_abort_connection(&client);
    int id = -1;
    assert(enqueue(0, 1, &id) == ESP_ERR_INVALID_STATE && !id);
    assert(outbox_get_size(client.outbox) == 133); /* accepted before loss survives reconnect */
    outbox_delete_all_items(client.outbox); /* explicit stop discards after the same fence */
    assert(enqueue(0, 1, &id) == ESP_ERR_INVALID_STATE && !outbox_get_size(client.outbox));
    reset();
    config.outbox_limit = 130;
    assert(enqueue(125, 0, &id) == ESP_ERR_NO_MEM && !id);
    config.outbox_limit = 131;
    assert(enqueue(125, 0, &id) == ESP_OK && !id && outbox_get_size(client.outbox) == 131);
    outbox_item_handle_t loan = outbox_dequeue(client.outbox, QUEUED, NULL);
    config.outbox_limit = 138;
    assert(enqueue(0, 1, &id) == ESP_OK && id > 0 && loan);
    size_t length; uint16_t packet; int type, qos;
    uint8_t *data = outbox_item_get_data(loan, &length, &packet, &type, &qos);
    assert(length == 131 && !packet && !qos && type == MQTT_MSG_TYPE_PUBLISH);
    assert(data[0] == 0x30 && data[1] == 0x80 && data[2] == 1 && data[6] == 1 && data[7] == 0);
    assert(outbox_delete_item(client.outbox, loan) == ESP_OK && outbox_get_size(client.outbox) == 7);
    puts("PASS quota/state: exact full wire charge, two producers, native invalidation and loan survival");
}

static void ids_oom_and_rollback(void)
{
    reset();
    client.mqtt_state.connection.last_message_id = UINT16_MAX - 1;
    int first, second, third;
    assert(enqueue(0, 1, &first) == ESP_OK && first == UINT16_MAX);
    esp_mqtt_topic_t topic = {.filter = "x", .qos = 1};
    second = esp_mqtt_client_subscribe_multiple(&client, &topic, 1);
    assert(second == 1 && outbox_get_size(client.outbox) == 15);
    client.mqtt_state.connection.last_message_id = UINT16_MAX - 1;
    assert(enqueue(0, 1, &third) == ESP_OK && third == 2);
    const size_t kept = outbox_get_size(client.outbox);
    uint16_t cursor = client.mqtt_state.connection.last_message_id;
    for (int at = 0; at < 2; ++at) {
        atomic_store(&fail_allocation, at);
        int id = -1;
        assert(enqueue(0, 1, &id) == ESP_ERR_NO_MEM && !id);
        atomic_store(&fail_allocation, -1);
        assert(outbox_get_size(client.outbox) == kept && client.mqtt_state.connection.last_message_id == cursor);
    }
    atomic_store(&fail_write, true);
    assert(esp_mqtt_client_subscribe_multiple(&client, &topic, 1) < 0);
    assert(outbox_get_size(client.outbox) == kept && outbox_get(client.outbox, first) && outbox_get(client.outbox, second));
    /* Real synchronous QoS1 write fails; its node alone must be rolled back. */
    assert(esp_mqtt_client_publish(&client, "a", "z", 1, 1, 0) < 0);
    assert(outbox_get_size(client.outbox) == kept && outbox_get(client.outbox, third));
    uint32_t failures[4];
    xt_mqtt_outbox_failures(client.outbox, failures);
    assert(failures[XT_MQTT_QUEUE_ALLOC] >= 2 && failures[XT_MQTT_QUEUE_QUOTA] >= 2);
    puts("PASS IDs/OOM: native wrap/collision, both allocation stages, own-node subscribe/publish rollback");
}

int main(void)
{
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&api, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    client.api_lock = &api;
    client.transport = (esp_transport_handle_t)&client;
    client.config = &config;
    client.outbox = outbox_init();
    assert(client.outbox && mqtt_msg_buffer_init(&client.mqtt_state.connection, 64) == ESP_OK);
    client.mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    client.mqtt_state.connection.message_id_available = xt_mqtt_outbox_available_id;
    client.mqtt_state.connection.message_id_ctx = client.outbox;
    network_barrier();
    close_barrier();
    quota_and_state();
    ids_oom_and_rollback();
    printf("host bookkeeping=%zu, mutex=%zu, local codec=%zu, ID scan stack=32 (not target ABI)\n",
           host_outbox_bookkeeping(), sizeof(pthread_mutex_t), sizeof(mqtt_connection_t));
    mqtt_msg_buffer_destroy(&client.mqtt_state.connection);
    outbox_destroy(client.outbox);
    assert(!pthread_mutex_destroy(&api));
    assert(!pthread_cond_destroy(&barrier_changed) && !pthread_mutex_destroy(&barrier_lock));
    puts("PASS native queued proof (ASan/UBSan/LSan; no socket/network/ESP build)");
    return 0;
}
