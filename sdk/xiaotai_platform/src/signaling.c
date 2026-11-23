// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "signal_pool.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/mqtt.h"

#define MQTT_ATTEMPTS 3U
#define MQTT_RETRY_US INT64_C(3000000)
#define MQTT_POLL_US INT64_C(1000000)

/* The worker owns the handle. Subscription attempts and reassembly belong
 * to the port's serial callback, until stop joins it. */
static struct {
    xiaotai_mqtt_t *client;
    bool started, connected, waiting, tick_started;
    atomic_uint epoch, pending_events, retries;
    atomic_bool ready, watch;
    atomic_int unbind_published;
    int32_t unbind_id, early_unbind_published;
    bool issuing_unbind;
    uint32_t generation, sequence;
    unsigned attempts;
    int32_t ids[2], used_ids[2U * MQTT_ATTEMPTS];
    size_t id_count;
    int64_t deadline, poll_at, heartbeat_at;
    char client_id[XIAOTAI_IDENTITY_DEVICE_ID_MAX + sizeof("sn_")];
    char cmd[XIAOTAI_MQTT_TOPIC_MAX + 1U], notify[XIAOTAI_MQTT_TOPIC_MAX + 1U];
    char ack[XIAOTAI_MQTT_TOPIC_MAX + 1U], up[XIAOTAI_MQTT_TOPIC_MAX + 1U];
} sig;

enum { EVENT_DROP = 1U, EVENT_READY = 2U, EVENT_AUTH = 4U };

static bool receive_generation_current(void)
{
    const uint32_t generation = sig.generation;
    return generation != 0 && generation == atomic_load(&platform.session.receive_generation) &&
        generation == xiaotai_identity_generation();
}

static void forward(unsigned bits)
{
    if (bits != EVENT_READY) atomic_fetch_and(&sig.pending_events, ~EVENT_READY);
    atomic_fetch_or(&sig.pending_events, bits);
    platform_wake();
}

static void forget_attempt(void)
{
    atomic_store(&sig.watch, false);
    atomic_store(&sig.ready, false);
    sig.waiting = false;
    sig.deadline = 0;
    sig.ids[0] = sig.ids[1] = 0;
}

static void forget_connection(void)
{
    forget_attempt();
    sig.connected = false;
    sig.attempts = 0;
    sig.id_count = 0;
}

static void fail_attempt(int64_t now)
{
    forget_attempt();
    if (sig.attempts < MQTT_ATTEMPTS) {
        sig.deadline = now + MQTT_RETRY_US;
        atomic_store(&sig.watch, true);
    }
}

static void subscribe(int64_t now)
{
    const xiaotai_mqtt_topic_t topics[] = {{sig.cmd, 1}, {sig.notify, 1}};
    const size_t count = platform.config.subscribe_per_topic ? 2U : 1U;
    forget_attempt();
    if (sig.attempts++) atomic_fetch_add(&sig.retries, 1U);
    bool valid = true;
    for (size_t i = 0; i < count; ++i) {
        int32_t id = 0;
        if (xiaotai_mqtt_subscribe(sig.client, topics + i, count == 2U ? 1U : 2U, &id) != XIAOTAI_OK || id <= 0) {
            valid = false;
            continue;
        }
        for (size_t j = 0; j < sig.id_count; ++j) if (sig.used_ids[j] == id) valid = false;
        sig.used_ids[sig.id_count++] = id;
        sig.ids[i] = id;
    }
    if (!valid) { fail_attempt(now); return; }
    sig.waiting = true;
    sig.deadline = now + (int64_t)platform.config.subscribe_timeout_s * 1000000;
    atomic_store(&sig.watch, true);
}

static void subscribed(const xiaotai_mqtt_event_t *event, int64_t now)
{
    if (!sig.waiting || event->message_id <= 0) return;
    if (now >= sig.deadline) { fail_attempt(now); return; }
    size_t which;
    for (which = 0; which < 2U; ++which) if (sig.ids[which] == event->message_id) break;
    if (which == 2U) return;
    const size_t count = platform.config.subscribe_per_topic ? 1U : 2U;
    if (!event->granted || event->granted_count != count) { fail_attempt(now); return; }
    bool bad = false, refused = false;
    for (size_t i = 0; i < count; ++i) {
        bad |= event->granted[i] > 1U;
        refused |= event->granted[i] == XIAOTAI_MQTT_REFUSED;
    }
    if (refused && platform.config.auth_failure != XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE) {
        forget_attempt();
        forward(EVENT_AUTH);
        return;
    }
    if (bad) { fail_attempt(now); return; }
    sig.ids[which] = 0;
    if (!sig.ids[0] && !sig.ids[1]) {
        sig.waiting = false;
        atomic_store(&sig.watch, false);
        atomic_store(&sig.ready, true);
        forward(EVENT_READY);
    }
}

