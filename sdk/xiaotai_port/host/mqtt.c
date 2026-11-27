// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "mqtt_config.h"
#include "xiaotai_mqtt_host.h"
#include "mem_ledger.h"
#include "xiaotai/port/mem.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HOST_EVENTS_MAX 32U
#define HOST_OBSERVATIONS_MAX 32U
#define HOST_SUB_REQUESTS_MAX 32U

typedef struct publication {
    struct publication *next;
    size_t topic_size, size, charge;
    int32_t id;
    uint8_t qos;
    bool sent, sent_before, duplicate, ready_ack;
    unsigned char bytes[]; /* NUL-terminated topic followed by payload */
} publication_t;

typedef struct incoming {
    struct incoming *next;
    xiaotai_mqtt_event_t event;
    uint8_t granted[XIAOTAI_MQTT_SUBSCRIBE_MAX];
    size_t fragments;
    /* DATA stores the fragment sizes, then a topic without NUL, then the payload. */
    size_t lengths[];
} incoming_t;

typedef struct subscription {
    struct subscription *next;
    char topic[];
} subscription_t;

typedef struct {
    uint16_t id;
    bool reply_queued;
    size_t count;
    uint8_t grants[XIAOTAI_MQTT_SUBSCRIBE_MAX];
} sub_request_t;

typedef struct {
    subscription_t *subscriptions;
    size_t count;
    sub_request_t requests[HOST_SUB_REQUESTS_MAX];
} broker_session_t;

typedef struct {
    xiaotai_mqtt_t *client;
} worker_context_t;

struct xiaotai_mqtt {
    mqtt_port_config_t cfg;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    worker_context_t *worker;
    bool stopping, dispatching, wake, close_requested;
    bool send_suback, auto_puback, fail_next_write;
    xiaotai_mqtt_error_t conn_error;
    int32_t conn_detail;
    uint16_t sequence;
    uint64_t reconnect_at, clock_offset;
    uint8_t suback_codes[XIAOTAI_MQTT_SUBSCRIBE_MAX];
    size_t suback_count;
    uint8_t *rx, *tx;
    broker_session_t *session;
    incoming_t *events, *last_event;
    size_t event_count;
    publication_t *outbox, *observation, *last_observation;
    size_t outbox_bytes, observation_count;
    xiaotai_mqtt_stats_t stats;
};

static uint64_t real_us(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

static struct timespec absolute(uint64_t us)
{
    return (struct timespec){.tv_sec = (time_t)(us / 1000000U), .tv_nsec = (long)(us % 1000000U) * 1000L};
}

static uint64_t broker_us(const xiaotai_mqtt_t *c) { return real_us() + c->clock_offset; }
static void changed(xiaotai_mqtt_t *c) { pthread_cond_broadcast(&c->changed); }

static bool on_worker(const xiaotai_mqtt_t *c)
{
    return c->worker && pthread_equal(c->thread, pthread_self());
}

static bool connected(const xiaotai_mqtt_t *c)
{
    return c->worker && !c->stopping && c->stats.connected;
}

static void emit(xiaotai_mqtt_t *c, const xiaotai_mqtt_event_t *event)
{
    c->dispatching = true;
    pthread_mutex_unlock(&c->mutex);
    c->cfg.value.handler(c->cfg.value.ctx, event);
    pthread_mutex_lock(&c->mutex);
    c->dispatching = false;
    changed(c);
}

static void clear_events(xiaotai_mqtt_t *c)
{
    while (c->events) {
        incoming_t *event = c->events;
        c->events = event->next;
        xiaotai_mem_free(event);
    }
    c->last_event = NULL;
    c->event_count = 0;
}

static void clear_session(xiaotai_mqtt_t *c)
{
    if (c->session) {
        subscription_t *at = c->session->subscriptions;
        while (at) {
            subscription_t *next = at->next;
            xiaotai_mem_free(at);
            at = next;
        }
        xiaotai_mem_free(c->session);
        c->session = NULL;
    }
    c->stats.connected = false;
    clear_events(c);
    /* Reconnect retains pending publications, not clean-session state.
     * Explicit stop clears the outbox after the worker has finished. */
    for (publication_t *p = c->outbox; p; p = p->next) {
        p->sent = false;
        p->ready_ack = false;
    }
}

static void clear_observations(xiaotai_mqtt_t *c)
{
    while (c->observation) {
        publication_t *p = c->observation;
        c->observation = p->next;
        xiaotai_mem_free(p);
    }
    c->last_observation = NULL;
    c->observation_count = 0;
}

static publication_t *new_publication(const char *topic, size_t topic_size, const void *data, size_t size)
{
    publication_t *p = xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*p) + topic_size + 1 + size);
    if (!p) return NULL;
    p->topic_size = topic_size;
    p->size = size;
    memcpy(p->bytes, topic, topic_size + 1);
    if (size) memcpy(p->bytes + topic_size + 1, data, size);
    return p;
}

