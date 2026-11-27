// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "guest.h"
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/mqtt.h"

enum { GUEST_CLIENT_ID_BYTES = 65, POLL_MS = 10, INTERNAL_MESSAGE_BYTES = 12 * 1024 };
typedef enum { ACK_IDLE, ACK_WANT, ACK_PUBLISHED, ACK_COMPLETE } ack_state_t;

typedef struct {
    char *text;
    size_t total, received;
    int32_t packet;
    uint32_t generation;
    bool complete;
    char *fields;
    size_t fields_bytes, channel_offset;
} guest_data_t;

typedef struct {
    ack_state_t state;
    int64_t deadline;
    int32_t packet;
    xiaotai_err_t result;
    xiaotai_platform_done_fn_t done;
    void *ctx;
} guest_ack_t;

/* The guest mutex outlives an engine reset so a stale owner job can safely
 * check its token. The worker exclusively creates/stops/destroys client.
 * No provider call or owner callback runs with the mutex held. */
static struct {
    xiaotai_mutex_t lock;
    bool lock_initialized, message_borrowed;
    uint32_t id, id_seq, job, job_seq, network_revision;
    bool closing, silent, started, connected, ready, ready_pending, unready_pending, owner_running;
    atomic_bool live;
    int32_t subscription;
    int64_t subscribe_deadline, repost_at;
    xiaotai_err_t reason;
    xiaotai_mqtt_t *client;
    char *token;
    char client_id[GUEST_CLIENT_ID_BYTES];
    char cmd[sizeof("device//cmd") + GUEST_CLIENT_ID_BYTES - 1U];
    char ack[sizeof("device//ack") + GUEST_CLIENT_ID_BYTES - 1U];
    xiaotai_platform_guest_t handlers;
    guest_data_t data;
    guest_ack_t awaiting;
} g;

static void lock(void) { (void)xiaotai_mutex_lock(&g.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g.lock); }
static void post_owner(void);

static uint32_t advance(uint32_t *sequence) { if (!++*sequence) ++*sequence; return *sequence; }

static bool owner_live(uint32_t id)
{
    return g.id == id && id && !g.closing;
}

static void data_release(guest_data_t *data)
{
    if (data->text) xiaotai_identity_wipe(data->text, data->total + 1U);
    if (data->fields) xiaotai_identity_wipe(data->fields, data->fields_bytes);
    xiaotai_mem_free(data->text);
    xiaotai_mem_free(data->fields);
    *data = (guest_data_t){0};
}

static void clear_token(void)
{
    if (g.token) xiaotai_identity_wipe(g.token, strlen(g.token) + 1U);
    xiaotai_mem_free(g.token);
    g.token = NULL;
}

static void finish_ack(xiaotai_err_t result)
{
    if (g.awaiting.state == ACK_WANT || g.awaiting.state == ACK_PUBLISHED) {
        g.awaiting.state = ACK_COMPLETE;
        g.awaiting.result = result;
    }
}

static void close_request(xiaotai_err_t reason, bool silent)
{
    if (!g.id) return;
    if (!g.closing) {
        g.closing = true;
        g.reason = reason;
    }
    g.ready = g.ready_pending = g.unready_pending = false;
    if (silent) {
        g.silent = true;
        g.job = 0;
        g.awaiting = (guest_ack_t){0};
    } else finish_ack(XIAOTAI_ERR_PLATFORM_STALE);
}

static void retire(void)
{
    /* Worker joined the provider and the owner has taken any message it
     * currently borrows (g.data is no longer aliased). */
    data_release(&g.data);
    clear_token();
    memset(g.client_id, 0, sizeof(g.client_id));
    memset(g.cmd, 0, sizeof(g.cmd));
    memset(g.ack, 0, sizeof(g.ack));
    g.handlers = (xiaotai_platform_guest_t){0};
    g.awaiting = (guest_ack_t){0};
    g.id = g.job = 0;
    g.closing = g.silent = g.connected = g.ready = g.ready_pending = g.unready_pending = false;
    g.subscription = 0;
    g.subscribe_deadline = g.repost_at = 0;
}

