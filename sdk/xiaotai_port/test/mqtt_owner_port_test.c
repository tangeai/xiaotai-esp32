// SPDX-License-Identifier: MIT
/* The network/RTOS provider is shared with the earlier native-task proof. Its
 * native task, sync writes, event dispatch and stop bodies remain hash-pinned.
 * This fixture adds the actual port wrapper and the opaque native owner API. */
#define main native_only_fixture_main
#include "mqtt_owner_integration_test.c"
#undef main
#include "xiaotai/port/mqtt.h"
#include "xiaotai_mem_host.h"
#include "mqtt_owner_api.h"
#include "esp_heap_caps.h"

static bool fail_bridge, fail_task, hold_task, task_created, release_task, task_joinable;
static unsigned native_destroyed, bridge_allocated, bridge_freed;
static void (*task_entry)(void *);
static void *task_arg;
static xiaotai_mqtt_t *port_client;
static unsigned port_events[7], translated, retry_submitted;
static bool callback_reentry, closing_callback;
static int32_t last_sub_id;
static uint32_t previous_epoch;
static _Thread_local bool defer_api;
static bool api_deferred, allow_api;
static bool connecting, allow_connect;

void fixture_before_api_lock(SemaphoreHandle_t lock, int ticks)
{
    (void)lock; (void)ticks;
    if (!defer_api) return;
    defer_api = false;
    assert(!pthread_mutex_lock(&condition_lock));
    api_deferred = true;
    assert(!pthread_cond_broadcast(&condition));
    while (!allow_api) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    if (caps == MALLOC_CAP_DEFAULT) return fixture_lifecycle_calloc(count, size);
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (fail_bridge) return NULL;
    void *memory = fixture_lifecycle_calloc(count, size);
    if (memory) ++bridge_allocated;
    return memory;
}
void heap_caps_free(void *memory) { assert(memory); ++bridge_freed; fixture_lifecycle_free(memory); }

/* These observation wrappers preserve the complete generated native bodies. */
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    assert(!fixture_native);
    fixture_native = fixture_real_native_init(cfg);
    if (fixture_native) fixture_lifecycle_mark_tx(fixture_native->mqtt_state.connection.buffer);
    return fixture_native;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t native)
{
    assert(native && !native->run && !task_joinable);
    const bool current = native == fixture_native;
    esp_err_t rc = fixture_real_native_destroy(native);
    if (rc == ESP_OK) {
        if (current) fixture_native = NULL;
        ++native_destroyed;
    }
    return rc;
}

static void *created_task(void *arg)
{
    (void)arg;
    assert(!pthread_mutex_lock(&condition_lock));
    owner_thread = pthread_self();
    fixture_native->task_handle = xTaskGetCurrentTaskHandle();
    task_created = true;
    assert(!pthread_cond_broadcast(&condition));
    while (hold_task && !release_task) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
    task_entry(task_arg);
    return NULL;
}
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
                            void *arg, unsigned priority, TaskHandle_t *handle, int core)
{
    (void)core;
    assert(!strcmp(name, "mqtt_task") && stack == 6144 && priority == (unsigned)fixture_native->config->task_prio && !task_joinable);
    if (fail_task) return 0;
    task_entry = entry; task_arg = arg;
    *handle = NULL;
    task_created = release_task = false;
    assert(!pthread_create(&owner_thread, NULL, created_task, NULL));
    task_joinable = true;
    return pdTRUE;
}

#include "mqtt_owner_api.inc"