static bool write_allowed(xiaotai_mqtt_t *c)
{
    const bool fail = c->fail_next_write;
    c->fail_next_write = false;
    return !fail;
}

/* Called on the caller for SEND_NOW, on the client's task for SEND_QUEUED.
 * The observation belongs to the fake broker; QoS 1 ownership stays in outbox. */
static bool send_one(xiaotai_mqtt_t *c, publication_t *p)
{
    if (!write_allowed(c)) return false;
    const size_t count = p->topic_size + 1 + p->size;
    publication_t *copy = xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*copy) + count);
    if (!copy) return false;
    const size_t chunk = c->cfg.value.tx_buffer_bytes;
    for (size_t at = 0; at < count;) {
        const size_t len = count - at < chunk ? count - at : chunk;
        memcpy(c->tx, p->bytes + at, len);
        memcpy(copy->bytes + at, c->tx, len);
        at += len;
    }
    copy->topic_size = p->topic_size;
    copy->size = p->size;
    copy->id = p->id;
    copy->qos = p->qos;
    copy->duplicate = p->sent_before;
    if (c->observation_count == HOST_OBSERVATIONS_MAX) {
        publication_t *old = c->observation;
        c->observation = old->next;
        xiaotai_mem_free(old);
        --c->observation_count;
    }
    if (c->last_observation) c->last_observation->next = copy;
    else c->observation = copy;
    c->last_observation = copy;
    ++c->observation_count;
    ++c->stats.published;
    p->sent = p->sent_before = true;
    p->ready_ack = p->qos && c->auto_puback;
    return true;
}

static void enqueue_event(xiaotai_mqtt_t *c, incoming_t *e)
{
    if (c->last_event) c->last_event->next = e;
    else c->events = e;
    c->last_event = e;
    ++c->event_count;
    changed(c);
}

static sub_request_t *sub_request(broker_session_t *session, uint16_t id)
{
    if (!session) return NULL;
    for (size_t i = 0; i < HOST_SUB_REQUESTS_MAX; ++i)
        if (session->requests[i].id == id) return &session->requests[i];
    return NULL;
}

static void make_suback(xiaotai_mqtt_t *c, incoming_t *ack, sub_request_t *request)
{
    ack->event.kind = XIAOTAI_MQTT_SUBSCRIBED;
    ack->event.message_id = request->id;
    ack->event.granted_count = request->count;
    memcpy(ack->granted, request->grants, request->count);
    request->reply_queued = true;
    enqueue_event(c, ack);
}

static publication_t **ready_publication(xiaotai_mqtt_t *c)
{
    for (publication_t **p = &c->outbox; *p; p = &(*p)->next) {
        if (!(*p)->sent || (*p)->ready_ack) return p;
    }
    return NULL;
}

static void free_publication(xiaotai_mqtt_t *c, publication_t **slot)
{
    publication_t *p = *slot;
    *slot = p->next;
    c->outbox_bytes -= p->charge;
    xiaotai_mem_free(p);
}