static bool phase_for_guest(void)
{
    return !platform.status.authorized &&
        (platform.status.phase == XIAOTAI_PLATFORM_NO_IDENTITY ||
         platform.status.phase == XIAOTAI_PLATFORM_UNBOUND ||
         platform.status.phase == XIAOTAI_PLATFORM_PAUSED);
}

static int64_t subscribe_deadline(int64_t now)
{
    const uint16_t seconds = g.handlers.subscribe_timeout_s ? g.handlers.subscribe_timeout_s :
                             platform.config.subscribe_timeout_s;
    return now + (int64_t)seconds * 1000000;
}

xiaotai_err_t xiaotai_platform_guest_open(const xiaotai_platform_guest_t *guest, uint32_t *id)
{
    if (id) *id = 0;
    if (!guest || !id || !guest->client_id || !guest->token || guest->initial_subscribe_deadline_us < 0)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t client_bytes = strnlen(guest->client_id, sizeof(g.client_id));
    const size_t token_bytes = strnlen(guest->token, XIAOTAI_PLATFORM_BEARER_MAX);
    if (!client_bytes || !token_bytes) return XIAOTAI_ERR_INVALID_ARG;
    if (client_bytes == sizeof(g.client_id) || token_bytes == XIAOTAI_PLATFORM_BEARER_MAX)
        return XIAOTAI_ERR_INVALID_SIZE;
    /* These bytes cannot form the required literal ACK publish topic. */
    if (strpbrk(guest->client_id, "#+")) return XIAOTAI_ERR_INVALID_ARG;
    if (guest->initial_subscribe_deadline_us && xiaotai_time_us() >= guest->initial_subscribe_deadline_us)
        return XIAOTAI_ERR_TIMEOUT;
    if (!atomic_load(&platform.running) || platform.provisioning) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    platform_lock();
    xiaotai_err_t result = XIAOTAI_OK;
    if (!platform.network_up || !platform.status.discovered || atomic_load(&platform.stopped)) {
        result = XIAOTAI_ERR_PLATFORM_OFFLINE;
    } else if (!phase_for_guest()) result = XIAOTAI_ERR_INVALID_STATE;
    else {
        if (!g.lock_initialized) {
            result = xiaotai_mutex_init(&g.lock);
            g.lock_initialized = result == XIAOTAI_OK;
        }
        if (result != XIAOTAI_OK) { platform_unlock(); return result; }
        lock();
        if (g.id || g.owner_running) result = XIAOTAI_ERR_BUSY;
        else {
            g.token = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, token_bytes + 1U);
            if (!g.token) result = XIAOTAI_ERR_NO_MEM;
            else {
                memcpy(g.token, guest->token, token_bytes + 1U);
                memcpy(g.client_id, guest->client_id, client_bytes + 1U);
                (void)snprintf(g.cmd, sizeof(g.cmd), "device/%s/cmd", g.client_id);
                (void)snprintf(g.ack, sizeof(g.ack), "device/%s/ack", g.client_id);
                g.handlers = *guest;
                g.handlers.client_id = g.handlers.token = NULL;
                g.network_revision = platform.network_revision;
                g.id = *id = advance(&g.id_seq);
                g.subscribe_deadline = subscribe_deadline(xiaotai_time_us());
                if (guest->initial_subscribe_deadline_us && guest->initial_subscribe_deadline_us < g.subscribe_deadline)
                    g.subscribe_deadline = guest->initial_subscribe_deadline_us;
                atomic_store(&g.live, true);
            }
        }
        unlock();
    }
    platform_unlock();
    if (result == XIAOTAI_OK) platform_wake();
    return result;
}

