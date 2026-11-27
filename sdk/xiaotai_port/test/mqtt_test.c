// SPDX-License-Identifier: MIT
#include "mqtt_config.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_mqtt_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define LEN(a) (sizeof(a) / sizeof((a)[0]))
#define FIXTURE_EVENTS 48

typedef struct {
    xiaotai_mqtt_event_t event;
    char topic[XIAOTAI_MQTT_TOPIC_MAX + 1];
    uint8_t codes[XIAOTAI_MQTT_SUBSCRIBE_MAX];
    uint8_t data[64];
} captured_event_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t worker;
    bool seen_worker, hold_next, holding, release;
    bool reenter;
    xiaotai_mqtt_t *client;
    captured_event_t events[FIXTURE_EVENTS];
    size_t n;
} fixture_t;

static void setup(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    assert(pthread_mutex_init(&f->lock, NULL) == 0);
    assert(pthread_cond_init(&f->changed, NULL) == 0);
}

static void end(fixture_t *f)
{
    assert(pthread_cond_destroy(&f->changed) == 0);
    assert(pthread_mutex_destroy(&f->lock) == 0);
}

static void handler(void *ctx, const xiaotai_mqtt_event_t *event)
{
    fixture_t *f = ctx;
    pthread_mutex_lock(&f->lock);
    if (f->seen_worker) assert(pthread_equal(f->worker, pthread_self()));
    f->worker = pthread_self();
    f->seen_worker = true;
    assert(f->n < FIXTURE_EVENTS);
    captured_event_t *capture = &f->events[f->n++];
    capture->event = *event;
    if (event->granted) {
        assert(event->granted_count <= sizeof(capture->codes));
        memcpy(capture->codes, event->granted, event->granted_count);
        capture->event.granted = capture->codes;
    }
    if (event->topic) {
        assert(event->topic_size < sizeof(capture->topic));
        memcpy(capture->topic, event->topic, event->topic_size);
        capture->topic[event->topic_size] = 0;
        capture->event.topic = capture->topic;
    }
    if (event->size) {
        assert(event->data && event->size <= sizeof(capture->data));
        memcpy(capture->data, event->data, event->size);
        capture->event.data = capture->data;
    }
    if (f->reenter && event->kind == XIAOTAI_MQTT_CONNECTED) {
        f->reenter = false;
        const xiaotai_mqtt_topic_t topics[] = {{.topic = "from/handler", .qos = 1}};
        int32_t id;
        assert(xiaotai_mqtt_subscribe(f->client, topics, LEN(topics), &id) == XIAOTAI_OK && id > 0);
        assert(xiaotai_mqtt_publish(f->client, "handler/reply", "x", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_OK);
        assert(xiaotai_mqtt_stop(f->client) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_mqtt_destroy(f->client) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (f->hold_next) {
        f->hold_next = false;
        f->holding = true;
        pthread_cond_broadcast(&f->changed);
        while (!f->release) assert(pthread_cond_wait(&f->changed, &f->lock) == 0);
        f->release = f->holding = false;
    }
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
}

static xiaotai_mqtt_config_t config(fixture_t *f)
{
    /* No real network is used. The long reconnect delay makes time advancement
     * explicit and avoids scheduler-dependent reconnect races in the fixtures. */
    return (xiaotai_mqtt_config_t){.uri = "mqtt://broker.example:1883", .client_id = "fixture-client",
        .username = "fixture-user", .password = "fixture-password", .handler = handler, .ctx = f,
        .rx_buffer_bytes = 4, .tx_buffer_bytes = 3, .reconnect_ms = 60000};
}

static void flush(xiaotai_mqtt_t *client) { assert(xiaotai_mqtt_host_flush(client, 1000) == XIAOTAI_OK); }

static void release(fixture_t *f)
{
    pthread_mutex_lock(&f->lock);
    f->release = true;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
}

static void await_hold(fixture_t *f)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&f->lock);
    while (!f->holding) assert(pthread_cond_timedwait(&f->changed, &f->lock, &deadline) == 0);
    pthread_mutex_unlock(&f->lock);
}

static void *wipe_address;
static size_t wipe_size;
static bool wiped;
void __real_xiaotai_mem_free(void *ptr);
void __wrap_xiaotai_mem_free(void *ptr)
{
    if (ptr && ptr == wipe_address) {
        const uint8_t *bytes = ptr;
        for (size_t i = 0; i < wipe_size; ++i) assert(bytes[i] == 0);
        wipe_address = NULL;
        wiped = true;
    }
    __real_xiaotai_mem_free(ptr);
}

static void configuration(void)
{
    fixture_t f;
    setup(&f);
    xiaotai_mqtt_config_t original = config(&f);
    xiaotai_mqtt_t *client = (void *)1;
    assert(xiaotai_mqtt_create(NULL, &client) == XIAOTAI_ERR_INVALID_ARG && !client);
    static const char *const bad[] = {"", "http://host", "mqtt://", "mqtt://host:", "mqtt://host:0",
        "mqtt://host:65536", "mqtt://user:secret@host", "mqtt://host/path", "mqtt://a b",
        "mqtt://host?x", "mqtt://[::1]foo", "mqtt://:123"};
    for (size_t i = 0; i < LEN(bad); ++i) {
        original.uri = bad[i];
        assert(xiaotai_mqtt_create(&original, &client) == XIAOTAI_ERR_INVALID_ARG && !client);
    }
    mqtt_port_config_t copy;
    char uri[] = "mqtts://[::1]:8883";
    char id[] = "client";
    char user[] = "user";
    char secret[] = "password";
    original = (xiaotai_mqtt_config_t){
        .uri = uri, .client_id = id, .username = user, .password = secret, .handler = handler, .ctx = &f,
    };
    assert(mqtt_port_config_copy(&original, false, &copy) == XIAOTAI_ERR_NOT_SUPPORTED && !copy.strings);
    assert(mqtt_port_config_copy(&original, true, &copy) == XIAOTAI_OK);
    memset(uri, 0, sizeof(uri));
    memset(id, 0, sizeof(id));
    memset(user, 0, sizeof(user));
    memset(secret, 0, sizeof(secret));
    assert(copy.tls && !strcmp(copy.value.uri, "mqtts://[::1]:8883") &&
           !strcmp(copy.value.client_id, "client") && !strcmp(copy.value.username, "user") &&
           !strcmp(copy.value.password, "password"));
    assert(copy.value.keepalive_s == 60 && copy.value.reconnect_ms == 3000 &&
           copy.value.rx_buffer_bytes && copy.value.tx_buffer_bytes &&
           copy.value.task_stack_bytes && !copy.value.outbox_limit_bytes && MQTT_PORT_VERSION == 4U);
    wipe_size = copy.strings_bytes;
    wipe_address = copy.strings;
    mqtt_port_config_clear(&copy);
    assert(wiped && !copy.strings && !copy.value.password);
    xiaotai_mqtt_config_t valid = config(&f);
    valid.client_id = "";
    assert(xiaotai_mqtt_create(&valid, &client) == XIAOTAI_ERR_INVALID_ARG && !client);
    valid.client_id = "id";
    valid.handler = NULL;
    assert(xiaotai_mqtt_create(&valid, &client) == XIAOTAI_ERR_INVALID_ARG && !client);
    assert(xiaotai_mqtt_destroy(NULL) == XIAOTAI_OK);
    size_t bytes = 0;
    char payload[125] = {0};
    assert(mqtt_port_publish_size("p", payload, 124, 0, XIAOTAI_MQTT_SEND_QUEUED, &bytes) && bytes == 129);
    assert(mqtt_port_publish_size("p", payload, 125, 0, XIAOTAI_MQTT_SEND_QUEUED, &bytes) && bytes == 131);
    assert(!mqtt_port_publish_size("p", payload, SIZE_MAX, 0, XIAOTAI_MQTT_SEND_QUEUED, &bytes));
    assert(!mqtt_port_publish_size("p/+", payload, 125, 0, XIAOTAI_MQTT_SEND_QUEUED, &bytes));
    end(&f);
}

static void subscribe_and_reconnect(void)
{
    fixture_t f;
    setup(&f);
    const xiaotai_mqtt_config_t opts = config(&f);
    assert(xiaotai_mqtt_create(&opts, &f.client) == XIAOTAI_OK);
    const xiaotai_mqtt_topic_t topics[] = {
        {.topic = "command", .qos = 2}, {.topic = "notification", .qos = 1},
        {.topic = "reply", .qos = 0}, {.topic = "status", .qos = 1},
    };
    int32_t id;
    assert(xiaotai_mqtt_subscribe(f.client, topics, LEN(topics), &id) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mqtt_publish(f.client, "reply", "x", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_mqtt_host_connack(f.client, XIAOTAI_MQTT_ERROR_REFUSED, 5);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_ERR_INVALID_STATE);
    flush(f.client);
    assert(f.n == 1 && f.events[0].event.kind == XIAOTAI_MQTT_ERROR &&
           f.events[0].event.error == XIAOTAI_MQTT_ERROR_REFUSED && f.events[0].event.detail == 5);
    assert(!pthread_equal(f.worker, pthread_self()));
    xiaotai_mqtt_host_connack(f.client, XIAOTAI_MQTT_ERROR_NONE, 0);
    flush(f.client);
    assert(f.n == 1);
    xiaotai_mqtt_host_advance_ms(f.client, opts.reconnect_ms);
    flush(f.client);
    assert(f.n == 2 && f.events[1].event.kind == XIAOTAI_MQTT_CONNECTED);
    uint8_t grants[] = {2, 1, XIAOTAI_MQTT_REFUSED, 0};
    assert(xiaotai_mqtt_host_suback(f.client, false, grants, LEN(grants)) == XIAOTAI_OK);
    xiaotai_mqtt_host_next_id(f.client, 112);
    assert(xiaotai_mqtt_subscribe(f.client, topics, LEN(topics), &id) == XIAOTAI_OK && id == 112);
    assert(xiaotai_mqtt_host_subscriptions(f.client) == 3);
    flush(f.client);
    assert(f.n == 2);
    xiaotai_mqtt_host_next_id(f.client, 112);
    assert(xiaotai_mqtt_subscribe(f.client, topics, LEN(topics), &id) == XIAOTAI_OK && id != 112);
    assert(xiaotai_mqtt_host_suback_reply(f.client, 112) == XIAOTAI_OK);
    flush(f.client);
    assert(f.n == 3 && f.events[2].event.kind == XIAOTAI_MQTT_SUBSCRIBED &&
           f.events[2].event.message_id == 112 && f.events[2].event.granted_count == LEN(grants) &&
           !memcmp(grants, f.events[2].codes, sizeof(grants)));
    assert(xiaotai_mqtt_host_suback(f.client, true, grants, LEN(grants)) == XIAOTAI_OK);
    xiaotai_mqtt_host_next_id(f.client, 112);
    assert(xiaotai_mqtt_subscribe(f.client, topics, LEN(topics), &id) == XIAOTAI_OK && id == 112);
    flush(f.client);
    assert(f.n == 4 && f.events[3].event.kind == XIAOTAI_MQTT_SUBSCRIBED && f.events[3].event.message_id == 112);
    assert(xiaotai_mqtt_host_disconnect(f.client) == XIAOTAI_OK);
    flush(f.client);
    assert(f.n == 5 && f.events[4].event.kind == XIAOTAI_MQTT_DISCONNECTED);
    assert(xiaotai_mqtt_host_subscriptions(f.client) == 0);
    xiaotai_mqtt_host_advance_ms(f.client, opts.reconnect_ms);
    flush(f.client);
    assert(f.n == 6 && f.events[5].event.kind == XIAOTAI_MQTT_CONNECTED);
    assert(xiaotai_mqtt_host_subscriptions(f.client) == 0);
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(f.client, &stats);
    assert(stats.connected && stats.connects == 2 && stats.errors == 1 && stats.disconnects == 1);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    end(&f);
}

static void messages_and_wake(void)
{
    fixture_t f;
    setup(&f);
    const xiaotai_mqtt_config_t opts = config(&f);
    f.hold_next = f.reenter = true;
    assert(xiaotai_mqtt_create(&opts, &f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    await_hold(&f);
    char topic[] = "command";
    char data[] = "abcdefgh";
    const size_t slices[] = {0, 4, 4};
    assert(xiaotai_mqtt_host_message(f.client, topic, data, strlen(data), 1, 19, true, true,
                                    slices, LEN(slices)) == XIAOTAI_OK);
    memset(topic, 0, sizeof(topic));
    memset(data, 0, sizeof(data));
    assert(xiaotai_mqtt_host_message(f.client, "notification", "12345", 5, 2, 19, false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_message(f.client, "empty", NULL, 0, 0, 0, false, false, NULL, 0) == XIAOTAI_OK);
    for (unsigned i = 0; i < 10; ++i) assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    release(&f);
    flush(f.client);
    assert(f.n == 10 && f.events[0].event.kind == XIAOTAI_MQTT_CONNECTED &&
           f.events[1].event.kind == XIAOTAI_MQTT_SUBSCRIBED);
    for (size_t i = 0; i < LEN(slices); ++i) {
        const xiaotai_mqtt_event_t *e = &f.events[2 + i].event;
        assert(e->kind == XIAOTAI_MQTT_DATA && e->message_id == 19 && e->topic_size == 7 &&
               !strcmp(e->topic, "command") && e->retained && e->duplicate &&
               e->qos == 1 && e->total == 8 && e->size == slices[i] && e->offset == (i == 2 ? 4 : 0));
        if (e->size) assert(!memcmp(e->data, i == 1 ? "abcd" : "efgh", 4));
        else assert(!e->data);
    }
    assert(f.events[5].event.kind == XIAOTAI_MQTT_DATA && f.events[5].event.message_id == 19 &&
           f.events[5].event.qos == 2 && f.events[5].event.size == 4 &&
           !strcmp(f.events[5].topic, "notification") && !memcmp(f.events[5].data, "1234", 4));
    assert(f.events[6].event.kind == XIAOTAI_MQTT_DATA && f.events[6].event.message_id == 19 &&
           f.events[6].event.size == 1 && f.events[6].event.offset == 4 &&
           !strcmp(f.events[6].topic, "notification") && f.events[6].data[0] == '5');
    assert(f.events[7].event.kind == XIAOTAI_MQTT_DATA && !f.events[7].event.total && !f.events[7].event.size);
    assert(f.events[8].event.kind == XIAOTAI_MQTT_WAKE && f.events[9].event.kind == XIAOTAI_MQTT_PUBLISHED);
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(f.client, &stats);
    assert(stats.messages == 3 && stats.fragments == 6 && stats.published == 1);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mqtt_host_message(f.client, "empty", NULL, 0, 0, 0, false, false, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(f.n == 10);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    end(&f);
}

static void publication_lifecycle(void)
{
    fixture_t f;
    setup(&f);
    xiaotai_mqtt_config_t opts = config(&f);
    opts.outbox_limit_bytes = 8;  /* exactly a PUBLISH to 'p' with a 1-byte QoS 1 payload */
    assert(xiaotai_mqtt_create(&opts, &f.client) == XIAOTAI_OK);
    xiaotai_mqtt_host_puback(f.client, false);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    flush(f.client);
    int32_t retained;
    char value = 'Q';
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &retained) == XIAOTAI_OK);
    value = 'X';
    flush(f.client);
    assert(f.n == 1);
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 1, XIAOTAI_MQTT_SEND_NOW, NULL) == XIAOTAI_ERR_NO_MEM);
    char topic[8], data[8];
    xiaotai_mqtt_host_publication_t record;
    assert(xiaotai_mqtt_host_take(f.client, &record, NULL, 0, NULL, 0) == XIAOTAI_ERR_INVALID_SIZE && record.size == 1);
    assert(xiaotai_mqtt_host_take(f.client, &record, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_OK &&
           record.message_id == retained && !record.duplicate && data[0] == 'Q' && !strcmp(topic, "p"));
    /* An automatic reconnect still retries the outstanding packet. */
    assert(xiaotai_mqtt_host_disconnect(f.client) == XIAOTAI_OK);
    flush(f.client);
    xiaotai_mqtt_host_puback(f.client, true);
    xiaotai_mqtt_host_advance_ms(f.client, opts.reconnect_ms);
    flush(f.client);
    assert(f.events[f.n - 1].event.kind == XIAOTAI_MQTT_PUBLISHED && f.events[f.n - 1].event.message_id == retained);
    assert(xiaotai_mqtt_host_take(f.client, &record, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_OK &&
           record.message_id == retained && record.duplicate && data[0] == 'Q');
    assert(xiaotai_mqtt_host_ack(f.client, (uint16_t)retained) == XIAOTAI_ERR_NOT_FOUND);

    xiaotai_mqtt_host_fail_write(f.client);
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 0, XIAOTAI_MQTT_SEND_NOW, NULL) == XIAOTAI_FAIL);
    int32_t no_id = -1;
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 0, XIAOTAI_MQTT_SEND_NOW, &no_id) == XIAOTAI_OK && !no_id);
    assert(xiaotai_mqtt_host_take(f.client, &record, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_OK &&
           !record.message_id && !record.qos && data[0] == 'X');
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(f.client, &stats);
    assert(stats.outbox_full == 1 && stats.published == 3 && stats.publish_failures == 2);
    xiaotai_mqtt_host_puback(f.client, false);
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 1, XIAOTAI_MQTT_SEND_NOW, &retained) == XIAOTAI_OK);
    flush(f.client);
    const size_t before_stop = f.n;
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(f.n == before_stop);
    assert(xiaotai_mqtt_host_ack(f.client, (uint16_t)retained) == XIAOTAI_ERR_INVALID_STATE);
    /* Explicit stop discards the queue; start cannot replay or ACK it. */
    f.seen_worker = false;
    xiaotai_mqtt_host_puback(f.client, true);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    flush(f.client);
    assert(f.n == before_stop + 1 && f.events[f.n - 1].event.kind == XIAOTAI_MQTT_CONNECTED);
    assert(xiaotai_mqtt_host_take(f.client, &record, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_mqtt_host_ack(f.client, (uint16_t)retained) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_mqtt_publish(f.client, "p", &value, 1, 1, XIAOTAI_MQTT_SEND_NOW, &retained) == XIAOTAI_OK);
    flush(f.client);
    assert(f.events[f.n - 1].event.kind == XIAOTAI_MQTT_PUBLISHED && f.events[f.n - 1].event.message_id == retained);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    end(&f);
}

