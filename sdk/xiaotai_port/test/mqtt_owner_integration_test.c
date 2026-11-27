// SPDX-License-Identifier: MIT
#include "mqtt_client_priv.h"
#include "mqtt_owner_bridge.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "mqtt_client";
esp_event_base_t MQTT_EVENTS = "mqtt";
static const int STOPPED_BIT = 1, RECONNECT_BIT = 2, DISCONNECT_BIT = 4;
static _Thread_local int task_identity;
static pthread_mutex_t condition_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned polls, allow_poll, waits, stop_waits, error_events, disconnect_events, wake_events, native_posts;
static bool failed_write;
static bool reentry_case, connected_returned, reentry_submitted;
static unsigned callback_depth, max_callback_depth, write_calls;
static int fault_code, next_error;
static int poll_result;
static esp_err_t (*receive_hook)(esp_mqtt_client_handle_t native);
static void (*connect_hook)(void);
static bool fail_transport;
static char order[128];
static size_t order_size;
static pthread_t owner_thread;
static esp_mqtt_client_handle_t fixture_native;
static xt_mqtt_owner_bridge_t bridge;

struct host_group { pthread_mutex_t lock; pthread_cond_t changed; unsigned bits; };
#if XIAOTAI_OWNER_REAL_LIFECYCLE
#define XIAOTAI_MQTT_LIFECYCLE_IMPLEMENTATION
#include "mqtt_owner_lifecycle_provider.h"
#undef XIAOTAI_MQTT_LIFECYCLE_IMPLEMENTATION
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &task_identity; }

unsigned xEventGroupSetBits(EventGroupHandle_t handle, unsigned bits)
{
    struct host_group *group = handle;
    assert(!pthread_mutex_lock(&group->lock));
    group->bits |= bits;
    unsigned value = group->bits;
    assert(!pthread_cond_broadcast(&group->changed) && !pthread_mutex_unlock(&group->lock));
    return value;
}
unsigned xEventGroupClearBits(EventGroupHandle_t handle, unsigned bits)
{
    struct host_group *group = handle;
    assert(!pthread_mutex_lock(&group->lock));
    unsigned old = group->bits;
    group->bits &= ~bits;
    assert(!pthread_mutex_unlock(&group->lock));
    return old;
}
unsigned xEventGroupGetBits(EventGroupHandle_t handle)
{
    struct host_group *group = handle;
    assert(!pthread_mutex_lock(&group->lock));
    unsigned value = group->bits;
    assert(!pthread_mutex_unlock(&group->lock));
    return value;
}
unsigned xEventGroupWaitBits(EventGroupHandle_t handle, unsigned bits, bool clear, bool all, int ticks)
{
    struct host_group *group = handle;
    if (bits == XT_MQTT_OWNER_READY || bits == 1U) {
        assert(!pthread_mutex_lock(&condition_lock));
        if (bits == XT_MQTT_OWNER_READY) ++waits;
        else ++stop_waits;
        assert(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&condition_lock));
    }
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_nsec += (ticks == portMAX_DELAY ? 3000 : ticks) * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    assert(!pthread_mutex_lock(&group->lock));
    while (!(all ? (group->bits & bits) == bits : (group->bits & bits)) && ticks) {
        int rc = pthread_cond_timedwait(&group->changed, &group->lock, &deadline);
        assert(rc == 0 || rc == ETIMEDOUT);
        if (rc == ETIMEDOUT) { assert(ticks != portMAX_DELAY); break; }
    }
    unsigned value = group->bits;
    if (clear && (all ? (value & bits) == bits : (value & bits))) group->bits &= ~bits;
    assert(!pthread_mutex_unlock(&group->lock));
    return value;
}