xiaotai_err_t xiaotai_platform_guest_ack(uint32_t id, uint32_t timeout_ms, xiaotai_platform_done_fn_t done,
                                         void *ctx)
{
    if (!id || !timeout_ms || !done) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !atomic_load(&g.live)) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    xiaotai_err_t result = XIAOTAI_OK;
    if (!owner_live(id)) result = XIAOTAI_ERR_PLATFORM_STALE;
    else if (g.awaiting.state != ACK_IDLE) result = XIAOTAI_ERR_BUSY;
    else g.awaiting = (guest_ack_t){.state = ACK_WANT, .done = done, .ctx = ctx,
        .deadline = xiaotai_time_us() + (int64_t)timeout_ms * 1000};
    unlock();
    if (result == XIAOTAI_OK) platform_wake();
    return result;
}

bool xiaotai_platform_guest_subscribed(uint32_t id)
{
    if (!id || !atomic_load(&g.live)) return false;
    lock();
    const bool subscribed = owner_live(id) && g.ready && !g.unready_pending;
    unlock();
    return subscribed;
}

void xiaotai_platform_guest_close(uint32_t id)
{
    if (!id || !xiaotai_on_owner() || !atomic_load(&g.live)) return;
    lock();
    if (g.id == id) close_request(XIAOTAI_OK, true);
    unlock();
    platform_wake();
}

static void reassemble(const xiaotai_mqtt_event_t *e)
{
    /* Only one grant can wait for owner delivery. An unadmitted grant is
     * not application-ACKed; the issuer is free to send it again. */
    if (!g.ready || !g.handlers.message || g.data.complete || g.message_borrowed) return;
    guest_data_t *data = &g.data;
    const size_t topic_size = strlen(g.cmd);
    if (!e->topic || e->topic_size != topic_size || memcmp(e->topic, g.cmd, topic_size) ||
        !e->total || e->total >= platform.config.signal_bytes || e->offset > e->total ||
        e->size > e->total - e->offset || (e->size && !e->data)) {
        data_release(data);
        return;
    }
    if (!e->offset && (!data->text || data->received || e->message_id != data->packet || e->total != data->total)) {
        data_release(data);
        data->text = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, e->total + 1U);
        if (!data->text) return;
        data->total = e->total;
        data->packet = e->message_id;
        data->generation = xiaotai_identity_generation();
    }
    if (!data->text || data->packet != e->message_id || data->total != e->total || data->received != e->offset) {
        data_release(data);
        return;
    }
    if (e->size) memcpy(data->text + data->received, e->data, e->size);
    data->received += e->size;
    if (data->received != data->total) return;
    data->text[data->total] = '\0';
    platform_envelope_t envelope;
    if (platform_json_envelope(data->text, data->total, &envelope) != XIAOTAI_OK) {
        data_release(data);
        return;
    }
    size_t type = 0, channel = 0;
    (void)platform_json_span_text(data->text, envelope.type, NULL, 0, &type);
    if (envelope.has_channel) (void)platform_json_span_text(data->text, envelope.channel, NULL, 0, &channel);
    data->fields_bytes = type + 1U + channel + 1U;
    if (!platform.external && data->fields_bytes + data->total + 1U > INTERNAL_MESSAGE_BYTES) {
        data_release(data);
        return;
    }
    data->fields = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, data->fields_bytes);
    if (!data->fields) {
        data_release(data);
        return;
    }
    data->channel_offset = type + 1U;
    data->fields[type + 1U] = '\0';
    if (platform_json_span_text(data->text, envelope.type, data->fields, type + 1U, NULL) != XIAOTAI_OK ||
        (envelope.has_channel && platform_json_span_text(data->text, envelope.channel, data->fields + type + 1U,
            channel + 1U, NULL) != XIAOTAI_OK)) {
        data_release(data);
        return;
    }
    data->complete = true;
}