static void deliver(xiaotai_mqtt_t *c, incoming_t *item)
{
    xiaotai_mqtt_event_t event = item->event;
    if (event.kind != XIAOTAI_MQTT_DATA) {
        if (event.kind == XIAOTAI_MQTT_SUBSCRIBED) {
            sub_request_t *request = sub_request(c->session, (uint16_t)event.message_id);
            if (request) request->id = 0;
            event.granted = item->granted;
        }
        emit(c, &event);
        return;
    }
    for (size_t i = 0; i < item->fragments && !c->stopping; ++i) {
        event.size = item->lengths[i];
        event.data = NULL;
        if (event.size) {
            memcpy(c->rx, item->event.data + event.offset, event.size);
            event.data = c->rx;
        }
        ++c->stats.fragments;
        if (i + 1 == item->fragments) ++c->stats.messages;
        emit(c, &event);
        event.offset += event.size;
    }
}

static void emit_error(xiaotai_mqtt_t *c, xiaotai_mqtt_error_t error, int32_t detail)
{
    ++c->stats.errors;
    emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_ERROR, .error = error, .detail = detail});
}

static void broken(xiaotai_mqtt_t *c)
{
    clear_session(c);
    c->close_requested = false;
    c->reconnect_at = broker_us(c) + (uint64_t)c->cfg.value.reconnect_ms * 1000U;
    ++c->stats.disconnects;
    emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_DISCONNECTED});
}

static void attempt(xiaotai_mqtt_t *c)
{
    if (!c->conn_error) {
        c->session = xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c->session));
        if (c->session) {
            ++c->stats.connects;
            c->stats.connected = true;
            emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_CONNECTED});
            return;
        }
    }
    c->reconnect_at = broker_us(c) + (uint64_t)c->cfg.value.reconnect_ms * 1000U;
    emit_error(c, c->conn_error ? c->conn_error : XIAOTAI_MQTT_ERROR_TRANSPORT,
               c->conn_error ? c->conn_detail : ENOMEM);
}

static void *client_thread(void *arg)
{
    worker_context_t *worker = arg;
    xiaotai_mqtt_t *c = worker->client;
    pthread_mutex_lock(&c->mutex);
    while (!c->stopping) {
        if (c->close_requested) {
            broken(c);
        } else if (!c->stats.connected && broker_us(c) >= c->reconnect_at) {
            attempt(c);
        } else if (c->events) {
            incoming_t *e = c->events;
            c->events = e->next;
            if (!c->events) c->last_event = NULL;
            --c->event_count;
            deliver(c, e);
            xiaotai_mem_free(e);
        } else if (c->wake) {
            c->wake = false;
            emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_WAKE});
        } else {
            publication_t **slot = connected(c) ? ready_publication(c) : NULL;
            if (slot) {
                publication_t *p = *slot;
                if (p->ready_ack) {
                    const int32_t id = p->id;
                    free_publication(c, slot);
                    emit(c, &(xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_PUBLISHED, .message_id = id});
                } else if (!send_one(c, p)) {
                    ++c->stats.publish_failures;
                    c->close_requested = true;
                    emit_error(c, XIAOTAI_MQTT_ERROR_TRANSPORT, EIO);
                } else if (!p->qos) {
                    free_publication(c, slot);
                }
            } else {
                changed(c);
                if (c->stats.connected) (void)pthread_cond_wait(&c->changed, &c->mutex);
                else {
                    const struct timespec at = absolute(c->reconnect_at - c->clock_offset);
                    (void)pthread_cond_timedwait(&c->changed, &c->mutex, &at);
                }
            }
        }
    }
    clear_session(c);
    clear_observations(c);
    c->wake = c->close_requested = false;
    xiaotai_mem_host_task_close(worker);
    xiaotai_mem_free(worker);
    c->worker = NULL;
    changed(c);
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

xiaotai_err_t xiaotai_mqtt_create(const xiaotai_mqtt_config_t *config, xiaotai_mqtt_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    mqtt_port_config_t cfg;
    xiaotai_err_t err = mqtt_port_config_copy(config, true, &cfg);
    if (err != XIAOTAI_OK) return err;
    xiaotai_mqtt_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) {
        mqtt_port_config_clear(&cfg);
        return XIAOTAI_ERR_NO_MEM;
    }
    c->cfg = cfg;
    c->rx = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, cfg.value.rx_buffer_bytes);
    c->tx = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, cfg.value.tx_buffer_bytes);
    err = XIAOTAI_ERR_NO_MEM;
    if (c->rx && c->tx) {
        err = XIAOTAI_FAIL;
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) == 0) {
            if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                pthread_mutex_init(&c->mutex, NULL) == 0) {
                if (pthread_cond_init(&c->changed, &attr) == 0) {
                    c->auto_puback = c->send_suback = true;
                    *out = c;
                } else {
                    pthread_mutex_destroy(&c->mutex);
                }
            }
            pthread_condattr_destroy(&attr);
        }
    }
    if (*out) return XIAOTAI_OK;
    xiaotai_mem_free(c->rx);
    xiaotai_mem_free(c->tx);
    mqtt_port_config_clear(&c->cfg);
    xiaotai_mem_free(c);
    return err;
}