static void on_mqtt(void *ctx, const xiaotai_mqtt_event_t *event)
{
    if ((uint32_t)(uintptr_t)ctx != atomic_load(&sig.epoch)) return;
    switch (event->kind) {
    case XIAOTAI_MQTT_CONNECTED:
        platform_signals_disconnected();
        forget_connection();
        sig.connected = true;
        subscribe(xiaotai_time_us());
        break;
    case XIAOTAI_MQTT_DISCONNECTED:
        platform_signals_disconnected();
        forget_connection();
        forward(EVENT_DROP);
        break;
    case XIAOTAI_MQTT_SUBSCRIBED:
        subscribed(event, xiaotai_time_us());
        break;
    case XIAOTAI_MQTT_PUBLISHED:
        if (sig.issuing_unbind) sig.early_unbind_published = event->message_id;
        else if (sig.unbind_id > 0 && event->message_id == sig.unbind_id) {
            atomic_store(&sig.unbind_published, event->message_id);
            platform_wake();
        }
        break;
    case XIAOTAI_MQTT_WAKE: {
        const int64_t now = xiaotai_time_us();
        /* Binding can revoke this generation before the old client is stopped. */
        if (sig.connected && atomic_load(&sig.watch) && now >= sig.deadline && receive_generation_current()) {
            if (sig.waiting) fail_attempt(now);
            else subscribe(now);
        }
        break;
    }
    case XIAOTAI_MQTT_ERROR:
        if (event->error == XIAOTAI_MQTT_ERROR_REFUSED &&
            platform.config.auth_failure != XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE) forward(EVENT_AUTH);
        if (event->error == XIAOTAI_MQTT_ERROR_PROTOCOL) {
            platform_signals_disconnected();
            if (sig.waiting && event->message_id > 0 &&
                (event->message_id == sig.ids[0] || event->message_id == sig.ids[1]))
                fail_attempt(xiaotai_time_us());
        }
        break;
    case XIAOTAI_MQTT_DATA:
        platform_signals_message(sig.client, event, sig.cmd, sig.notify, sig.ack, sig.generation,
            receive_generation_current());
        break;
    default:
        break;
    }
}

bool platform_signaling_unbind_issuing(xiaotai_mqtt_t *client)
{
    if (!platform.config.unbind_puback_wait_ms || !client || client != sig.client || sig.unbind_id || sig.issuing_unbind)
        return false;
    sig.early_unbind_published = 0;
    sig.issuing_unbind = true;
    return true;
}

void platform_signaling_unbind_queued(int32_t id)
{
    sig.issuing_unbind = false;
    sig.unbind_id = id;
    if (id > 0 && sig.early_unbind_published == id) atomic_store(&sig.unbind_published, id);
}

bool platform_signaling_unbind_published(int32_t id)
{
    return id > 0 && atomic_load(&sig.unbind_published) == id;
}

static xiaotai_err_t start(void *ctx, uint32_t epoch, const char *broker, const char *device, const char *token)
{
    (void)ctx;
    if (sig.client) return XIAOTAI_ERR_INVALID_STATE;
    const int length = snprintf(sig.client_id, sizeof(sig.client_id), "sn_%s", device);
    if (length <= 0 || (size_t)length >= sizeof(sig.client_id)) return XIAOTAI_ERR_INVALID_SIZE;
    (void)snprintf(sig.cmd, sizeof(sig.cmd), "device/%s/cmd", sig.client_id);
    (void)snprintf(sig.notify, sizeof(sig.notify), "device/%s/notify", sig.client_id);
    (void)snprintf(sig.ack, sizeof(sig.ack), "device/%s/ack", sig.client_id);
    (void)snprintf(sig.up, sizeof(sig.up), "device/%s/up", sig.client_id);
    const xiaotai_mqtt_config_t config = {
        .uri = broker, .client_id = sig.client_id, .username = device, .password = token,
        .keepalive_s = platform.config.keepalive_s, .reconnect_ms = platform.config.reconnect_ms,
        .rx_buffer_bytes = platform.config.signaling_buffer_bytes, .tx_buffer_bytes = platform.config.signaling_buffer_bytes,
        .outbox_limit_bytes = platform.config.signaling_outbox_bytes, .task_stack_bytes = platform.config.signaling_stack_bytes,
        .event_stack_bytes = XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES,
        .handler = on_mqtt, .ctx = (void *)(uintptr_t)epoch,
    };
    sig.generation = atomic_load(&platform.session.receive_generation);
    sig.unbind_id = sig.early_unbind_published = 0;
    sig.issuing_unbind = false;
    atomic_store(&sig.unbind_published, 0);
    forget_connection();
    atomic_store(&sig.pending_events, 0);
    atomic_store(&sig.epoch, epoch);
    xiaotai_err_t err = xiaotai_mqtt_create(&config, &sig.client);
    if (err == XIAOTAI_OK) {
        err = xiaotai_mqtt_start(sig.client);
        sig.started = err == XIAOTAI_OK;
    }
    return err;
}