static void guest_event(void *ctx, const xiaotai_mqtt_event_t *event)
{
    const uint32_t id = (uint32_t)(uintptr_t)ctx;
    int64_t now = xiaotai_time_us();
    lock();
    if (!owner_live(id)) { unlock(); return; }
    xiaotai_mqtt_t *const current = g.client;
    switch (event->kind) {
    case XIAOTAI_MQTT_CONNECTED: {
        g.connected = true;
        g.ready = g.ready_pending = false;
        g.subscription = 0;
        if (!g.subscribe_deadline) g.subscribe_deadline = subscribe_deadline(now);
        const xiaotai_mqtt_topic_t topic = {.topic = g.cmd, .qos = 1};
        unlock();
        int32_t packet = 0;
        xiaotai_err_t result = xiaotai_mqtt_subscribe(current, &topic, 1, &packet);
        lock();
        if (owner_live(id)) {
            if (result == XIAOTAI_OK && packet > 0) g.subscription = packet;
            else close_request(XIAOTAI_FAIL, false);
        }
        break;
    }
    case XIAOTAI_MQTT_SUBSCRIBED:
        if (g.connected && g.subscription > 0 && g.subscription == event->message_id) {
            if (now >= g.subscribe_deadline) close_request(XIAOTAI_ERR_TIMEOUT, false);
            else if (!event->granted || event->granted_count != 1 || event->granted[0] > 1U)
                close_request(XIAOTAI_FAIL, false);
            else {
                g.ready = g.ready_pending = true;
                if (!g.handlers.initial_subscribe_deadline_us) g.subscribe_deadline = 0;
            }
            g.subscription = 0;
        }
        break;
    case XIAOTAI_MQTT_DISCONNECTED:
        if (g.handlers.unready && g.ready) g.unready_pending = true;
        g.connected = g.ready = g.ready_pending = false;
        g.subscription = 0;
        if (g.handlers.unready || !g.data.complete) data_release(&g.data);
        if (!g.subscribe_deadline) g.subscribe_deadline = subscribe_deadline(now);
        break;
    case XIAOTAI_MQTT_ERROR:
        if (event->error == XIAOTAI_MQTT_ERROR_REFUSED) close_request(XIAOTAI_FAIL, false);
        break;
    case XIAOTAI_MQTT_DATA:
        reassemble(event);
        break;
    case XIAOTAI_MQTT_PUBLISHED:
        if (g.awaiting.state == ACK_PUBLISHED && g.awaiting.packet == event->message_id)
            finish_ack(now < g.awaiting.deadline ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT);
        break;
    case XIAOTAI_MQTT_WAKE:
        if (g.awaiting.state == ACK_WANT && now < g.awaiting.deadline && g.ready) {
            /* Queueing from this serial callback publishes before any
             * corresponding PUBLISHED can be dispatched, even with an
             * immediate broker ACK. */
            static const char payload[] = "{\"ack\":true}";
            unlock();
            int32_t packet = 0;
            xiaotai_err_t result = xiaotai_mqtt_publish(current, g.ack, payload,
                sizeof(payload) - 1U, 1, XIAOTAI_MQTT_SEND_QUEUED, &packet);
            lock();
            if (owner_live(id) && g.awaiting.state == ACK_WANT) {
                if (result == XIAOTAI_OK && packet > 0) {
                    g.awaiting.state = ACK_PUBLISHED;
                    g.awaiting.packet = packet;
                } else finish_ack(result == XIAOTAI_OK ? XIAOTAI_FAIL : result);
            }
        }
        break;
    default: break;
    }
    unlock();
    post_owner();
    platform_wake();
}