#if XIAOTAI_OWNER_REAL_LIFECYCLE
static atomic_int_fast64_t fixture_tick = 1;
static int64_t platform_tick_get_ms(void) { return atomic_load(&fixture_tick); }
#else
static int64_t platform_tick_get_ms(void) { return 1; }
#endif
static esp_err_t esp_mqtt_dispatch_event(esp_mqtt_client_handle_t native);
static esp_err_t esp_mqtt_dispatch_event_with_msgid(esp_mqtt_client_handle_t native)
{
    return esp_mqtt_dispatch_event(native);
}
static void esp_mqtt_client_dispatch_transport_error(esp_mqtt_client_handle_t native);
static int esp_transport_write(esp_transport_handle_t transport, const char *data, int size, int timeout)
{
    assert(transport && data && size && timeout >= 0);
    ++write_calls;
    if (failed_write) {
        fixture_native->event.error_handle->esp_transport_sock_errno = fault_code;
        return -1;
    }
    return size;
}
static int esp_transport_close(esp_transport_handle_t transport) { return transport ? 0 : -1; }
static int esp_transport_get_default_port(esp_transport_handle_t transport) { assert(transport); return 1883; }
static void esp_transport_tcp_set_keep_alive(esp_transport_handle_t transport, const void *config)
{
    (void)transport; (void)config;
}
static esp_transport_handle_t esp_transport_list_get_transport(esp_transport_list_handle_t list, const char *scheme)
{
    (void)list; (void)scheme; return fixture_native->config->transport;
}
static esp_err_t esp_mqtt_client_create_transport(esp_mqtt_client_handle_t native)
{
    assert(native);
    if (fail_transport) return ESP_FAIL;
    native->config->transport = (void *)native;
    return ESP_OK;
}
static int esp_transport_connect(esp_transport_handle_t transport, const char *host, int port, int ms)
{
    assert(transport && host && port == 1883 && ms == fixture_native->config->network_timeout_ms);
    if (connect_hook) connect_hook();
    return 0;
}
static esp_err_t esp_mqtt_connect(esp_mqtt_client_handle_t native, int ms)
{
    assert(native && ms == fixture_native->config->network_timeout_ms); return ESP_OK;
}
static int max_poll_timeout(esp_mqtt_client_handle_t native, int ms) { assert(native); return ms; }
static int esp_transport_poll_read(esp_transport_handle_t transport, int timeout)
{
    assert(transport && timeout == MQTT_POLL_READ_TIMEOUT_MS);
    assert(!pthread_mutex_lock(&condition_lock));
    unsigned current = ++polls;
    assert(!pthread_cond_broadcast(&condition));
    while (allow_poll < current) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
    int result = poll_result;
    poll_result = 0;
    return result;
}
static void mqtt_delete_expired_messages(esp_mqtt_client_handle_t native) { assert(native); }
static esp_err_t mqtt_process_receive(esp_mqtt_client_handle_t native)
{
    assert(native);
    return receive_hook ? receive_hook(native) : ESP_OK;
}
static bool has_timed_out(uint64_t start, int timeout)
{
#if XIAOTAI_OWNER_REAL_LIFECYCLE
    return (int64_t)(start + (uint64_t)timeout - (uint64_t)platform_tick_get_ms()) <= 0;
#else
    (void)start; (void)timeout;
    return false;
#endif
}
static esp_err_t process_keepalive(esp_mqtt_client_handle_t native) { assert(native); return ESP_OK; }
#ifdef XIAOTAI_MQTT_NATIVE_QUEUE
static esp_err_t mqtt_resend_queued(esp_mqtt_client_handle_t native, outbox_item_handle_t item);
#else
static esp_err_t mqtt_resend_queued(esp_mqtt_client_handle_t native, outbox_item_handle_t item)
{
    assert(native && item);
    return ESP_FAIL; /* No fixture publication is QUEUED while the worker runs. */
}
#endif
static esp_err_t mqtt_resend_pubrel(esp_mqtt_client_handle_t native, outbox_item_handle_t item)
{
    assert(native && item); return ESP_FAIL;
}
static esp_err_t esp_event_post_to(esp_event_loop_handle_t loop, esp_event_base_t base, int event,
                                    const void *data, size_t size, int ticks)
{
    (void)loop; (void)base; (void)event; (void)data; (void)size; (void)ticks;
    ++native_posts;
    return ESP_FAIL;
}
static esp_err_t esp_event_loop_run(esp_event_loop_handle_t loop, int ticks)
{
    (void)loop; (void)ticks; return ESP_OK;
}
static void vTaskDelete(void *handle) { assert(!handle); pthread_exit(NULL); }

/* These are the exact native bodies, with the small owner hook transformation
 * applied to dispatch, synchronous admission, waits, task entry and exit. */
#if XIAOTAI_OWNER_REAL_LIFECYCLE
static void esp_transport_destroy(esp_transport_handle_t transport) { (void)transport; }
static void esp_transport_list_destroy(esp_transport_list_handle_t list) { (void)list; assert(false); }
static char *platform_create_id_string(void) { assert(false); return NULL; }
#define malloc fixture_lifecycle_malloc
#define calloc fixture_lifecycle_calloc
#define strdup fixture_lifecycle_strdup
#define free fixture_lifecycle_free
#define heap_caps_calloc fixture_lifecycle_caps_calloc
#endif
#include "mqtt_owner_native.inc"
#if XIAOTAI_OWNER_REAL_LIFECYCLE
#undef malloc
#undef calloc
#undef strdup
#undef free
#undef heap_caps_calloc
#endif