static void join_task(void)
{
    assert(task_joinable && !pthread_join(owner_thread, NULL));
    task_joinable = false;
}
static void check_native_balanced(void)
{
    const fixture_lifecycle_stats_t memory = fixture_lifecycle_snapshot();
    assert(!memory.live && !memory.live_bytes && !memory.dirty);
    assert(memory.mutex_created == memory.mutex_deleted && memory.group_created == memory.group_deleted);
    assert(memory.loop_created == memory.loop_deleted);
}
static void hold_connect(void)
{
    assert(!pthread_mutex_lock(&condition_lock));
    connecting = true;
    assert(!pthread_cond_broadcast(&condition));
    while (!allow_connect) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
}
static void port_handler(void *ctx, const xiaotai_mqtt_event_t *event)
{
    assert(ctx == &port_client && pthread_equal(pthread_self(), owner_thread));
    assert(xiaotai_idf_mqtt_owner_current(fixture_native));
    assert(++callback_depth == 1);
    if (callback_depth > max_callback_depth) max_callback_depth = callback_depth;
    ++port_events[event->kind];
    char seen = "CDSPTEW"[event->kind];
    assert(order_size + 1 < sizeof(order));
    order[order_size++] = seen; order[order_size] = 0;
    /* All management rejection happens before the management mutex, even
     * while another task holds it waiting for this callback to return. */
    assert(xiaotai_mqtt_start(port_client) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mqtt_stop(port_client) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_ERR_INVALID_STATE);
    if (event->kind == XIAOTAI_MQTT_CONNECTED && callback_reentry) {
        const xiaotai_mqtt_topic_t topic = {"fixture", 1};
        int32_t id = 99;
        failed_write = true; fault_code = 110;
        assert(xiaotai_mqtt_subscribe(port_client, &topic, 1, &id) == XIAOTAI_FAIL && !id);
        xt_mqtt_owner_bridge_t *owner = xt_mqtt_owner_bridge(fixture_native);
        assert(owner->count == 1 && !port_events[XIAOTAI_MQTT_ERROR]);
        const unsigned writes = write_calls;
        const uint16_t packet = fixture_native->mqtt_state.connection.last_message_id;
        const uint64_t bytes = outbox_get_size(fixture_native->outbox);
        for (unsigned i = 0; i < 4; ++i) {
            assert(xiaotai_mqtt_subscribe(port_client, &topic, 1, &id) == XIAOTAI_FAIL && !id);
            assert(write_calls == writes && owner->count == 1 && fixture_native->mqtt_state.connection.last_message_id == packet);
            assert(outbox_get_size(fixture_native->outbox) == bytes);
        }
        failed_write = false;
    }
    if (event->kind == XIAOTAI_MQTT_ERROR) {
        assert(event->error == XIAOTAI_MQTT_ERROR_TRANSPORT && event->detail == fault_code);
        if (callback_reentry && !retry_submitted) {
            const xiaotai_mqtt_topic_t topic = {"after-return", 1};
            assert(xiaotai_mqtt_subscribe(port_client, &topic, 1, &last_sub_id) == XIAOTAI_OK && last_sub_id > 0);
            ++retry_submitted;
        }
        if (closing_callback) {
            int32_t id = 99;
            const unsigned writes = write_calls;
            assert(xiaotai_mqtt_publish(port_client, "a", "x", 1, 0, XIAOTAI_MQTT_SEND_NOW, &id)
                   == XIAOTAI_ERR_INVALID_STATE && !id && writes == write_calls);
        }
    }
    if (event->kind == XIAOTAI_MQTT_SUBSCRIBED) {
        assert(event->message_id == last_sub_id && event->granted_count == 2 &&
               event->granted[0] == 1 && event->granted[1] == 0x80);
        ++translated;
    }
    if (event->kind == XIAOTAI_MQTT_PUBLISHED) { assert(event->message_id == 123); ++translated; }
    if (event->kind == XIAOTAI_MQTT_DATA) {
        assert(event->message_id == 77 && event->topic_size == 4 && !memcmp(event->topic, "test", 4));
        assert(event->total == 3 && event->qos == 1 && event->duplicate && event->retained);
        assert(event->offset == (translated == 2 ? 0U : 1U));
        assert(event->size == (translated == 2 ? 1U : 2U));
        ++translated;
    }
    --callback_depth;
}
static xiaotai_mqtt_config_t port_config(void)
{
    return (xiaotai_mqtt_config_t){.uri = "mqtt://fixture", .client_id = "owner-port",
        .username = "owner-user", .password = "owner-pass",
        .handler = port_handler, .ctx = &port_client, .reconnect_ms = 20};
}
static void reset_observation(void)
{
    memset(port_events, 0, sizeof(port_events));
    polls = allow_poll = waits = 0;
    stop_waits = 0;
    order_size = 0; order[0] = 0;
    callback_depth = max_callback_depth = 0;
    callback_reentry = closing_callback = failed_write = false;
    fail_transport = false;
    hold_task = false;
}
static void *start_port(void *arg)
{
    xiaotai_err_t *result = arg;
    *result = xiaotai_mqtt_start(port_client);
    return NULL;
}
static void *stop_port(void *arg)
{
    xiaotai_err_t *result = arg;
    *result = xiaotai_mqtt_stop(port_client);
    return NULL;
}
static void *subscribe_port(void *arg)
{
    xiaotai_err_t *result = arg;
    const xiaotai_mqtt_topic_t topic = {"foreign", 1};
    int32_t id = 99;
    *result = xiaotai_mqtt_subscribe(port_client, &topic, 1, &id);
    assert(*result == XIAOTAI_FAIL && !id);
    return NULL;
}
static void *subscribe_before_stop(void *arg)
{
    defer_api = true;
    const xiaotai_mqtt_topic_t topic = {"late", 1};
    int32_t id = 99;
    *(xiaotai_err_t *)arg = xiaotai_mqtt_subscribe(port_client, &topic, 1, &id);
    assert(*(xiaotai_err_t *)arg == XIAOTAI_ERR_INVALID_STATE && !id);
    return NULL;
}
static esp_err_t translate_on_owner(esp_mqtt_client_handle_t native)
{
    receive_hook = NULL;
    const uint8_t codes[] = {1, 0x80};
    esp_mqtt_error_codes_t *error = native->event.error_handle;
    native->event = (esp_mqtt_event_t){.event_id = MQTT_EVENT_SUBSCRIBED, .msg_id = last_sub_id,
        .data = (char *)codes, .data_len = sizeof(codes), .error_handle = error};
    assert(esp_mqtt_dispatch_event(native) == ESP_OK);
    native->event.event_id = MQTT_EVENT_PUBLISHED; native->event.msg_id = 123;
    assert(esp_mqtt_dispatch_event(native) == ESP_OK);
    native->event = (esp_mqtt_event_t){.event_id = MQTT_EVENT_DATA, .topic = "test", .topic_len = 4,
        .msg_id = 77, .total_data_len = 3, .qos = 1, .dup = true, .retain = true,
        .data = "X", .data_len = 1, .error_handle = error};
    assert(esp_mqtt_dispatch_event(native) == ESP_OK);
    native->event.topic = NULL; native->event.topic_len = 0;
    native->event.data = "\0Z"; native->event.data_len = 2; native->event.current_data_offset = 1;
    assert(esp_mqtt_dispatch_event(native) == ESP_OK);
    return ESP_OK;
}