static void owner_run(void *ctx)
{
    if (!atomic_load(&g.live)) return;
    lock();
    if (!atomic_load(&g.live) || g.job != (uint32_t)(uintptr_t)ctx || g.silent || !g.id) {
        unlock();
        return;
    }
    g.job = 0;
    g.owner_running = true;
    if (!g.closing && g.ready_pending && g.subscribe_deadline && xiaotai_time_us() >= g.subscribe_deadline)
        close_request(XIAOTAI_ERR_TIMEOUT, false);
    const uint32_t id = g.id;
    const xiaotai_platform_guest_t handlers = g.handlers;
    enum { NONE, READY, UNREADY, MESSAGE, ACK, CLOSED } action = NONE;
    guest_ack_t ack = {0};
    guest_data_t message = {0};
    xiaotai_err_t reason = g.reason;
    if (!g.closing && g.unready_pending) {
        g.unready_pending = false;
        action = UNREADY;
    } else if (!g.closing && g.ready_pending) {
        g.ready_pending = false;
        g.subscribe_deadline = 0;
        g.handlers.initial_subscribe_deadline_us = 0;
        action = READY;
    } else if (g.awaiting.state == ACK_COMPLETE) {
        ack = g.awaiting;
        g.awaiting = (guest_ack_t){0};
        action = ACK;
    } else if (!g.closing && g.data.complete) {
        message = g.data;
        g.data = (guest_data_t){0};
        g.message_borrowed = true;
        action = MESSAGE;
    } else if (g.closing && !g.client && !g.token) action = CLOSED;
    unlock();

    if (action == READY && handlers.ready) handlers.ready(id, handlers.ctx);
    if (action == UNREADY && handlers.unready) handlers.unready(id, handlers.ctx);
    if (action == MESSAGE && message.generation == xiaotai_identity_generation()) {
        const xiaotai_platform_signal_t signal = {.type = message.fields,
            .channel = message.fields + message.channel_offset, .json = message.text,
            .size = message.total, .generation = message.generation, .command = true};
        handlers.message(id, &signal, handlers.ctx);
    }
    if (action == ACK && ack.done) ack.done(ack.result, ack.ctx);
    if (action == CLOSED && handlers.closed) handlers.closed(id, reason, handlers.ctx);
    data_release(&message);
    lock();
    if (action == CLOSED && g.id == id) retire();
    if (action == MESSAGE) g.message_borrowed = false;
    g.owner_running = false;
    unlock();
    platform_wake();
}

static bool pending_owner(void)
{
    return g.id && !g.silent && (g.ready_pending || g.unready_pending || (!g.closing && g.data.complete) ||
        g.awaiting.state == ACK_COMPLETE || (g.closing && !g.client && !g.token));
}

static void post_owner(void)
{
    lock();
    if (g.job || g.owner_running || !pending_owner() || xiaotai_time_us() < g.repost_at) {
        unlock();
        return;
    }
    const uint32_t token = g.job = advance(&g.job_seq);
    unlock();
    if (xiaotai_run(owner_run, (void *)(uintptr_t)token) != XIAOTAI_OK) {
        lock();
        if (g.job == token) g.job = 0;
        g.repost_at = xiaotai_time_us() + POLL_MS * INT64_C(1000);
        unlock();
    }
}

static void start_client(void)
{
    char broker[XIAOTAI_PLATFORM_URL_MAX];
    xiaotai_err_t result = xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, broker, sizeof(broker));
    lock();
    const uint32_t id = g.id;
    if (!owner_live(id)) { unlock(); return; }
    const xiaotai_mqtt_config_t config = {
        .uri = broker, .client_id = g.client_id, .username = g.client_id, .password = g.token,
        .keepalive_s = platform.config.keepalive_s, .reconnect_ms = platform.config.reconnect_ms,
        .rx_buffer_bytes = platform.config.signaling_buffer_bytes, .tx_buffer_bytes = platform.config.signaling_buffer_bytes,
        .outbox_limit_bytes = platform.config.signaling_outbox_bytes, .task_stack_bytes = platform.config.signaling_stack_bytes,
        .handler = guest_event, .ctx = (void *)(uintptr_t)id,
    };
    unlock();
    xiaotai_mqtt_t *client = NULL;
    if (result == XIAOTAI_OK) result = xiaotai_mqtt_create(&config, &client);
    lock();
    g.client = client;
    clear_token(); /* port copied the credential on successful create */
    const bool start = result == XIAOTAI_OK && owner_live(id);
    unlock();
    if (start) result = xiaotai_mqtt_start(client);
    lock();
    g.started = start && result == XIAOTAI_OK;
    if (result != XIAOTAI_OK) close_request(result, false);
    unlock();
}