typedef struct { xiaotai_mqtt_t *client; atomic_bool done; } stopper_t;
static void *stopper(void *arg)
{
    stopper_t *s = arg;
    assert(xiaotai_mqtt_stop(s->client) == XIAOTAI_OK);
    atomic_store(&s->done, true);
    return NULL;
}

static void stop_fences_callback(void)
{
    fixture_t f;
    setup(&f);
    f.hold_next = true;
    const xiaotai_mqtt_config_t opts = config(&f);
    assert(xiaotai_mqtt_create(&opts, &f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    await_hold(&f);
    /* The connected callback is held, so this accepted packet cannot yet be
     * sent. Stop must discard it as well as already transmitted QoS 1 data. */
    int32_t queued;
    assert(xiaotai_mqtt_publish(f.client, "p", "Q", 1, 1,
                               XIAOTAI_MQTT_SEND_QUEUED, &queued) == XIAOTAI_OK);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(f.client, 0) == XIAOTAI_ERR_TIMEOUT);
    stopper_t s = {.client = f.client};
    pthread_t task;
    assert(pthread_create(&task, NULL, stopper, &s) == 0);
    const int64_t limit = xiaotai_time_us() + 5000000;
    while (xiaotai_mqtt_wake(f.client) == XIAOTAI_OK) {
        assert(xiaotai_time_us() < limit);
        xiaotai_delay_ms(1);
    }
    assert(!atomic_load(&s.done));
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_ERR_INVALID_STATE);
    release(&f);
    assert(pthread_join(task, NULL) == 0 && atomic_load(&s.done));
    assert(f.n == 1 && f.events[0].event.kind == XIAOTAI_MQTT_CONNECTED);
    assert(xiaotai_mqtt_wake(f.client) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(f.client, &stats);
    assert(!stats.published);
    f.seen_worker = false;
    assert(xiaotai_mqtt_start(f.client) == XIAOTAI_OK);
    flush(f.client);
    assert(f.n == 2 && f.events[1].event.kind == XIAOTAI_MQTT_CONNECTED);
    assert(xiaotai_mqtt_host_ack(f.client, (uint16_t)queued) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    end(&f);
}

static uint32_t cycle(unsigned fail_one)
{
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(fail_one);
    fixture_t f;
    setup(&f);
    xiaotai_mqtt_config_t opts = config(&f);
    opts.uri = "mqtts://host.example";
    xiaotai_err_t result = xiaotai_mqtt_create(&opts, &f.client);
    if (result == XIAOTAI_OK) {
        result = xiaotai_mqtt_start(f.client);
        if (result == XIAOTAI_OK) {
            flush(f.client);
            xiaotai_mqtt_stats_t s;
            xiaotai_mqtt_stats(f.client, &s);
            if (!s.connected) {
                assert(fail_one && s.errors == 1 && !s.connects);
                xiaotai_mqtt_host_advance_ms(f.client, opts.reconnect_ms);
                flush(f.client);
                xiaotai_mqtt_stats(f.client, &s);
                assert(s.connected);
            }
            const xiaotai_mqtt_topic_t subs[] = {{.topic = "a", .qos = 1}, {.topic = "b", .qos = 0}};
            int32_t id;
            result = xiaotai_mqtt_subscribe(f.client, subs, LEN(subs), &id);
            assert(result == XIAOTAI_OK || (fail_one && result == XIAOTAI_FAIL));
            assert(xiaotai_mqtt_host_subscriptions(f.client) == (result == XIAOTAI_OK ? 2U : 0U));
            flush(f.client);
            result = xiaotai_mqtt_host_message(f.client, "a", "16", 2, 1, 16, false, false, NULL, 0);
            assert(result == XIAOTAI_OK || (fail_one && result == XIAOTAI_ERR_NO_MEM));
            flush(f.client);
            result = xiaotai_mqtt_publish(f.client, "b", "1", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &id);
            assert(result == XIAOTAI_OK || (fail_one && result == XIAOTAI_ERR_NO_MEM));
            flush(f.client);
            assert(xiaotai_mqtt_stop(f.client) == XIAOTAI_OK);
        } else assert(fail_one && result == XIAOTAI_ERR_NO_MEM);
        assert(xiaotai_mqtt_destroy(f.client) == XIAOTAI_OK);
    } else assert(fail_one && result == XIAOTAI_ERR_NO_MEM && !f.client);
    end(&f);
    assert(!xiaotai_mem_host_fail_pending());
    assert(!xiaotai_mem_host_outstanding());
    return xiaotai_mem_host_allocations();
}

int main(void)
{
    configuration();
    subscribe_and_reconnect();
    messages_and_wake();
    publication_lifecycle();
    stop_fences_callback();
    uint32_t baseline = cycle(0);
    assert(baseline >= 12);
    for (unsigned i = 0; i < 10; ++i) assert(cycle(0) == baseline);
    for (unsigned i = 1; i <= baseline; ++i) (void)cycle(i);
    return 0;
}
