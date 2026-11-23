// SPDX-License-Identifier: MIT
/* Port conformance: the behaviour SDK code relies on, written only against
 * xiaotai/port headers so the same file can run on every port. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/port/clock.h"
#include "xiaotai/port/console.h"
#include "xiaotai/port/http.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/mqtt.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static xiaotai_queue_t s_queue;
static xiaotai_signal_t s_done;
static xiaotai_events_t s_events;
static xiaotai_mutex_t s_mutex;
static xiaotai_critical_t s_critical;
static xiaotai_task_t s_worker;
static volatile int s_counter;

typedef struct {
    unsigned calls;
    xiaotai_err_t result;
} kv_completion_t;

static void committed(xiaotai_err_t result, void *ctx)
{
    kv_completion_t *completion = ctx;
    ++completion->calls;
    completion->result = result;
}

static void read_back(const xiaotai_kv_value_t *values, size_t count, void *ctx)
{
    bool *valid = ctx;
    *valid = count == 2 && values[0].result == XIAOTAI_ERR_NOT_FOUND && !values[0].data &&
             values[1].result == XIAOTAI_OK && values[1].number == 4321;
}

static int storage(void)
{
    static const char space[] = "xt_port_check"; /* scratch test partition only */
    uint8_t bytes[] = {0, 2, 0xff};
    xiaotai_kv_op_t ops[] = {
        {.kind = XIAOTAI_KV_OP_SET, .key = "bytes", .data = bytes, .size = sizeof(bytes)},
        {.kind = XIAOTAI_KV_OP_SET_INT, .key = "number", .int_type = XIAOTAI_KV_U16, .number = 4321},
    };
    kv_completion_t completion = {0};
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_kv_commit_async(space, ops, 2, committed, &completion) == XIAOTAI_OK);
    memset(bytes, 0, sizeof(bytes));
    ops[1].number = 0;
    CHECK(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    CHECK(completion.calls == 1 && completion.result == XIAOTAI_OK);
    size_t size = sizeof(bytes);
    CHECK(xiaotai_kv_get_blob_strict(space, "bytes", bytes, &size, 1000) == XIAOTAI_OK);
    CHECK(size == sizeof(bytes) && bytes[0] == 0 && bytes[1] == 2 && bytes[2] == 0xff);
    int64_t value = 0;
    CHECK(xiaotai_kv_get_int(space, "number", XIAOTAI_KV_U16, &value, 1000) == XIAOTAI_OK && value == 4321);
    CHECK(xiaotai_kv_get_int(space, "number", XIAOTAI_KV_U8, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_set_int_async(space, "number", XIAOTAI_KV_U8, 256) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_kv_read_t queries[] = {
        {.kind = XIAOTAI_KV_READ_INT, .key = "number", .int_type = XIAOTAI_KV_U8},
        {.kind = XIAOTAI_KV_READ_INT, .key = "number", .int_type = XIAOTAI_KV_U16},
    };
    bool read_valid = false;
    CHECK(xiaotai_kv_read_async(space, queries, 2, read_back, &read_valid) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(2000) == XIAOTAI_OK && read_valid);
    CHECK(xiaotai_kv_erase_space(space, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_int(space, "number", XIAOTAI_KV_U16, &value, 1000) == XIAOTAI_ERR_NOT_FOUND);
    return 0;
}

static void mqtt_event(void *ctx, const xiaotai_mqtt_event_t *event)
{
    (void)event;
    ++*(unsigned *)ctx;
}

static int idle_clients(void)
{
    /* The malformed HTTP URL cannot reach DNS or transport. MQTT is never started. */
    xiaotai_http_t *http = NULL;
    CHECK(xiaotai_http_create(NULL, &http) == XIAOTAI_OK && http);
    xiaotai_http_response_t response = {0};
    const xiaotai_http_request_t invalid = {.method = XIAOTAI_HTTP_GET, .url = "/relative", .timeout_ms = 100};
    CHECK(xiaotai_http_perform(http, &invalid, &response) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(!xiaotai_http_idle_connection(http));
    xiaotai_http_destroy(http);

    unsigned events = 0;
    const xiaotai_mqtt_config_t mqtt_config = {
        .uri = "mqtt://127.0.0.1", .client_id = "xt-port-check", .handler = mqtt_event, .ctx = &events,
    };
    xiaotai_mqtt_t *mqtt = NULL;
    CHECK(xiaotai_mqtt_create(&mqtt_config, &mqtt) == XIAOTAI_OK && mqtt);
    CHECK(xiaotai_mqtt_wake(mqtt) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_mqtt_publish(mqtt, "test", "x", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_mqtt_stop(mqtt) == XIAOTAI_OK);
    CHECK(xiaotai_mqtt_destroy(mqtt) == XIAOTAI_OK && events == 0);
    return 0;
}

static int platform_values(void)
{
    static const uint8_t abc[XIAOTAI_SHA256_BYTES] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t hash[XIAOTAI_SHA256_BYTES];
    CHECK(xiaotai_sha256("abc", 3, hash) == XIAOTAI_OK && !memcmp(hash, abc, sizeof(hash)));
    CHECK(xiaotai_hmac_sha256(NULL, 0, "abc", 3, hash) == XIAOTAI_ERR_INVALID_ARG);
    for (size_t i = 0; i < sizeof(hash); ++i) CHECK(hash[i] == 0);

    static const char *const servers[] = {"clock.invalid"};
    const xiaotai_clock_config_t clock = {.servers = servers, .server_count = 1};
    CHECK(xiaotai_clock_init(NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_clock_init(&clock) == XIAOTAI_OK);
    CHECK(!xiaotai_clock_synced());
    CHECK(xiaotai_clock_set_timezone(NULL) == XIAOTAI_OK);
    xiaotai_clock_fields_t fields;
    CHECK(xiaotai_clock_local(INT64_C(1704067200000), &fields) == XIAOTAI_OK);
    CHECK(fields.year == 2024 && fields.month == 1 && fields.day == 1 && fields.hour == 0 && fields.utc_offset_s == 0);
    CHECK(xiaotai_clock_set_unix_ms(0) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_console_info_t console = {0};
    xiaotai_console_info(&console);
    CHECK(console.device && (console.present == (console.device[0] != '\0')));
    return 0;
}

static void producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < 64; ++i) xiaotai_queue_send(&s_queue, &i, XIAOTAI_WAIT_FOREVER);
    xiaotai_events_set(&s_events, 1u << 3);
    xiaotai_signal_give(&s_done);
    xiaotai_task_exit();
}

static void notified_worker(void *arg)
{
    (void)arg;
    if (xiaotai_task_wait_notify(2000)) xiaotai_events_set(&s_events, 1u << 5);
    for (int i = 0; i < 1000; ++i) {
        xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
        ++s_counter;
        xiaotai_mutex_unlock(&s_mutex);
    }
    xiaotai_signal_give(&s_done);
    xiaotai_task_exit();
}

int main(void)
{
    CHECK(storage() == 0);
    CHECK(idle_clients() == 0);
    CHECK(platform_values() == 0);

    /* time and delay */
    const int64_t start = xiaotai_time_us();
    xiaotai_delay_ms(20);
    const int64_t elapsed = xiaotai_time_us() - start;
    CHECK(elapsed >= 15000 && elapsed < 2000000);

    /* memory by purpose */
    void *bulk = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 1000);
    uint8_t *zeroed = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 16, 4);
    void *aligned = xiaotai_mem_aligned(XIAOTAI_MEM_DMA, 64, 256);
    CHECK(bulk && zeroed && aligned && ((uintptr_t)aligned % 64) == 0);
    for (int i = 0; i < 64; ++i) CHECK(zeroed[i] == 0);
    CHECK(!xiaotai_mem_has_external() || xiaotai_mem_is_external(bulk));
    xiaotai_mem_stats_t heap;
    xiaotai_mem_sample();
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &heap);
    CHECK(heap.minimum_free <= heap.free && heap.free <= heap.total && heap.largest_block <= heap.free);
    CHECK(heap.samples && heap.minimum_largest_block <= heap.largest_block);
    xiaotai_mem_free(bulk);
    xiaotai_mem_free(zeroed);
    xiaotai_mem_free(aligned);
    xiaotai_mem_free(NULL);

    /* queue: order, depth limit, timeout, reset */
    CHECK(xiaotai_queue_init(&s_queue, 4, sizeof(uint32_t), XIAOTAI_MEM_BULK) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_done) == XIAOTAI_OK);
    CHECK(xiaotai_events_init(&s_events) == XIAOTAI_OK);
    CHECK(xiaotai_mutex_init(&s_mutex) == XIAOTAI_OK);
    xiaotai_critical_init(&s_critical);
    uint32_t value = 7;
    for (int i = 0; i < 4; ++i) CHECK(xiaotai_queue_send(&s_queue, &value, 0));
    CHECK(!xiaotai_queue_send(&s_queue, &value, 10));
    CHECK(xiaotai_queue_count(&s_queue) == 4);
    xiaotai_queue_reset(&s_queue);
    CHECK(xiaotai_queue_count(&s_queue) == 0);
    CHECK(!xiaotai_queue_receive(&s_queue, &value, 10));

    /* a producer task fills a short queue faster than we drain it: nothing lost, order kept */
    xiaotai_task_t producer_task;
    const xiaotai_task_config_t producer_config = {
        .name = "producer", .entry = producer, .stack_bytes = 4096,
        .stack = XIAOTAI_MEM_BULK, .priority = 5, .core = XIAOTAI_CORE_ANY,
    };
    CHECK(xiaotai_task_create(&producer_config, &producer_task) == XIAOTAI_OK);
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(xiaotai_queue_receive(&s_queue, &value, 2000));
        CHECK(value == i);
    }
    CHECK(xiaotai_signal_take(&s_done, 2000));
    CHECK((xiaotai_events_wait(&s_events, 1u << 3, true, true, 1000) & (1u << 3)) != 0);
    CHECK((xiaotai_events_get(&s_events) & (1u << 3)) == 0);

    /* notification and mutual exclusion */
    const xiaotai_task_config_t worker_config = {
        .name = "worker", .entry = notified_worker, .stack_bytes = 4096,
        .stack = XIAOTAI_MEM_INTERNAL, .priority = 4, .core = XIAOTAI_CORE_ANY,
    };
    CHECK(xiaotai_task_create(&worker_config, &s_worker) == XIAOTAI_OK);
    xiaotai_delay_ms(20);
    xiaotai_task_notify(&s_worker);
    for (int i = 0; i < 1000; ++i) {
        CHECK(xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER));
        ++s_counter;
        xiaotai_mutex_unlock(&s_mutex);
    }
    CHECK(xiaotai_signal_take(&s_done, 5000));
    CHECK(s_counter == 2000);
    CHECK((xiaotai_events_get(&s_events) & (1u << 5)) != 0);

    /* timeouts on a held mutex, an empty signal and unset bits */
    CHECK(xiaotai_mutex_lock(&s_mutex, 0));
    xiaotai_mutex_unlock(&s_mutex);
    CHECK(!xiaotai_signal_take(&s_done, 10));
    CHECK((xiaotai_events_wait(&s_events, 1u << 9, false, false, 10) & (1u << 9)) == 0);

    xiaotai_critical_enter(&s_critical);
    ++s_counter;
    xiaotai_critical_exit(&s_critical);
    CHECK(s_counter == 2001);

    xiaotai_firmware_t firmware;
    xiaotai_firmware(&firmware);
    CHECK(firmware.version && firmware.platform && firmware.chip && firmware.cores >= 1);
    (void)xiaotai_random();

    xiaotai_queue_deinit(&s_queue);
    xiaotai_signal_deinit(&s_done);
    xiaotai_events_deinit(&s_events);
    xiaotai_mutex_deinit(&s_mutex);
    printf("PASS: port conformance on %s (%s)\n", firmware.platform, firmware.chip);
    return 0;
}
