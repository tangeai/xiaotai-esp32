#include "thing_mqtt_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "app_task_affinity.h"

static const char *TAG = "thing_mqtt";

#define THING_MQTT_TOPIC_MAX_LEN      160
#define THING_MQTT_CLIENT_ID_MAX_LEN  144
#define THING_MQTT_PAYLOAD_MAX_LEN    4096
#define THING_MQTT_UP_PAYLOAD_MAX_LEN 512
#define THING_MQTT_HEARTBEAT_MS       30000U
#define THING_MQTT_TASK_STACK_SIZE    6144
#define THING_MQTT_HEARTBEAT_STACK_SIZE 4096
#define THING_MQTT_BUFFER_SIZE        2048
#define THING_MQTT_MAX_LISTENERS      4
#define THING_MQTT_COMMAND_ACK_BIT    (1U << 0)

typedef struct {
    bool used;
    thing_mqtt_message_cb_t on_message;
    void *ctx;
} thing_mqtt_listener_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    TaskHandle_t heartbeat_task;
    bool connected;
    bool stopping;
    char device_id[128];
    char cmd_topic[THING_MQTT_TOPIC_MAX_LEN];
    char notify_topic[THING_MQTT_TOPIC_MAX_LEN];
    char ack_topic[THING_MQTT_TOPIC_MAX_LEN];
    char up_topic[THING_MQTT_TOPIC_MAX_LEN];
    size_t payload_len;
    char payload_topic[THING_MQTT_TOPIC_MAX_LEN];
    bool payload_is_cmd;
    bool payload_is_notify;
    uint32_t heartbeat_interval_ms;
    thing_mqtt_message_cb_t on_message;
    void *ctx;
    thing_mqtt_disconnect_cb_t on_disconnect;
    void *disconnect_ctx;
    thing_mqtt_heartbeat_payload_cb_t build_heartbeat;
    void *heartbeat_ctx;
    int command_ack_msg_id;
    bool command_ack_confirmed;
    thing_mqtt_listener_t listeners[THING_MQTT_MAX_LISTENERS];
} thing_mqtt_runtime_t;

static EXT_RAM_BSS_ATTR thing_mqtt_runtime_t s_thing_mqtt;
static EXT_RAM_BSS_ATTR char s_thing_mqtt_payload[THING_MQTT_PAYLOAD_MAX_LEN];
static SemaphoreHandle_t s_thing_mqtt_lock;
static EventGroupHandle_t s_thing_mqtt_events;

static void reset_runtime_locked(bool keep_listeners)
{
    thing_mqtt_listener_t listeners[THING_MQTT_MAX_LISTENERS] = {0};

    if (keep_listeners) {
        memcpy(listeners, s_thing_mqtt.listeners, sizeof(listeners));
    }
    memset(&s_thing_mqtt, 0, sizeof(s_thing_mqtt));
    s_thing_mqtt.command_ack_msg_id = -1;
    s_thing_mqtt_payload[0] = '\0';
    if (keep_listeners) {
        memcpy(s_thing_mqtt.listeners, listeners, sizeof(s_thing_mqtt.listeners));
    }
}