static void *native_task(void *ctx) { esp_mqtt_task(ctx); return NULL; }
static void release_poll(unsigned number)
{
    assert(!pthread_mutex_lock(&condition_lock));
    allow_poll = number;
    assert(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&condition_lock));
}
static void wait_for(unsigned *value, unsigned minimum)
{
    struct timespec limit;
    assert(!clock_gettime(CLOCK_REALTIME, &limit));
    limit.tv_sec += 3;
    assert(!pthread_mutex_lock(&condition_lock));
    while (*value < minimum) {
        int rc = pthread_cond_timedwait(&condition, &condition_lock, &limit);
        if (rc) fprintf(stderr, "wait target=%s current=%u need=%u polls=%u allow=%u ready=%u stop=%u state=%d run=%d order=%s\n",
                        value == &polls ? "poll" : value == &waits ? "ready" : "stop", *value, minimum,
                        polls, allow_poll, waits, stop_waits, fixture_native ? (int)fixture_native->state : -1,
                        fixture_native ? fixture_native->run : 0, order);
        assert(!rc);
    }
    assert(!pthread_mutex_unlock(&condition_lock));
}
static void observer(void *ctx, esp_event_base_t base, int32_t kind, void *data)
{
    assert(ctx == &bridge && base == MQTT_EVENTS && pthread_equal(pthread_self(), owner_thread));
    ++callback_depth;
    if (callback_depth > max_callback_depth) max_callback_depth = callback_depth;
    assert(callback_depth == 1);
    const esp_mqtt_event_t *event = data;
    assert(event->client == fixture_native && event->event_id == kind);
    char seen = '?';
    if (kind == MQTT_EVENT_BEFORE_CONNECT) seen = 'B';
    if (kind == MQTT_EVENT_CONNECTED) seen = 'C';
    if (kind == MQTT_EVENT_ERROR) {
        assert(event->error_handle && event->error_handle->esp_transport_sock_errno == next_error);
        next_error += 110;
        ++error_events; seen = 'E';
    }
    if (kind == MQTT_EVENT_DISCONNECTED) { ++disconnect_events; seen = 'D'; }
    if (kind == MQTT_USER_EVENT) { ++wake_events; seen = 'W'; }
    assert(order_size + 1 < sizeof(order));
    order[order_size++] = seen;
    order[order_size] = 0;
    if (reentry_case && kind == MQTT_EVENT_CONNECTED && !connected_returned) {
        failed_write = true;
        fault_code = next_error = 110;
        const esp_mqtt_topic_t topic = {.filter = "fixture", .qos = 1};
        assert(esp_mqtt_client_subscribe_multiple(fixture_native, &topic, 1) < 0);
        assert(bridge.count == 1 && !error_events);
        const unsigned attempts = write_calls;
        const uint16_t id = fixture_native->mqtt_state.connection.last_message_id;
        const uint64_t bytes = outbox_get_size(fixture_native->outbox);
        for (unsigned i = 0; i < 4; ++i) {
            assert(esp_mqtt_client_subscribe_multiple(fixture_native, &topic, 1) < 0);
            assert(write_calls == attempts && bridge.count == 1 && !error_events);
            assert(fixture_native->mqtt_state.connection.last_message_id == id && outbox_get_size(fixture_native->outbox) == bytes);
        }
        failed_write = false;
        connected_returned = true;
    } else if (reentry_case && kind == MQTT_EVENT_ERROR && !reentry_submitted) {
        assert(connected_returned);
        const esp_mqtt_topic_t topic = {.filter = "after-return", .qos = 1};
        if (error_events < 3) {
            failed_write = true;
            fault_code = next_error;
            assert(esp_mqtt_client_subscribe_multiple(fixture_native, &topic, 1) < 0);
        } else {
            failed_write = false;
            assert(esp_mqtt_client_subscribe_multiple(fixture_native, &topic, 1) > 0);
            reentry_submitted = true;
        }
    }
    --callback_depth;
}
static void *subscribe_failed(void *ctx)
{
    (void)ctx;
    const esp_mqtt_topic_t topic = {.filter = "fixture", .qos = 1};
    assert(esp_mqtt_client_subscribe_multiple(fixture_native, &topic, 1) < 0);
    return NULL;
}
static void *stop_client(void *ctx)
{
    (void)ctx;
    assert(esp_mqtt_client_stop(fixture_native) == ESP_OK);
    return NULL;
}

