// SPDX-License-Identifier: MIT
#include <assert.h>
#include <net/if.h>
#include <pthread.h>
#include <stdlib.h>
#include "mqtt_client_priv.h"

static const char test_id[] = "fixture-native-client";
static const char test_user[] = "fixture-native-user";
static const char test_password[] = "fixture-native-password";
typedef struct { void *ptr; size_t size; bool sensitive; } allocation_t;
static allocation_t allocations[64];
static size_t attempts, fail_at, live, dirty, wiped, tx_wiped;
static void *tx_buffer;

static allocation_t *find_allocation(void *ptr)
{
    for (size_t i = 0; i < 64; ++i) if (allocations[i].ptr == ptr) return &allocations[i];
    assert(false);
    return NULL;
}

static void *checked_malloc(size_t size)
{
    if (++attempts == fail_at) return NULL;
    void *ptr = malloc(size);
    assert(ptr);
    allocation_t *slot = find_allocation(NULL);
    *slot = (allocation_t){ptr, size, false};
    ++live;
    return ptr;
}

static void *checked_calloc(size_t count, size_t size)
{
    void *ptr = checked_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

static char *checked_strdup(const char *value)
{
    const size_t size = strlen(value) + 1U;
    char *ptr = checked_malloc(size);
    if (!ptr) return NULL;
    memcpy(ptr, value, size);
    if (!strcmp(value, test_id) || !strcmp(value, test_user) || !strcmp(value, test_password))
        find_allocation(ptr)->sensitive = true;
    return ptr;
}

static void checked_free(void *ptr)
{
    if (!ptr) return;
    allocation_t *allocation = find_allocation(ptr);
    if (allocation->sensitive) {
        const unsigned char *data = ptr;
        unsigned char nonzero = 0;
        for (size_t i = 0; i < allocation->size; ++i) nonzero |= data[i];
        if (nonzero) ++dirty;
        else { ++wiped; if (ptr == tx_buffer) ++tx_wiped; }
    }
    *allocation = (allocation_t){0};
    assert(live);
    --live;
    free(ptr);
}

static SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    pthread_mutex_t *mutex = checked_malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(mutex, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    return mutex;
}

static void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(!pthread_mutex_destroy(mutex));
    checked_free(mutex);
}

static EventGroupHandle_t xEventGroupCreate(void) { return checked_malloc(1); }
static void vEventGroupDelete(EventGroupHandle_t events) { checked_free(events); }
static void esp_transport_destroy(esp_transport_handle_t transport) { assert(!transport); }
static void esp_transport_list_destroy(esp_transport_list_handle_t list) { (void)list; assert(false); }
static char *platform_create_id_string(void) { assert(false); return NULL; }
static int64_t platform_tick_get_ms(void) { return 1; }

esp_err_t esp_mqtt_client_set_uri(esp_mqtt_client_handle_t client, const char *uri)
{
    (void)client; (void)uri;
    assert(false); /* The fixture uses the equivalent supported hostname configuration. */
    return ESP_FAIL;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    (void)client;
    assert(false); /* No task is started in this resource ownership proof. */
    return ESP_FAIL;
}

/* All allocations inside these actual source functions are instrumented,
 * including the failure cleanup that public native init calls itself. */
#define malloc checked_malloc
#define calloc checked_calloc
#define strdup checked_strdup
#define free checked_free
#undef ESP_MEM_CHECK
#define ESP_MEM_CHECK(tag, value, action) do { (void)(tag); if (!(value)) { action; } } while (0);
#include "native_msg.c"
#define TAG outbox_tag
#include "native_outbox.c"
#undef TAG
static const char *TAG = "mqtt_client";
#include "native_cleanup.inc"
#undef malloc
#undef calloc
#undef strdup
#undef free

static esp_mqtt_client_config_t config(void)
{
    return (esp_mqtt_client_config_t){
        .broker.address = {.hostname = "fixture.invalid", .transport = MQTT_TRANSPORT_OVER_TCP},
        .credentials = {.client_id = test_id, .username = test_user, .authentication.password = test_password},
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1, .buffer = {.size = 128, .out_size = 128},
    };
}

static void begin_case(size_t failure)
{
    assert(!live);
    fail_at = failure;
    attempts = dirty = wiped = tx_wiped = 0;
    tx_buffer = NULL;
}

static void check_result(void)
{
    assert(!live);
#if !TEST_STOCK
    assert(!dirty);
#endif
    assert(!strcmp(test_id, "fixture-native-client") && !strcmp(test_user, "fixture-native-user") &&
           !strcmp(test_password, "fixture-native-password"));
}

int main(void)
{
    const esp_mqtt_client_config_t supplied = config();
    for (unsigned receive = 0; receive < 2; ++receive) {
        begin_case(0);
        struct esp_mqtt_client direct = {0};
        direct.api_lock = xSemaphoreCreateRecursiveMutex();
        assert(direct.api_lock);
        fail_at = attempts + (receive ? 3U : 2U); /* config, TX, RX */
        const esp_err_t result = esp_mqtt_set_config(&direct, &supplied);
        printf("%s: set_config %s allocation failed, result=0x%x config_null=%u\n",
               TEST_STOCK ? "stock" : "cleanup", receive ? "RX" : "TX", result, !direct.config);
        assert(!direct.config);
#if TEST_STOCK
        assert(result == ESP_OK);
#else
        assert(result == ESP_ERR_NO_MEM);
#endif
        vSemaphoreDelete(direct.api_lock);
        check_result();
    }
    begin_case(0);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&supplied);
    assert(client);
    const size_t allocation_count = attempts;
    mqtt_connect_info_t *info = &client->mqtt_state.connection.information;
    assert(info->client_id != test_id && info->username != test_user && info->password != test_password);
    assert(!strcmp(info->client_id, test_id) && !strcmp(info->username, test_user) && !strcmp(info->password, test_password));
    memset(client->mqtt_state.connection.buffer, 0xa5, client->mqtt_state.connection.buffer_length);
    mqtt_message_t *connect = mqtt_msg_connect(&client->mqtt_state.connection, info);
    assert(connect->length && memmem(connect->data, connect->length, test_password, strlen(test_password)));
    tx_buffer = client->mqtt_state.connection.buffer;
    find_allocation(tx_buffer)->sensitive = true;
    assert(esp_mqtt_client_destroy(client) == ESP_OK);
    check_result();
#if TEST_STOCK
    assert(dirty == 4 && !wiped);
#else
    assert(wiped == 4 && tx_wiped == 1);
#endif

    size_t partial_dirty = 0, partial_wiped = 0, false_success = 0;
    for (size_t failure = 1; failure <= allocation_count; ++failure) {
        begin_case(failure);
        client = esp_mqtt_client_init(&supplied);
        if (client) {
            assert(!client->config);
            ++false_success;
            printf("COUNTEREXAMPLE: native allocation %zu/%zu failed, yet init returned a client without config\n",
                   failure, allocation_count);
            assert(esp_mqtt_client_destroy(client) == ESP_OK);
        }
        check_result();
        partial_dirty += dirty;
        partial_wiped += wiped;
    }
#if TEST_STOCK
    assert(partial_dirty > 0 && !partial_wiped);
    assert(false_success == 2);
    puts("COUNTEREXAMPLE: native destroy and partial-init cleanup free nonzero credential copies / CONNECT buffer");
#else
    assert(partial_wiped > 0 && !false_success);
    puts("PASS: native credentials and CONNECT TX buffer cleared before free, all init allocation failures release ownership");
#endif
    return 0;
}