static xiaotai_err_t destroy(void *ctx)
{
    (void)ctx;
    if (!sig.client) return XIAOTAI_OK;
    const xiaotai_err_t stopped = sig.started ? xiaotai_mqtt_stop(sig.client) : XIAOTAI_OK;
    if (stopped != XIAOTAI_OK) return stopped;
    sig.started = false;
    platform_signals_disconnected();
    atomic_store(&sig.epoch, 0);
    atomic_store(&sig.pending_events, 0);
    forget_connection();
    const xiaotai_err_t err = xiaotai_mqtt_destroy(sig.client);
    if (err == XIAOTAI_OK) sig.client = NULL;
    return err;
}

xiaotai_err_t platform_signaling_default_link(void)
{
    if (!platform.session.link.start && platform.session.enabled) {
        const xiaotai_err_t err = platform_signals_start();
        if (err != XIAOTAI_OK) return err;
        platform.session.link = (platform_session_link_t){.start = start, .destroy = destroy};
    }
    return XIAOTAI_OK;
}

uint32_t platform_signaling_step(void)
{
    if (platform.session.link.start != start) return XIAOTAI_WAIT_FOREVER;
    const unsigned events = atomic_exchange(&sig.pending_events, 0);
    const uint32_t epoch = atomic_load(&sig.epoch);
    if (events & EVENT_DROP) platform_session_link_event(epoch, PLATFORM_LINK_DROPPED);
    if (events & EVENT_AUTH) platform_session_link_event(epoch, PLATFORM_LINK_AUTH_REFUSED);
    else if (events & EVENT_READY) platform_session_link_event(epoch, PLATFORM_LINK_READY);
    const unsigned retries = atomic_exchange(&sig.retries, 0);
    if (retries) {
        platform_lock();
        platform.stats.subscribe_retries += retries;
        platform_unlock();
    }
    const int64_t now = xiaotai_time_us();
    const int64_t interval = (int64_t)platform.config.heartbeat_s * 1000000;
    if (!sig.tick_started) {
        sig.tick_started = true;
        sig.heartbeat_at = platform.config.heartbeat_when_ready_only ? interval : now + interval;
    }
    if (sig.started && atomic_load(&sig.watch) && now >= sig.poll_at && receive_generation_current()) {
        sig.poll_at = now + MQTT_POLL_US;
        (void)xiaotai_mqtt_wake(sig.client);
    }
    if (now >= sig.heartbeat_at) {
        platform_lock();
        const bool up = platform.status.signaling && !platform.status.held && atomic_load(&sig.ready);
        platform_unlock();
        const bool publish = sig.started && up;
        if (publish || !platform.config.heartbeat_when_ready_only) {
            sig.heartbeat_at = now + interval;
            ++sig.sequence;
        }
        if (publish) {
            char body[100];
            const int size = snprintf(body, sizeof(body), "{\"type\":\"heartbeat\",\"seq\":%" PRIu32 ",\"ts\":%" PRId64 "}",
                                      sig.sequence, xiaotai_clock_unix_ms() / 1000);
            if (size > 0 && (size_t)size < sizeof(body) && xiaotai_mqtt_publish(sig.client, sig.up, body, (size_t)size,
                    0, XIAOTAI_MQTT_SEND_QUEUED, NULL) == XIAOTAI_OK) {
                platform_lock();
                ++platform.stats.heartbeats;
                platform_unlock();
            }
        }
    }
    return 1000U;
}

void platform_signaling_reset(void)
{
    if (sig.client) return; /* A failed stop still owns the handle. */
    sig.tick_started = false;
    sig.sequence = 0;
    sig.poll_at = sig.heartbeat_at = 0;
    atomic_store(&sig.retries, 0);
}

bool platform_signaling_heap_reaped(void)
{
#ifdef XIAOTAI_PLATFORM_MQTT_STOP_FREES_WORKER
    /* The host worker frees its accounted allocation before clearing the
     * worker pointer that xiaotai_mqtt_stop waits on. */
    return platform.session.link.start == start;
#else
    /* An exit fence alone does not establish that task memory was reaped. */
    return false;
#endif
}