int main(int argc, char **argv)
{
    fixture_native = calloc(1, sizeof(*fixture_native));
    assert(fixture_native);
    reentry_case = argc > 1 && !strcmp(argv[1], "reentry");
    pthread_mutexattr_t attr;
    pthread_mutex_t api;
    assert(!pthread_mutexattr_init(&attr) && !pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&api, &attr) && !pthread_mutexattr_destroy(&attr));
    struct host_group group = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
    mqtt_config_storage_t config = {.transport = (void *)fixture_native, .port = 1883, .host = "fixture",
        .network_timeout_ms = 1000, .reconnect_timeout_ms = 20};
    esp_mqtt_error_codes_t error = {0};
    uint8_t input[4] = {0};
    fixture_native->api_lock = &api; fixture_native->status_bits = &group;
    fixture_native->config = &config; fixture_native->event.error_handle = &error;
    fixture_native->mqtt_state.in_buffer = input;
    fixture_native->mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    fixture_native->outbox = outbox_init();
    assert(fixture_native->outbox && mqtt_msg_buffer_init(&fixture_native->mqtt_state.connection, 64) == ESP_OK);
    xt_mqtt_owner_prepare(fixture_native, &bridge, observer, &bridge);
    assert(xt_mqtt_owner_wake(fixture_native)); /* Before task entry: no fake event and no queue slot. */
    assert(!pthread_create(&owner_thread, NULL, native_task, fixture_native));
    wait_for(&polls, 1);
    if (reentry_case) {
        assert(!strcmp(order, "WBCEEE") && reentry_submitted && error_events == 3 && max_callback_depth == 1);
        pthread_t stopper;
        assert(!pthread_create(&stopper, NULL, stop_client, NULL));
        release_poll(UINT32_MAX);
        assert(!pthread_join(stopper, NULL) && !pthread_join(owner_thread, NULL));
        assert(!outbox_get_size(fixture_native->outbox) && !xt_mqtt_owner_wake(fixture_native));
        mqtt_msg_buffer_destroy(&fixture_native->mqtt_state.connection);
        outbox_destroy(fixture_native->outbox);
        assert(!pthread_mutex_destroy(&api));
        assert(!pthread_mutex_destroy(&group.lock) && !pthread_cond_destroy(&group.changed));
        free(fixture_native);
        fixture_native = NULL;
        puts("PASS: native handler depth one, pre-I/O capacity rejection, causal completion and fresh retry");
        return 0;
    }
    assert(!strcmp(order, "WBC") && wake_events == 1 && !native_posts);

    failed_write = true; fault_code = next_error = 110;
    subscribe_failed(NULL);
    assert(bridge.count == 1 && fixture_native->state == MQTT_STATE_CONNECTED && !error_events);
    /* A second subscribe must wait rather than overrun a bounded error bridge. */
    fault_code = 220;
    pthread_t second;
    assert(!pthread_create(&second, NULL, subscribe_failed, NULL));
    wait_for(&waits, 1);
    for (unsigned i = 0; i < 1000; ++i) assert(xt_mqtt_owner_wake(fixture_native));
    release_poll(1);
    wait_for(&polls, 2);
    assert(!pthread_join(second, NULL));
    assert(bridge.count == 1 && error_events == 1 && wake_events == 2);
    error.esp_transport_sock_errno = 999; /* Prove pending records own their error scalar. */
    release_poll(2);
    wait_for(&polls, 3);
    assert(error_events == 2 && bridge.count == 0);

    fault_code = 330;
    assert(esp_mqtt_client_publish(fixture_native, "a", "x", 1, 0, 0) < 0);
    assert(bridge.count == 2 && error_events == 2 && !disconnect_events);
    error.esp_transport_sock_errno = 999;
    xEventGroupSetBits(fixture_native->status_bits, RECONNECT_BIT);
    release_poll(3);
    wait_for(&polls, 4);
    assert(error_events == 3 && disconnect_events == 1 && !strcmp(order, "WBCEWEEDBC"));
    assert(!native_posts);
    assert(max_callback_depth == 1);

    failed_write = false;
    assert(outbox_get_size(fixture_native->outbox) > 0);
    pthread_t stopper;
    assert(!pthread_create(&stopper, NULL, stop_client, NULL));
    /* The native socket wait remains native; completing it is a fixture action. */
    release_poll(UINT32_MAX);
    assert(!pthread_join(stopper, NULL) && !pthread_join(owner_thread, NULL));
    assert(!outbox_get_size(fixture_native->outbox) && !xt_mqtt_owner_wake(fixture_native));
    mqtt_msg_buffer_destroy(&fixture_native->mqtt_state.connection);
    outbox_destroy(fixture_native->outbox);
    assert(!pthread_mutex_destroy(&api));
    assert(!pthread_mutex_destroy(&group.lock) && !pthread_cond_destroy(&group.changed));
    free(fixture_native);
    fixture_native = NULL;
    puts("PASS: actual native task owner hooks, bounded causal error bridge, independent wake and pending stop");
    return 0;
}