int main(void)
{
    xiaotai_mem_host_clear();
    fixture_lifecycle_begin(0);
    xiaotai_mqtt_config_t cfg = port_config();
    fail_bridge = true;
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_ERR_NO_MEM && !port_client);
    assert(native_destroyed == 1 && !fixture_native && !bridge_allocated);
    check_native_balanced();
    fail_bridge = false;
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_OK);
    assert(bridge_allocated == 1);
    fail_task = true;
    assert(xiaotai_mqtt_start(port_client) == XIAOTAI_FAIL && !fixture_native->run);
    assert(xiaotai_mqtt_wake(port_client) == XIAOTAI_ERR_INVALID_STATE);
    fail_task = false;
    previous_epoch = xt_mqtt_owner_bridge(fixture_native)->epoch;

    /* Hold the native task before its first instruction. start waits only for
     * task entry; stop races it through the port management lock. */
    hold_task = true;
    xiaotai_err_t started = 999, stopped = 999;
    pthread_t starter, stopper;
    assert(!pthread_create(&starter, NULL, start_port, &started));
    assert(!pthread_mutex_lock(&condition_lock));
    while (!task_created) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
    assert(started == 999 && !fixture_native->run);
    assert(xt_mqtt_owner_bridge(fixture_native)->epoch != previous_epoch);
    assert(xiaotai_mqtt_wake(port_client) == XIAOTAI_OK);
    assert(!pthread_create(&stopper, NULL, stop_port, &stopped));
    release_poll(UINT32_MAX);
    assert(!pthread_mutex_lock(&condition_lock));
    release_task = true;
    assert(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&condition_lock));
    assert(!pthread_join(starter, NULL) && !pthread_join(stopper, NULL));
    assert(started == XIAOTAI_OK && stopped == XIAOTAI_OK);
    join_task();
    assert(!xt_mqtt_owner_wake(fixture_native) && !fixture_native->run && !outbox_get_size(fixture_native->outbox));
    unsigned after_stop = 0;
    for (unsigned i = 0; i < 7; ++i) after_stop += port_events[i];
    assert(xiaotai_mqtt_wake(port_client) == XIAOTAI_ERR_INVALID_STATE);
    assert(!native_posts);

    reset_observation();
    callback_reentry = true;
    previous_epoch = xt_mqtt_owner_bridge(fixture_native)->epoch;
    assert(xiaotai_mqtt_start(port_client) == XIAOTAI_OK);
    wait_for(&polls, 1);
    assert(xt_mqtt_owner_bridge(fixture_native)->epoch != previous_epoch);
    assert(!strcmp(order, "CE") && retry_submitted == 1 && max_callback_depth == 1);
    assert(!xt_mqtt_mailbox_take(&xt_mqtt_owner_bridge(fixture_native)->wake, previous_epoch));
    for (unsigned i = 0; i < 1000; ++i) assert(xiaotai_mqtt_wake(port_client) == XIAOTAI_OK);
    receive_hook = translate_on_owner;
    release_poll(1);
    wait_for(&polls, 2);
    assert(!strcmp(order, "CEWSPTT") && translated == 4 && port_events[XIAOTAI_MQTT_WAKE] == 1);

    /* These calls cross the real wrapper, typed boundary, shared ID allocator
     * and original outbox. The actual native task sends/deletes queued QoS0. */
    int32_t id = 99;
    const unsigned queued_writes = write_calls;
    const uint64_t queued_bytes = outbox_get_size(fixture_native->outbox);
    assert(xiaotai_mqtt_publish(port_client, "a", NULL, 0, 0, XIAOTAI_MQTT_SEND_QUEUED, &id) == XIAOTAI_OK && !id);
    assert(write_calls == queued_writes && outbox_get_size(fixture_native->outbox) == queued_bytes + 5U);
    release_poll(2);
    wait_for(&polls, 3);
    assert(write_calls == queued_writes + 1 && outbox_get_size(fixture_native->outbox) == queued_bytes);
    assert(xiaotai_mqtt_publish(port_client, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_NOW, &id) == XIAOTAI_OK && id > 0);
    assert(outbox_get(fixture_native->outbox, id) && port_events[XIAOTAI_MQTT_PUBLISHED] == 1);

    callback_reentry = false;
    failed_write = true; fault_code = 220;
    xiaotai_err_t first = 999, second = 999;
    assert(!pthread_create(&starter, NULL, subscribe_port, &first) && !pthread_join(starter, NULL));
    assert(first == XIAOTAI_FAIL && port_events[XIAOTAI_MQTT_ERROR] == 1);
    assert(!pthread_create(&starter, NULL, subscribe_port, &second));
    wait_for(&waits, 1);
    release_poll(3);
    wait_for(&polls, 4);
    assert(!pthread_join(starter, NULL) && second == XIAOTAI_FAIL);
    fixture_native->event.error_handle->esp_transport_sock_errno = 999;
    release_poll(4);
    wait_for(&polls, 5);
    assert(port_events[XIAOTAI_MQTT_ERROR] == 3 && max_callback_depth == 1);

    /* Foreign publish produces ERROR then DISCONNECTED, both copied before
     * the native error box is overwritten, and reconnect is still the owner. */
    fault_code = 330;
    const uint64_t before_failed_publish = outbox_get_size(fixture_native->outbox);
    assert(xiaotai_mqtt_publish(port_client, "a", "x", 1, 1, XIAOTAI_MQTT_SEND_NOW, &id) == XIAOTAI_FAIL && !id);
    assert(outbox_get_size(fixture_native->outbox) == before_failed_publish);
    assert(xt_mqtt_owner_bridge(fixture_native)->count == 2 && port_events[XIAOTAI_MQTT_ERROR] == 3);
    const xiaotai_mqtt_topic_t offline_topic = {"offline", 1};
    int32_t offline_id = 99;
    const unsigned offline_writes = write_calls;
    const uint16_t offline_packet = fixture_native->mqtt_state.connection.last_message_id;
    const xiaotai_err_t offline = xiaotai_mqtt_subscribe(port_client, &offline_topic, 1, &offline_id);
    if (offline != XIAOTAI_ERR_INVALID_STATE)
        fprintf(stderr, "native disconnected before owner notification: subscribe rc=%d id=%d\n", offline, offline_id);
    assert(offline == XIAOTAI_ERR_INVALID_STATE && !offline_id);
    assert(write_calls == offline_writes && fixture_native->mqtt_state.connection.last_message_id == offline_packet &&
           outbox_get_size(fixture_native->outbox) == before_failed_publish &&
           xt_mqtt_owner_bridge(fixture_native)->count == 2 && port_events[XIAOTAI_MQTT_ERROR] == 3);
    fixture_native->event.error_handle->esp_transport_sock_errno = 999;
    atomic_fetch_add(&fixture_tick, fixture_native->config->reconnect_timeout_ms + 1);
    xEventGroupSetBits(fixture_native->status_bits, RECONNECT_BIT);
    failed_write = false;
    release_poll(5);
    wait_for(&polls, 6);
    assert(port_events[XIAOTAI_MQTT_ERROR] == 4 && port_events[XIAOTAI_MQTT_DISCONNECTED] == 1);
    assert(!strcmp(order, "CEWSPTTEEEDC"));

    /* A shutdown DISCONNECT write failure is delivered before the stop fence.
     * Its handler rejects lifecycle reentry without waiting for management. */
    closing_callback = failed_write = true; fault_code = 440;
    assert(outbox_get_size(fixture_native->outbox) > 0);
    xiaotai_err_t late = 999;
    assert(!pthread_create(&starter, NULL, subscribe_before_stop, &late));
    assert(!pthread_mutex_lock(&condition_lock));
    while (!api_deferred) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
    stopped = 999;
    assert(!pthread_create(&stopper, NULL, stop_port, &stopped));
    wait_for(&stop_waits, 1); /* The real stop is waiting for the native exit. */
    release_poll(UINT32_MAX);
    assert(!pthread_join(stopper, NULL) && stopped == XIAOTAI_OK);
    join_task();
    assert(port_events[XIAOTAI_MQTT_ERROR] == 5 && !outbox_get_size(fixture_native->outbox));
    const unsigned closed_writes = write_calls;
    const uint16_t closed_id = fixture_native->mqtt_state.connection.last_message_id;
    assert(!pthread_mutex_lock(&condition_lock));
    allow_api = true;
    assert(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&condition_lock));
    assert(!pthread_join(starter, NULL) && late == XIAOTAI_ERR_INVALID_STATE);
    assert(write_calls == closed_writes && fixture_native->mqtt_state.connection.last_message_id == closed_id &&
           !outbox_get_size(fixture_native->outbox));
    assert(!native_posts && !xt_mqtt_owner_wake(fixture_native));
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(port_client, &stats);
    assert(stats.connects >= 2 && stats.errors == 5 && stats.messages == 1 && stats.fragments == 2 && !stats.connected);
    assert(stats.published == 2 && stats.publish_failures == 2);
    assert(after_stop <= 2); /* startup race never created a foreign callback */
    assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_OK);
    assert(bridge_allocated == bridge_freed && !fixture_native && !xiaotai_mem_host_outstanding());
    check_native_balanced();

    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        const fixture_lifecycle_stats_t before = fixture_lifecycle_snapshot();
        reset_observation();
        assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_OK);
        assert(xiaotai_mqtt_start(port_client) == XIAOTAI_OK);
        wait_for(&polls, 1);
        assert(port_events[XIAOTAI_MQTT_CONNECTED] == 1);
        assert(xiaotai_mqtt_publish(port_client, "a", NULL, 0, 1, XIAOTAI_MQTT_SEND_NOW, &id)
               == XIAOTAI_OK && id > 0 && outbox_get_size(fixture_native->outbox));
        assert(xiaotai_mqtt_wake(port_client) == XIAOTAI_OK);
        assert(!pthread_create(&stopper, NULL, stop_port, &stopped));
        release_poll(UINT32_MAX);
        assert(!pthread_join(stopper, NULL) && stopped == XIAOTAI_OK);
        join_task();
        assert(!outbox_get_size(fixture_native->outbox) && !port_events[XIAOTAI_MQTT_PUBLISHED]);
        assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_OK);
        assert(bridge_allocated == bridge_freed && !fixture_native && !xiaotai_mem_host_outstanding());
        check_native_balanced();
        const fixture_lifecycle_stats_t after = fixture_lifecycle_snapshot();
        assert(after.wiped == before.wiped + 4U && after.tx_wiped == before.tx_wiped + 1U);
    }

    /* start's acknowledgement precedes the native connection attempt. A
     * blocked transport connect cannot make it wait for network success. */
    reset_observation();
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_OK);
    connect_hook = hold_connect;
    assert(xiaotai_mqtt_start(port_client) == XIAOTAI_OK);
    assert(!pthread_mutex_lock(&condition_lock));
    while (!connecting) assert(!pthread_cond_wait(&condition, &condition_lock));
    assert(!pthread_mutex_unlock(&condition_lock));
    assert(!port_events[XIAOTAI_MQTT_CONNECTED]);
    assert(!pthread_create(&stopper, NULL, stop_port, &stopped));
    release_poll(UINT32_MAX);
    assert(!pthread_mutex_lock(&condition_lock));
    allow_connect = true;
    assert(!pthread_cond_broadcast(&condition) && !pthread_mutex_unlock(&condition_lock));
    assert(!pthread_join(stopper, NULL) && stopped == XIAOTAI_OK);
    join_task();
    connect_hook = NULL;
    assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_OK);
    check_native_balanced();

    /* The actual native task can end itself before any connection callback.
     * stop still fences it and permits destroy, rather than restoring STARTED. */
    reset_observation();
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_OK);
    fixture_native->config->transport = NULL; /* native create_transport provider fails */
    fail_transport = true;
    assert(xiaotai_mqtt_start(port_client) == XIAOTAI_OK);
    xEventGroupWaitBits(fixture_native->status_bits, STOPPED_BIT, false, true, portMAX_DELAY);
    join_task();
    assert(!port_events[XIAOTAI_MQTT_CONNECTED] && !fixture_native->run);
    assert(xiaotai_mqtt_stop(port_client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_OK);
    assert(bridge_allocated == bridge_freed && !fixture_native && !xiaotai_mem_host_outstanding());
    check_native_balanced();

    /* The original native init ignored the event-loop allocation failure.
     * Isolate that last allocation using its unchanged init body, then run
     * the same allocation point through the final actual port path. */
    const esp_mqtt_client_config_t raw_cfg = {
        .broker.address.uri = cfg.uri,
        .credentials = {.client_id = cfg.client_id, .username = cfg.username, .authentication.password = cfg.password},
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .task.stack_size = 6144, .buffer = {.size = 1024, .out_size = 1024},
    };
    fixture_lifecycle_begin(0);
    esp_mqtt_client_handle_t stock = fixture_stock_loop_init(&raw_cfg);
    assert(stock && stock->config->event_loop_handle);
    const size_t event_loop_allocation = fixture_lifecycle_snapshot().attempts;
    assert(esp_mqtt_client_destroy(stock) == ESP_OK);
    check_native_balanced();
    fixture_lifecycle_begin(event_loop_allocation);
    stock = fixture_stock_loop_init(&raw_cfg);
    assert(stock && !stock->config->event_loop_handle);
    assert(esp_mqtt_client_destroy(stock) == ESP_OK);
    check_native_balanced();
    fixture_lifecycle_begin(event_loop_allocation);
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_ERR_NO_MEM && !port_client && !fixture_native);
    check_native_balanced();

    fixture_lifecycle_begin(0);
    assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_OK);
    const size_t construction_allocations = fixture_lifecycle_snapshot().attempts;
    assert(construction_allocations == event_loop_allocation + 1U); /* the INTERNAL bridge */
    assert(xiaotai_mqtt_destroy(port_client) == XIAOTAI_OK);
    check_native_balanced();
    for (size_t failure = 1; failure <= construction_allocations; ++failure) {
        fixture_lifecycle_begin(failure);
        assert(xiaotai_mqtt_create(&cfg, &port_client) == XIAOTAI_ERR_NO_MEM && !port_client && !fixture_native);
        check_native_balanced();
        assert(bridge_allocated == bridge_freed && !xiaotai_mem_host_outstanding());
    }
    printf("PASS: 10 complete native lifecycles, %zu allocation failures, original event-loop OOM red/final rejection green\n",
           construction_allocations);
    puts("PASS: actual port + native owner task, startup/stop fences, callback depth/order, WAKE epochs and translation");
    return 0;
}