xiaotai_err_t xiaotai_mqtt_start(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    if (c->worker || c->stopping) {
        pthread_mutex_unlock(&c->mutex);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    worker_context_t *worker = xiaotai_mem_host_fixture_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*worker));
    if (!worker) {
        pthread_mutex_unlock(&c->mutex);
        return XIAOTAI_ERR_NO_MEM;
    }
    worker->client = c;
    pthread_attr_t attrs;
    bool started = false;
    if (pthread_attr_init(&attrs) == 0) {
        /* A native stack is larger than its embedded equivalent, especially
         * under sanitizers. Its memory is handled by the pthread runtime. */
        size_t stack = c->cfg.value.task_stack_bytes;
        if (stack < 256U * 1024U) stack = 256U * 1024U;
        started = pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) == 0 &&
                  pthread_attr_setstacksize(&attrs, stack) == 0 &&
                  pthread_create(&c->thread, &attrs, client_thread, worker) == 0;
        pthread_attr_destroy(&attrs);
    }
    if (started) {
        xiaotai_mem_host_task_open(worker, "xiaotai_mqtt", XIAOTAI_MEM_INTERNAL,
                                   c->cfg.value.task_stack_bytes, XIAOTAI_LEDGER_CALLER);
        c->worker = worker;
        c->reconnect_at = broker_us(c);
    } else xiaotai_mem_free(worker);
    pthread_mutex_unlock(&c->mutex);
    return started ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_mqtt_stop(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    if (on_worker(c) || c->stopping) {
        pthread_mutex_unlock(&c->mutex);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (c->worker) {
        c->stopping = true;
        changed(c);
        while (c->worker) (void)pthread_cond_wait(&c->changed, &c->mutex);
        c->stopping = false;
    }
    while (c->outbox) free_publication(c, &c->outbox);
    pthread_mutex_unlock(&c->mutex);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_mqtt_destroy(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_OK;
    pthread_mutex_lock(&c->mutex);
    if (c->worker || c->stopping) {
        pthread_mutex_unlock(&c->mutex);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    while (c->outbox) free_publication(c, &c->outbox);
    pthread_mutex_unlock(&c->mutex);
    pthread_cond_destroy(&c->changed);
    pthread_mutex_destroy(&c->mutex);
    xiaotai_mem_free(c->rx);
    xiaotai_mem_free(c->tx);
    mqtt_port_config_clear(&c->cfg);
    xiaotai_mem_free(c);
    return XIAOTAI_OK;
}

static uint16_t packet_id(xiaotai_mqtt_t *c)
{
    for (uint32_t tries = 0; tries < UINT16_MAX; ++tries) {
        if (++c->sequence == 0) ++c->sequence;
        if (sub_request(c->session, c->sequence)) continue;
        bool taken = false;
        for (publication_t *p = c->outbox; p; p = p->next) {
            if (p->id == c->sequence) {
                taken = true;
                break;
            }
        }
        if (!taken) return c->sequence;
    }
    return 0;
}

static bool subscribed(subscription_t *at, const char *topic)
{
    for (; at; at = at->next) if (!strcmp(at->topic, topic)) return true;
    return false;
}

xiaotai_err_t xiaotai_mqtt_subscribe(xiaotai_mqtt_t *c, const xiaotai_mqtt_topic_t *topics,
                                     size_t count, int32_t *id)
{
    if (id) *id = 0;
    if (!c || !id || !mqtt_port_topics_valid(topics, count)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    incoming_t *ack = NULL;
    subscription_t *new_topics = NULL;
    size_t new_count = 0;
    if (!connected(c)) goto done;
    result = XIAOTAI_FAIL;
    if (!write_allowed(c) || (c->suback_count && c->suback_count != count)) goto done;
    sub_request_t *slot = sub_request(c->session, 0);
    if (!slot) goto done;
    if (c->send_suback) {
        if (c->event_count == HOST_EVENTS_MAX) goto done;
        ack = xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*ack));
        if (!ack) goto done;
    }
    sub_request_t request = {.count = count};
    for (size_t i = 0; i < count; ++i) {
        const uint8_t grant = c->suback_count ? c->suback_codes[i] : topics[i].qos;
        request.grants[i] = grant;
        const char *topic = topics[i].topic;
        if (grant == XIAOTAI_MQTT_REFUSED || subscribed(c->session->subscriptions, topic) ||
            subscribed(new_topics, topic)) continue;
        const size_t bytes = strlen(topic) + 1;
        subscription_t *added = xiaotai_mem_host_fixture_alloc(XIAOTAI_MEM_BULK, sizeof(*added) + bytes);
        if (!added) goto done;
        memcpy(added->topic, topic, bytes);
        added->next = new_topics;
        new_topics = added;
        ++new_count;
    }
    *id = packet_id(c);
    if (!*id) goto done;
    while (new_topics) {
        subscription_t *item = new_topics;
        new_topics = item->next;
        item->next = c->session->subscriptions;
        c->session->subscriptions = item;
    }
    c->session->count += new_count;
    request.id = (uint16_t)*id;
    *slot = request;
    if (ack) {
        make_suback(c, ack, slot);
        ack = NULL;
    }
    result = XIAOTAI_OK;
done:
    xiaotai_mem_free(ack);
    while (new_topics) {
        subscription_t *item = new_topics;
        new_topics = item->next;
        xiaotai_mem_free(item);
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

xiaotai_err_t xiaotai_mqtt_publish(xiaotai_mqtt_t *c, const char *topic, const void *data, size_t size,
                                    uint8_t qos, xiaotai_mqtt_send_t send, int32_t *id)
{
    if (id) *id = 0;
    size_t bytes;
    if (!c || !mqtt_port_publish_size(topic, data, size, qos, send, &bytes)) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    publication_t *p = NULL;
    if (!connected(c)) goto done;
    const size_t charge = send == XIAOTAI_MQTT_SEND_QUEUED || qos ? bytes : 0U;
    const size_t limit = c->cfg.value.outbox_limit_bytes ? c->cfg.value.outbox_limit_bytes : SIZE_MAX;
    if (charge > limit - c->outbox_bytes) {
        ++c->stats.outbox_full;
        result = XIAOTAI_ERR_NO_MEM;
        goto done;
    }
    p = new_publication(topic, strlen(topic), data, size);
    if (!p) {
        result = XIAOTAI_ERR_NO_MEM;
        goto done;
    }
    p->charge = charge;
    p->qos = qos;
    p->id = qos ? packet_id(c) : 0;
    if (qos && !p->id) {
        result = XIAOTAI_ERR_NO_MEM;
        goto done;
    }
    if (send == XIAOTAI_MQTT_SEND_NOW && !send_one(c, p)) {
        result = XIAOTAI_FAIL;
        goto done;
    }
    if (id) *id = p->id;
    if (charge) {
        publication_t **slot = &c->outbox;
        while (*slot) slot = &(*slot)->next;
        *slot = p;
        p = NULL;
        c->outbox_bytes += charge;
    }
    changed(c);
    result = XIAOTAI_OK;
done:
    if (result != XIAOTAI_OK) ++c->stats.publish_failures;
    xiaotai_mem_free(p);
    pthread_mutex_unlock(&c->mutex);
    return result;
}

xiaotai_err_t xiaotai_mqtt_wake(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    const bool ready = c->worker && !c->stopping;
    if (ready && !c->wake) {
        c->wake = true;
        changed(c);
    }
    pthread_mutex_unlock(&c->mutex);
    return ready ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_mqtt_stats(xiaotai_mqtt_t *c, xiaotai_mqtt_stats_t *stats)
{
    if (!stats) return;
    if (c) pthread_mutex_lock(&c->mutex);
    *stats = c ? c->stats : (xiaotai_mqtt_stats_t){0};
    if (c) pthread_mutex_unlock(&c->mutex);
}

void xiaotai_mqtt_host_connack(xiaotai_mqtt_t *c, xiaotai_mqtt_error_t error, int32_t detail)
{
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    c->conn_error = error;
    c->conn_detail = detail;
    pthread_mutex_unlock(&c->mutex);
}

xiaotai_err_t xiaotai_mqtt_host_disconnect(xiaotai_mqtt_t *c)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    const bool active = connected(c);
    if (active) {
        c->close_requested = true;
        changed(c);
    }
    pthread_mutex_unlock(&c->mutex);
    return active ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_mqtt_host_advance_ms(xiaotai_mqtt_t *c, uint32_t ms)
{
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    c->clock_offset += (uint64_t)ms * 1000U;
    changed(c);
    pthread_mutex_unlock(&c->mutex);
}

xiaotai_err_t xiaotai_mqtt_host_suback(xiaotai_mqtt_t *c, bool send, const uint8_t *codes, size_t count)
{
    if (!c || (count && !codes) || count > XIAOTAI_MQTT_SUBSCRIBE_MAX) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) {
        if (codes[i] > 2 && codes[i] != XIAOTAI_MQTT_REFUSED) return XIAOTAI_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&c->mutex);
    c->send_suback = send;
    c->suback_count = count;
    if (count) memcpy(c->suback_codes, codes, count);
    pthread_mutex_unlock(&c->mutex);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_mqtt_host_suback_reply(xiaotai_mqtt_t *c, uint16_t id)
{
    if (!c || !id) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (connected(c)) {
        const bool full = c->event_count == HOST_EVENTS_MAX;
        sub_request_t *request = sub_request(c->session, id);
        result = XIAOTAI_ERR_NOT_FOUND;
        if (request && !request->reply_queued) {
            result = XIAOTAI_ERR_NO_MEM;
            incoming_t *ack = full ? NULL : xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*ack));
            if (ack) {
                make_suback(c, ack, request);
                result = XIAOTAI_OK;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

void xiaotai_mqtt_host_next_id(xiaotai_mqtt_t *c, uint16_t id)
{
    if (!c || !id) return;
    pthread_mutex_lock(&c->mutex);
    c->sequence = (uint16_t)(id - 1);
    pthread_mutex_unlock(&c->mutex);
}

size_t xiaotai_mqtt_host_subscriptions(xiaotai_mqtt_t *c)
{
    if (!c) return 0;
    pthread_mutex_lock(&c->mutex);
    const size_t count = c->session ? c->session->count : 0;
    pthread_mutex_unlock(&c->mutex);
    return count;
}

xiaotai_err_t xiaotai_mqtt_host_message(xiaotai_mqtt_t *c, const char *topic,
    const void *data, size_t size, uint8_t qos, uint16_t id, bool retained, bool duplicate,
    const size_t *lengths, size_t fragments)
{
    size_t wire_bytes;
    if (!c || qos > 2 || (qos && !id) || (!qos && id) ||
        !mqtt_port_publish_size(topic, data, size, qos ? 1 : 0, XIAOTAI_MQTT_SEND_QUEUED, &wire_bytes) ||
        (lengths == NULL) != (fragments == 0)) return XIAOTAI_ERR_INVALID_ARG;
    size_t topic_size = strlen(topic);
    if (topic_size > XIAOTAI_MQTT_TOPIC_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const size_t rx = c->cfg.value.rx_buffer_bytes;
    if (fragments) {
        size_t left = size;
        for (size_t i = 0; i < fragments; ++i) {
            if (lengths[i] > left || lengths[i] > rx ||
                (lengths[i] == left && i + 1 < fragments)) return XIAOTAI_ERR_INVALID_ARG;
            left -= lengths[i];
        }
        if (left) return XIAOTAI_ERR_INVALID_ARG;
    } else {
        fragments = size ? (size - 1) / rx + 1 : 1;
    }
    if (fragments > (SIZE_MAX - sizeof(incoming_t) - topic_size - size) / sizeof(size_t))
        return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (connected(c)) {
        result = XIAOTAI_ERR_NO_MEM;
        if (c->event_count < HOST_EVENTS_MAX) {
            incoming_t *item = xiaotai_mem_host_fixture_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*item) +
                                                   fragments * sizeof(size_t) + topic_size + size);
            if (item) {
                char *copied_topic = (char *)(item->lengths + fragments);
                memcpy(copied_topic, topic, topic_size);
                uint8_t *copied_data = (uint8_t *)copied_topic + topic_size;
                if (size) memcpy(copied_data, data, size);
                item->fragments = fragments;
                for (size_t i = 0, off = 0; i < fragments; ++i) {
                    item->lengths[i] = lengths ? lengths[i] : size - off < rx ? size - off : rx;
                    off += item->lengths[i];
                }
                item->event = (xiaotai_mqtt_event_t){
                    .kind = XIAOTAI_MQTT_DATA, .message_id = id, .topic = copied_topic,
                    .topic_size = topic_size, .data = copied_data, .total = size,
                    .qos = qos, .retained = retained, .duplicate = duplicate,
                };
                enqueue_event(c, item);
                result = XIAOTAI_OK;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

void xiaotai_mqtt_host_puback(xiaotai_mqtt_t *c, bool automatic)
{
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    c->auto_puback = automatic;
    pthread_mutex_unlock(&c->mutex);
}

xiaotai_err_t xiaotai_mqtt_host_ack(xiaotai_mqtt_t *c, uint16_t id)
{
    if (!c || !id) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (connected(c)) {
        result = XIAOTAI_ERR_NOT_FOUND;
        for (publication_t *p = c->outbox; p; p = p->next) {
            if (p->id == id && p->sent) {
                p->ready_ack = true;
                result = XIAOTAI_OK;
                changed(c);
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

void xiaotai_mqtt_host_fail_write(xiaotai_mqtt_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    c->fail_next_write = true;
    pthread_mutex_unlock(&c->mutex);
}

xiaotai_err_t xiaotai_mqtt_host_take(xiaotai_mqtt_t *c, xiaotai_mqtt_host_publication_t *out,
    char *topic, size_t topic_capacity, void *data, size_t data_capacity)
{
    if (!c || !out || (topic_capacity && !topic) || (data_capacity && !data))
        return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    publication_t *p = c->observation;
    xiaotai_err_t result = XIAOTAI_ERR_NOT_FOUND;
    if (p) {
        *out = (xiaotai_mqtt_host_publication_t){
            .message_id = p->id, .qos = p->qos, .duplicate = p->duplicate, .size = p->size,
        };
        result = XIAOTAI_ERR_INVALID_SIZE;
        if (p->topic_size < topic_capacity && p->size <= data_capacity) {
            memcpy(topic, p->bytes, p->topic_size + 1);
            if (p->size) memcpy(data, p->bytes + p->topic_size + 1, p->size);
            c->observation = p->next;
            if (!p->next) c->last_observation = NULL;
            --c->observation_count;
            xiaotai_mem_free(p);
            result = XIAOTAI_OK;
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

static bool runnable(xiaotai_mqtt_t *c)
{
    return c->dispatching || c->events || c->wake || c->close_requested ||
           (!c->stats.connected ? broker_us(c) >= c->reconnect_at : ready_publication(c) != NULL);
}

xiaotai_err_t xiaotai_mqtt_host_flush(xiaotai_mqtt_t *c, uint32_t timeout_ms)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->mutex);
    if (on_worker(c) || !c->worker || c->stopping) {
        pthread_mutex_unlock(&c->mutex);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const struct timespec at = absolute(real_us() + (uint64_t)timeout_ms * 1000U);
    int status = 0;
    while (c->worker && runnable(c) && status == 0) status = pthread_cond_timedwait(&c->changed, &c->mutex, &at);
    const bool drained = !c->worker || !runnable(c);
    pthread_mutex_unlock(&c->mutex);
    return drained ? XIAOTAI_OK : status == ETIMEDOUT ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL;
}