static esp_err_t ensure_lock(void)
{
    if (s_thing_mqtt_lock == NULL) {
        s_thing_mqtt_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_thing_mqtt_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_thing_mqtt_events == NULL) {
        s_thing_mqtt_events = xEventGroupCreate();
        if (s_thing_mqtt_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void lock_runtime(void)
{
    if (ensure_lock() == ESP_OK) {
        xSemaphoreTake(s_thing_mqtt_lock, portMAX_DELAY);
    }
}

static void unlock_runtime(void)
{
    if (s_thing_mqtt_lock != NULL) {
        xSemaphoreGive(s_thing_mqtt_lock);
    }
}

static bool try_lock_runtime(TickType_t wait_ticks)
{
    return ensure_lock() == ESP_OK &&
           xSemaphoreTake(s_thing_mqtt_lock, wait_ticks) == pdTRUE;
}

static bool topic_equals(const esp_mqtt_event_t *event, const char *topic)
{
    return event != NULL && event->topic != NULL && topic != NULL &&
           event->topic_len == (int)strlen(topic) &&
           strncmp(event->topic, topic, (size_t)event->topic_len) == 0;
}

static uint8_t disconnect_reason_from_event(const esp_mqtt_event_t *event)
{
#ifdef CONFIG_MQTT_PROTOCOL_5
    if (event != NULL && event->error_handle != NULL) {
        return (uint8_t)event->error_handle->disconnect_return_code;
    }
#else
    (void)event;
#endif
    return 0;
}

static void publish_ack(void)
{
    static const char ack_payload[] = "{\"ack\":true}";
    int msg_id = -1;

    lock_runtime();
    if (s_thing_mqtt.client == NULL || !s_thing_mqtt.connected) {
        unlock_runtime();
        return;
    }
    xEventGroupClearBits(s_thing_mqtt_events, THING_MQTT_COMMAND_ACK_BIT);
    msg_id = esp_mqtt_client_publish(s_thing_mqtt.client,
                                     s_thing_mqtt.ack_topic,
                                     ack_payload,
                                     0,
                                     1,
                                     0);
    s_thing_mqtt.command_ack_msg_id = msg_id;
    s_thing_mqtt.command_ack_confirmed = false;
    unlock_runtime();
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "command ack queued: msg_id=%d", msg_id);
    } else {
        ESP_LOGW(TAG, "command ack queue failed: msg_id=%d", msg_id);
    }
}

static void reset_payload_assembly(void)
{
    s_thing_mqtt.payload_len = 0;
    s_thing_mqtt_payload[0] = '\0';
    s_thing_mqtt.payload_topic[0] = '\0';
    s_thing_mqtt.payload_is_cmd = false;
    s_thing_mqtt.payload_is_notify = false;
}

static void handle_mqtt_payload(const esp_mqtt_event_t *event)
{
    thing_mqtt_message_cb_t primary_cb = NULL;
    void *primary_ctx = NULL;
    thing_mqtt_listener_t listeners[THING_MQTT_MAX_LISTENERS] = {0};

    if (event == NULL || event->data == NULL || event->data_len <= 0) {
        return;
    }

    if (event->total_data_len >= (int)sizeof(s_thing_mqtt_payload)) {
        ESP_LOGW(TAG, "drop oversized MQTT message: total=%d", event->total_data_len);
        reset_payload_assembly();
        return;
    }
    if (event->current_data_offset == 0) {
        reset_payload_assembly();
        s_thing_mqtt.payload_is_cmd = topic_equals(event, s_thing_mqtt.cmd_topic);
        s_thing_mqtt.payload_is_notify = topic_equals(event, s_thing_mqtt.notify_topic);
        if (event->topic != NULL && event->topic_len > 0) {
            size_t topic_len = event->topic_len < (int)(sizeof(s_thing_mqtt.payload_topic) - 1) ?
                               (size_t)event->topic_len :
                               sizeof(s_thing_mqtt.payload_topic) - 1;
            memcpy(s_thing_mqtt.payload_topic, event->topic, topic_len);
            s_thing_mqtt.payload_topic[topic_len] = '\0';
        }
    }
    if (!s_thing_mqtt.payload_is_cmd && !s_thing_mqtt.payload_is_notify) {
        return;
    }
    if (event->current_data_offset != (int)s_thing_mqtt.payload_len) {
        ESP_LOGW(TAG,
                 "drop out-of-order MQTT fragment: offset=%d buffered=%u total=%d",
                 event->current_data_offset,
                 (unsigned)s_thing_mqtt.payload_len,
                 event->total_data_len);
        reset_payload_assembly();
        return;
    }
    if (s_thing_mqtt.payload_len + (size_t)event->data_len >= sizeof(s_thing_mqtt_payload)) {
        ESP_LOGW(TAG, "drop MQTT fragment: len=%u chunk=%d",
                 (unsigned)s_thing_mqtt.payload_len,
                 event->data_len);
        reset_payload_assembly();
        return;
    }

    memcpy(s_thing_mqtt_payload + s_thing_mqtt.payload_len, event->data, (size_t)event->data_len);
    s_thing_mqtt.payload_len += (size_t)event->data_len;
    s_thing_mqtt_payload[s_thing_mqtt.payload_len] = '\0';
    if (event->current_data_offset + event->data_len < event->total_data_len) {
        return;
    }

    if (s_thing_mqtt.payload_is_cmd) {
        publish_ack();
    }
    char topic[THING_MQTT_TOPIC_MAX_LEN] = {0};
    strlcpy(topic, s_thing_mqtt.payload_topic, sizeof(topic));

    lock_runtime();
    primary_cb = s_thing_mqtt.on_message;
    primary_ctx = s_thing_mqtt.ctx;
    memcpy(listeners, s_thing_mqtt.listeners, sizeof(listeners));
    unlock_runtime();

    if (primary_cb != NULL) {
        primary_cb(topic,
                   s_thing_mqtt_payload,
                   s_thing_mqtt.payload_len,
                   primary_ctx);
    }
    for (size_t index = 0; index < THING_MQTT_MAX_LISTENERS; ++index) {
        if (listeners[index].used && listeners[index].on_message != NULL) {
            listeners[index].on_message(topic,
                                        s_thing_mqtt_payload,
                                        s_thing_mqtt.payload_len,
                                        listeners[index].ctx);
        }
    }
    reset_payload_assembly();
}

static void thing_mqtt_event_handler(void *handler_args,
                                     esp_event_base_t base,
                                     int32_t event_id,
                                     void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_t *event = (esp_mqtt_event_t *)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
    {
        lock_runtime();
        s_thing_mqtt.connected = true;
        if (s_thing_mqtt.client != NULL) {
            (void)esp_mqtt_client_subscribe(s_thing_mqtt.client, s_thing_mqtt.cmd_topic, 1);
            (void)esp_mqtt_client_subscribe(s_thing_mqtt.client, s_thing_mqtt.notify_topic, 1);
        }
        unlock_runtime();

        ESP_LOGD(TAG, "connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
    {
        uint8_t reason = disconnect_reason_from_event(event);
        thing_mqtt_disconnect_cb_t disconnect_cb = NULL;
        void *disconnect_ctx = NULL;

        lock_runtime();
        s_thing_mqtt.connected = false;
        disconnect_cb = s_thing_mqtt.on_disconnect;
        disconnect_ctx = s_thing_mqtt.disconnect_ctx;
        unlock_runtime();
        if (s_thing_mqtt_events != NULL) {
            xEventGroupSetBits(s_thing_mqtt_events, THING_MQTT_COMMAND_ACK_BIT);
        }

        ESP_LOGW(TAG, "disconnected: reason=0x%02x", reason);
        if (disconnect_cb != NULL) {
            disconnect_cb(reason, disconnect_ctx);
        }
        break;
    }
    case MQTT_EVENT_DATA:
        handle_mqtt_payload(event);
        break;
    case MQTT_EVENT_PUBLISHED:
    {
        bool command_ack_confirmed = false;

        lock_runtime();
        if (s_thing_mqtt.command_ack_msg_id >= 0 &&
            event->msg_id == s_thing_mqtt.command_ack_msg_id) {
            s_thing_mqtt.command_ack_confirmed = true;
            command_ack_confirmed = true;
        }
        unlock_runtime();
        if (command_ack_confirmed && s_thing_mqtt_events != NULL) {
            ESP_LOGD(TAG, "command ack confirmed: msg_id=%d", event->msg_id);
            xEventGroupSetBits(s_thing_mqtt_events, THING_MQTT_COMMAND_ACK_BIT);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;

    while (true) {
        uint32_t wait_ms = THING_MQTT_HEARTBEAT_MS;
        bool stopping = false;
        bool ready = false;
        thing_mqtt_heartbeat_payload_cb_t build_heartbeat = NULL;
        void *heartbeat_ctx = NULL;

        lock_runtime();
        stopping = s_thing_mqtt.stopping;
        wait_ms = s_thing_mqtt.heartbeat_interval_ms != 0 ?
                  s_thing_mqtt.heartbeat_interval_ms :
                  THING_MQTT_HEARTBEAT_MS;
        unlock_runtime();
        if (stopping) {
            break;
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        lock_runtime();
        stopping = s_thing_mqtt.stopping;
        ready = s_thing_mqtt.client != NULL && s_thing_mqtt.connected;
        build_heartbeat = s_thing_mqtt.build_heartbeat;
        heartbeat_ctx = s_thing_mqtt.heartbeat_ctx;
        unlock_runtime();
        if (stopping) {
            break;
        }
        if (!ready) {
            continue;
        }

        ++seq;
        char payload[THING_MQTT_UP_PAYLOAD_MAX_LEN] = {0};

        esp_err_t ret = ESP_FAIL;
        if (build_heartbeat != NULL) {
            ret = build_heartbeat(payload, sizeof(payload), seq, heartbeat_ctx);
        }
        if (ret != ESP_OK || payload[0] == '\0') {
            int written = snprintf(payload,
                                   sizeof(payload),
                                   "{\"type\":\"heartbeat\",\"seq\":%lu,\"ts\":%lld}",
                                   (unsigned long)seq,
                                   (long long)(esp_timer_get_time() / 1000000LL));
            if (written <= 0 || written >= (int)sizeof(payload)) {
                continue;
            }
        }
        (void)thing_mqtt_client_publish_up(payload, 0);
    }

    lock_runtime();
    s_thing_mqtt.heartbeat_task = NULL;
    unlock_runtime();
    vTaskDeleteWithCaps(NULL);
}

esp_err_t thing_mqtt_client_start(const thing_mqtt_client_config_t *config)
{
    char active_device_id[sizeof(s_thing_mqtt.device_id)] = {0};

    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "mqtt lock init failed");

    if (config == NULL ||
        config->broker_uri == NULL || config->broker_uri[0] == '\0' ||
        config->device_id == NULL || config->device_id[0] == '\0' ||
        config->mqtt_token == NULL || config->mqtt_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(config->broker_uri, "mqtts://", 8) != 0) {
        ESP_LOGE(TAG, "plaintext device MQTT transport rejected");
        return ESP_ERR_INVALID_ARG;
    }

    lock_runtime();
    if (s_thing_mqtt.client != NULL) {
        bool same_identity = strcmp(s_thing_mqtt.device_id, config->device_id) == 0;
        strlcpy(active_device_id, s_thing_mqtt.device_id, sizeof(active_device_id));
        unlock_runtime();
        if (same_identity) {
            return ESP_OK;
        }
        ESP_LOGE(TAG,
                 "start rejected: active identity=%s requested identity=%s",
                 active_device_id,
                 config->device_id);
        return ESP_ERR_INVALID_STATE;
    }
    reset_runtime_locked(true);
    strlcpy(s_thing_mqtt.device_id, config->device_id, sizeof(s_thing_mqtt.device_id));
    snprintf(s_thing_mqtt.cmd_topic, sizeof(s_thing_mqtt.cmd_topic), "device/sn_%s/cmd", config->device_id);
    snprintf(s_thing_mqtt.notify_topic, sizeof(s_thing_mqtt.notify_topic), "device/sn_%s/notify", config->device_id);
    snprintf(s_thing_mqtt.ack_topic, sizeof(s_thing_mqtt.ack_topic), "device/sn_%s/ack", config->device_id);
    snprintf(s_thing_mqtt.up_topic, sizeof(s_thing_mqtt.up_topic), "device/sn_%s/up", config->device_id);
    s_thing_mqtt.heartbeat_interval_ms = config->heartbeat_interval_ms;
    s_thing_mqtt.on_message = config->on_message;
    s_thing_mqtt.ctx = config->ctx;
    s_thing_mqtt.on_disconnect = config->on_disconnect;
    s_thing_mqtt.disconnect_ctx = config->disconnect_ctx;
    s_thing_mqtt.build_heartbeat = config->build_heartbeat;
    s_thing_mqtt.heartbeat_ctx = config->heartbeat_ctx;
    unlock_runtime();

    char client_id[THING_MQTT_CLIENT_ID_MAX_LEN] = {0};
    snprintf(client_id, sizeof(client_id), "sn_%s", config->device_id);

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = client_id,
        .credentials.username = config->device_id,
        .credentials.authentication.password = config->mqtt_token,
        .session.keepalive = 60,
#ifdef CONFIG_MQTT_PROTOCOL_5
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
#endif
        .network.timeout_ms = 10000,
        .task.stack_size = THING_MQTT_TASK_STACK_SIZE,
        .buffer.size = THING_MQTT_BUFFER_SIZE,
        .buffer.out_size = THING_MQTT_BUFFER_SIZE,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
    if (client == NULL) {
        lock_runtime();
        reset_runtime_locked(true);
        unlock_runtime();
        return ESP_ERR_NO_MEM;
    }

    lock_runtime();
    s_thing_mqtt.client = client;
    unlock_runtime();

    esp_err_t ret = esp_mqtt_client_register_event(client,
                                                   MQTT_EVENT_ANY,
                                                   thing_mqtt_event_handler,
                                                   NULL);
    if (ret == ESP_OK) {
        ret = esp_mqtt_client_start(client);
    }
    if (ret != ESP_OK) {
        lock_runtime();
        s_thing_mqtt.client = NULL;
        reset_runtime_locked(true);
        unlock_runtime();
        esp_mqtt_client_destroy(client);
        return ret;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(heartbeat_task,
                                                          "thing_mqtt_hb",
                                                          THING_MQTT_HEARTBEAT_STACK_SIZE,
                                                          NULL,
                                                          3,
                                                          &s_thing_mqtt.heartbeat_task,
                                                          APP_TASK_CORE_NETWORK,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "heartbeat task not started");
    }

    ESP_LOGD(TAG, "start requested");
    return ESP_OK;
}

void thing_mqtt_client_stop(void)
{
    if (ensure_lock() != ESP_OK) {
        return;
    }

    lock_runtime();
    if (s_thing_mqtt.client == NULL) {
        reset_runtime_locked(true);
        unlock_runtime();
        return;
    }

    s_thing_mqtt.stopping = true;
    TaskHandle_t heartbeat_task_handle = s_thing_mqtt.heartbeat_task;
    esp_mqtt_client_handle_t client = s_thing_mqtt.client;
    s_thing_mqtt.client = NULL;
    s_thing_mqtt.connected = false;
    unlock_runtime();
    if (s_thing_mqtt_events != NULL) {
        xEventGroupSetBits(s_thing_mqtt_events, THING_MQTT_COMMAND_ACK_BIT);
    }

    if (heartbeat_task_handle != NULL) {
        xTaskNotifyGive(heartbeat_task_handle);
    }
    (void)esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    for (int retry = 0; retry < 20; ++retry) {
        lock_runtime();
        bool heartbeat_running = s_thing_mqtt.heartbeat_task != NULL;
        unlock_runtime();
        if (!heartbeat_running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lock_runtime();
    reset_runtime_locked(true);
    unlock_runtime();
}

bool thing_mqtt_client_is_started(void)
{
    bool started = false;

    if (ensure_lock() != ESP_OK) {
        return false;
    }
    lock_runtime();
    started = s_thing_mqtt.client != NULL;
    unlock_runtime();
    return started;
}

bool thing_mqtt_client_is_connected(void)
{
    bool connected = false;

    if (ensure_lock() != ESP_OK) {
        return false;
    }
    lock_runtime();
    connected = s_thing_mqtt.client != NULL && s_thing_mqtt.connected;
    unlock_runtime();
    return connected;
}

esp_err_t thing_mqtt_client_wait_last_command_ack(uint32_t timeout_ms)
{
    int target_msg_id = -1;
    bool confirmed = false;
    bool connected = false;

    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "mqtt ack wait init failed");

    lock_runtime();
    target_msg_id = s_thing_mqtt.command_ack_msg_id;
    confirmed = s_thing_mqtt.command_ack_confirmed;
    connected = s_thing_mqtt.client != NULL && s_thing_mqtt.connected;
    unlock_runtime();

    if (target_msg_id < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (confirmed) {
        return ESP_OK;
    }
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_thing_mqtt_events,
                                           THING_MQTT_COMMAND_ACK_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & THING_MQTT_COMMAND_ACK_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    lock_runtime();
    confirmed = s_thing_mqtt.command_ack_msg_id == target_msg_id &&
                s_thing_mqtt.command_ack_confirmed;
    unlock_runtime();
    return confirmed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t thing_mqtt_client_publish_up(const char *payload, int qos)
{
    esp_mqtt_client_handle_t client = NULL;
    bool connected = false;
    int msg_id = -1;

    if (payload == NULL || payload[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    lock_runtime();
    client = s_thing_mqtt.client;
    connected = s_thing_mqtt.connected;
    if (client == NULL || !connected || s_thing_mqtt.up_topic[0] == '\0') {
        unlock_runtime();
        return ESP_ERR_INVALID_STATE;
    }

    msg_id = esp_mqtt_client_publish(client,
                                     s_thing_mqtt.up_topic,
                                     payload,
                                     0,
                                     qos > 0 ? qos : 0,
                                     0);
    unlock_runtime();
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t thing_mqtt_client_add_listener(thing_mqtt_message_cb_t on_message,
                                         void *ctx,
                                         thing_mqtt_listener_handle_t *handle)
{
    if (on_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "mqtt lock init failed");

    lock_runtime();
    for (int index = 0; index < THING_MQTT_MAX_LISTENERS; ++index) {
        if (s_thing_mqtt.listeners[index].used &&
            s_thing_mqtt.listeners[index].on_message == on_message &&
            s_thing_mqtt.listeners[index].ctx == ctx) {
            if (handle != NULL) {
                *handle = index;
            }
            unlock_runtime();
            return ESP_OK;
        }
    }
    for (int index = 0; index < THING_MQTT_MAX_LISTENERS; ++index) {
        if (!s_thing_mqtt.listeners[index].used) {
            s_thing_mqtt.listeners[index].used = true;
            s_thing_mqtt.listeners[index].on_message = on_message;
            s_thing_mqtt.listeners[index].ctx = ctx;
            if (handle != NULL) {
                *handle = index;
            }
            unlock_runtime();
            return ESP_OK;
        }
    }
    unlock_runtime();
    return ESP_ERR_NO_MEM;
}

esp_err_t thing_mqtt_client_remove_listener_timeout(thing_mqtt_listener_handle_t handle,
                                                    uint32_t timeout_ms)
{
    TickType_t wait_ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (handle < 0 || handle >= THING_MQTT_MAX_LISTENERS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!try_lock_runtime(wait_ticks)) {
        return ESP_ERR_TIMEOUT;
    }

    memset(&s_thing_mqtt.listeners[handle], 0, sizeof(s_thing_mqtt.listeners[handle]));
    unlock_runtime();
    return ESP_OK;
}

void thing_mqtt_client_remove_listener(thing_mqtt_listener_handle_t handle)
{
    (void)thing_mqtt_client_remove_listener_timeout(handle, UINT32_MAX);
}