static xiaotai_err_t stop_client(void)
{
    lock();
    xiaotai_mqtt_t *client = g.client;
    const bool started = g.started;
    clear_token();
    unlock();
    if (client) {
        if (started) {
            const xiaotai_err_t stop = xiaotai_mqtt_stop(client);
            if (stop != XIAOTAI_OK) return stop;
            lock();
            g.started = false;
            unlock();
        }
        const xiaotai_err_t destroy = xiaotai_mqtt_destroy(client);
        if (destroy != XIAOTAI_OK) return destroy;
    }
    lock();
    g.client = NULL;
    data_release(&g.data);
    if (g.silent && !g.owner_running) retire();
    unlock();
    return XIAOTAI_OK;
}

uint32_t platform_guest_step(void)
{
    if (!atomic_load(&g.live)) return XIAOTAI_WAIT_FOREVER;
    const int64_t now = xiaotai_time_us();
    platform_lock();
    const bool network_up = platform.network_up;
    const uint32_t revision = platform.network_revision;
    platform_unlock();
    lock();
    if (!g.id) { unlock(); return XIAOTAI_WAIT_FOREVER; }
    if (!network_up || g.network_revision != revision)
        close_request(XIAOTAI_ERR_PLATFORM_OFFLINE, false);
    else if (g.subscribe_deadline && now >= g.subscribe_deadline) close_request(XIAOTAI_ERR_TIMEOUT, false);
    if (g.awaiting.deadline && now >= g.awaiting.deadline) finish_ack(XIAOTAI_ERR_TIMEOUT);
    const bool closing = g.closing;
    const bool start = !closing && !g.client && g.token;
    xiaotai_mqtt_t *wake = !closing && g.ready && g.awaiting.state == ACK_WANT ? g.client : NULL;
    unlock();
    if (closing) {
        if (stop_client() != XIAOTAI_OK) return POLL_MS;
    } else if (start) start_client();
    else if (wake) (void)xiaotai_mqtt_wake(wake);
    post_owner();
    lock();
    uint32_t wait = XIAOTAI_WAIT_FOREVER;
    if (g.id) {
        if (g.closing || pending_owner() || (g.ready && g.awaiting.state == ACK_WANT)) wait = POLL_MS;
        else if (g.subscribe_deadline) wait = platform_remaining_ms(g.subscribe_deadline);
        if (g.awaiting.state == ACK_WANT || g.awaiting.state == ACK_PUBLISHED) {
            const uint32_t left = platform_remaining_ms(g.awaiting.deadline);
            if (left < wait) wait = left;
        }
    }
    unlock();
    return wait;
}

xiaotai_err_t platform_guest_before_login(void *ctx)
{
    (void)ctx;
    if (!atomic_load(&g.live)) return XIAOTAI_OK;
    lock();
    const bool pending = g.id || g.owner_running;
    if (g.id) close_request(XIAOTAI_ERR_INVALID_STATE, false);
    unlock();
    if (!pending) return XIAOTAI_OK;
    if (stop_client() != XIAOTAI_OK) return XIAOTAI_ERR_BUSY;
    post_owner();
    return XIAOTAI_ERR_BUSY;
}

xiaotai_err_t platform_guest_shutdown(void)
{
    if (!atomic_load(&g.live)) return XIAOTAI_OK;
    lock();
    close_request(XIAOTAI_OK, true);
    unlock();
    return stop_client();
}

void platform_guest_reset(void)
{
    if (!atomic_exchange(&g.live, false)) return;
    for (;;) {
        lock();
        const bool running = g.owner_running;
        if (!running) {
            retire();
            unlock();
            break;
        }
        unlock();
        xiaotai_delay_ms(1);
    }
}
