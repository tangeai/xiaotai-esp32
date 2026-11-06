#include "wechat_voip.h"
#include "voip_state.h"
#include "voip_protocol.h"
#include "sample_audio.h"
#include "rtc_adapter.h"
#include "memory_probe.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "voip"
#define MESSAGE_SIZE 4096
#define MESSAGE_SLOTS 4
#define MAX_CONTACTS 8
#define REJECT_BODY_CAPACITY 2048
/* C3 SDK 2.5.0: WhipConnect uses 8256 bytes before its nested signaling/TLS calls. */
#define VOIP_TASK_STACK_BYTES 16384

_Static_assert((int)WECHAT_VOIP_IDLE == (int)CALL_IDLE &&
                   (int)WECHAT_VOIP_DIALING == (int)CALL_DIALING &&
                   (int)WECHAT_VOIP_RINGING == (int)CALL_RINGING &&
                   (int)WECHAT_VOIP_CONNECTING == (int)CALL_CONNECTING &&
                   (int)WECHAT_VOIP_WAITING_PEER == (int)CALL_WAITING_PEER &&
                   (int)WECHAT_VOIP_ACTIVE == (int)CALL_ACTIVE &&
                   (int)WECHAT_VOIP_CLOSING == (int)CALL_CLOSING,
               "public and internal VoIP state values must stay aligned");
typedef enum { EVENT_PLATFORM, EVENT_USER } event_kind_t;
typedef struct {
    event_kind_t kind;
    int slot;
    /* An event has exactly one source. Queue capacities and payload limits stay unchanged. */
    union {
        platform_event_t platform;
        struct {
            voip_command_t command;
            unsigned contact;
        };
    };
} event_t;
typedef struct {
    char openid[65], appid[65], remark[97];
} contact_t;
/* Contact/control caches are CPU-only and do not need DMA-capable SRAM. */
static RTC_DATA_ATTR contact_t s_contacts[MAX_CONTACTS];
static unsigned s_contact_count, s_version;
static char *s_messages[MESSAGE_SLOTS];
static size_t s_message_sizes[MESSAGE_SLOTS];
static bool s_slot_used[MESSAGE_SLOTS];
static portMUX_TYPE s_slots_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue, s_rtc_queue;
/* The single lifetime worker owns this stack; ESP-IDF stack sizes are bytes. */
_Alignas(portBYTE_ALIGNMENT) static StackType_t
    s_task_stack[VOIP_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_task_storage;
static StaticQueue_t s_queue_storage, s_rtc_queue_storage;
static RTC_DATA_ATTR uint8_t s_queue_buffer[8 * sizeof(event_t)];
static uint8_t s_rtc_queue_buffer[16 * sizeof(rtc_event_t)];
static atomic_bool s_event_failure;
static atomic_int s_public_state;
static atomic_int s_public_direction;
static atomic_uint s_public_end_reason;
static call_state_t s_call;
static cJSON *s_payload;
static tirtc_conn_t s_connection, s_closing;
static bool s_sdk_initialized, s_sdk_ready, s_sdk_stopping, s_need_shutdown;
static bool s_shutdown_failed, s_resume_after_stop;
static bool s_connect_pending, s_profile_ready, s_profile_pending, s_contacts_pending;
static bool s_contacts_dirty, s_call_request_pending;
static bool s_profile_dirty, s_audio_subscribed;
static uint8_t s_audio_stream;
static bool s_media_started, s_local_end;
static unsigned s_reason;
static int64_t s_deadline, s_disconnect_at;
static int64_t s_close_deadline;
static char s_room[129], s_openid[65], s_call_id[129], s_app_id[65];
static RTC_DATA_ATTR char s_closed_rooms[4][129];
static RTC_DATA_ATTR char s_closed_calls[4][129];
static unsigned s_closed_index;
static const char s_profile[] =
    "{\"profiles\":{\"voip\":{\"audio_rate\":16000,\"audio_channels\":1,"
    "\"down_audio_mt\":\"opus\",\"up_video_mt\":\"none\","
    "\"down_video_mt\":\"none\",\"no_video\":true,\"calling_timeout_sec\":30}}}";

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}
static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (size--)
        *p++ = 0;
}
static const char *string(const cJSON *json, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool bounded_copy(char *out, size_t capacity, const char *text)
{
    if (!text || strlen(text) >= capacity)
        return false;
    strcpy(out, text);
    return true;
}
static int reserve_slot(const char *json)
{
    if (!json)
        return -1;
    size_t length = strnlen(json, MESSAGE_SIZE);
    if (length >= MESSAGE_SIZE)
        return -2;
    int slot = -2;
    portENTER_CRITICAL(&s_slots_lock);
    for (unsigned i = 0; i < MESSAGE_SLOTS; ++i)
        if (!s_slot_used[i]) {
            s_slot_used[i] = true;
            slot = i;
            break;
        }
    portEXIT_CRITICAL(&s_slots_lock);
    if (slot >= 0) {
        /* Only control JSON is copied here. Reserve ownership before allocating;
         * the queue transfers it to the consumer. Never allocate under the mux. */
        char *copy = malloc(length + 1);
        if (!copy) {
            portENTER_CRITICAL(&s_slots_lock);
            s_slot_used[slot] = false;
            portEXIT_CRITICAL(&s_slots_lock);
            return -2;
        }
        memcpy(copy, json, length + 1);
        s_messages[slot] = copy;
        s_message_sizes[slot] = length + 1;
    }
    return slot;
}
static void release_slot(int slot)
{
    if (slot < 0)
        return;
    /* Wipe credentials before releasing the owned copy, then make the slot free. */
    volatile unsigned char *bytes = (volatile unsigned char *)s_messages[slot];
    for (size_t i = 0; i < s_message_sizes[slot]; ++i)
        bytes[i] = 0;
    free(s_messages[slot]);
    s_messages[slot] = NULL;
    s_message_sizes[slot] = 0;
    portENTER_CRITICAL(&s_slots_lock);
    s_slot_used[slot] = false;
    portEXIT_CRITICAL(&s_slots_lock);
}
bool wechat_voip_platform_event(const platform_event_t *p)
{
    event_t e = {.kind = EVENT_PLATFORM, .platform = *p, .slot = reserve_slot(p->json)};
    e.platform.json = NULL;
    if (e.slot == -2) {
        if (p->kind != PLATFORM_MESSAGE)
            atomic_store(&s_event_failure, true);
        return false;
    }
    if (xQueueSend(s_queue, &e, 0) == pdTRUE)
        return true;
    release_slot(e.slot);
    if (p->kind != PLATFORM_MESSAGE)
        atomic_store(&s_event_failure, true);
    return false;
}
static bool rtc_event(const rtc_event_t *r)
{
    if (xQueueSend(s_rtc_queue, r, 0) == pdTRUE)
        return true;
    atomic_store(&s_event_failure, true);
    return false;
}
esp_err_t wechat_voip_command(voip_command_t command, unsigned contact)
{
    event_t e = {.kind = EVENT_USER, .command = command, .contact = contact, .slot = -1};
    return s_queue && xQueueSend(s_queue, &e, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

wechat_voip_state_t wechat_voip_state(void)
{
    return (wechat_voip_state_t)atomic_load_explicit(&s_public_state,
                                                      memory_order_acquire);
}

wechat_voip_direction_t wechat_voip_direction(void)
{
    return (wechat_voip_direction_t)atomic_load_explicit(
        &s_public_direction, memory_order_acquire);
}

bool wechat_voip_last_end_failed(void)
{
    unsigned reason = atomic_load_explicit(&s_public_end_reason,
                                            memory_order_acquire);
    return reason == 6U || reason == 8U;
}
static void status(void)
{
    ESP_LOGI(
        TAG,
        "state=%s generation=%lu platform=%d rtc=%d profile=%d contacts=%u pending=%d "
        "closing=%d heap=%u largest=%u stack_hwm=%u dma_free=%u dma_largest=%u",
        call_stage_name(s_call.stage), (unsigned long)s_call.generation, platform_is_online(),
        s_sdk_ready, s_profile_ready, s_contact_count, s_connect_pending, s_closing != NULL,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                                                   MALLOC_CAP_8BIT));
    sample_audio_status();
}
static bool ready(void)
{
    return s_sdk_ready && platform_is_online() && s_profile_ready && !s_need_shutdown;
}
static void start_sdk(void)
{
    if (s_sdk_initialized || s_need_shutdown || !platform_is_online())
        return;
    platform_identity_t id;
    platform_identity_snapshot(&id);
    int ret = rtc_start(id.device_id, id.device_key, platform_client_id(), platform_rtc_endpoint());
    memset(&id, 0, sizeof(id));
    if (ret == 0)
        s_sdk_initialized = true;
    else
        ESP_LOGE(TAG, "RTC startup failed: %d; enter status/retry after fixing cause", ret);
}
static void request_profile(void)
{
    if (!platform_is_online())
        return;
    if (s_call.stage != CALL_IDLE) {
        if (!s_profile_dirty)
            ESP_LOGI(TAG, "profile refresh deferred until call idle: state=%s",
                     call_stage_name(s_call.stage));
        s_profile_dirty = true;
        return;
    }
    if (s_profile_pending) {
        s_profile_dirty = true;
        return;
    }
    esp_err_t err = platform_request(PLATFORM_PROFILE, 0, s_profile);
    if (err == ESP_OK) {
        s_profile_pending = true;
        s_profile_dirty = false;
    } else
        ESP_LOGE(TAG, "profile queue failed: %s", esp_err_to_name(err));
}
static void request_contacts(void)
{
    if (!platform_is_online()) {
        s_contacts_dirty = true;
        return;
    }
    if (s_call.stage != CALL_IDLE) {
        if (!s_contacts_dirty)
            ESP_LOGI(TAG, "contacts refresh deferred until call idle: state=%s",
                     call_stage_name(s_call.stage));
        s_contacts_dirty = true;
        return;
    }
    if (s_contacts_pending) {
        s_contacts_dirty = true;
        return;
    }
    esp_err_t err = platform_request(PLATFORM_CONTACTS, 0, NULL);
    if (err == ESP_OK) {
        s_contacts_pending = true;
        s_contacts_dirty = false;
    } else
        ESP_LOGW(TAG, "contacts unavailable: %s", esp_err_to_name(err));
}
static void list_contacts(void)
{
    for (unsigned i = 0; i < s_contact_count; ++i)
        printf("CONTACT %u name=%s app=%s openid=%s\n", i + 1, s_contacts[i].remark,
               s_contacts[i].appid, s_contacts[i].openid);
    ESP_LOGI(TAG, "contacts=%u; authorize in WeChat if empty (not stored in NVS)", s_contact_count);
}
static void refresh_deferred_metadata(void)
{
    if (!platform_is_online() || s_call.stage != CALL_IDLE)
        return;
    if (s_profile_dirty) {
        ESP_LOGI(TAG, "run deferred profile refresh after call");
        request_profile();
    } else if (s_contacts_dirty) {
        ESP_LOGI(TAG, "run deferred contacts refresh after call");
        request_contacts();
    }
}
static void finish(void)
{
    if (s_connect_pending || s_connection || s_closing || s_call_request_pending)
        return;
    sample_audio_end();
    s_media_started = false;
    platform_set_realtime_media(false);
    if (s_room[0] || s_call_id[0]) {
        unsigned slot = s_closed_index++ % 4;
        strcpy(s_closed_rooms[slot], s_room);
        strcpy(s_closed_calls[slot], s_call_id);
    }
    cJSON_Delete(s_payload);
    s_payload = NULL;
    memset(s_room, 0, sizeof(s_room));
    memset(s_openid, 0, sizeof(s_openid));
    memset(s_call_id, 0, sizeof(s_call_id));
    memset(s_app_id, 0, sizeof(s_app_id));
    call_finish(&s_call);
    s_deadline = s_disconnect_at = 0;
    s_close_deadline = 0;
    s_local_end = false;
    s_reason = 0;
    ESP_LOGI(TAG, "call resources ready: generation=%lu", (unsigned long)s_call.generation);
    refresh_deferred_metadata();
}
static int reject_payload(const cJSON *payload, unsigned reason)
{
    if (!payload || !s_sdk_ready)
        return -1;
    if (!string(payload, "wx_app_id")[0] || !string(payload, "wx_model_id")[0] ||
        !string(payload, "wx_server_token")[0] || !string(payload, "wx_room_id")[0]) {
        ESP_LOGE(TAG, "reject credentials missing; request not submitted");
        return -1;
    }
    /* Reject is a cold path that never overlaps active media. Keep its full
     * protocol capacity, but do not reserve 2 KiB from application RAM forever. */
    char *body = malloc(REJECT_BODY_CAPACITY);
    if (!body) {
        ESP_LOGE(TAG, "reject body allocation failed: bytes=%u", REJECT_BODY_CAPACITY);
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    bool ok =
        root && cJSON_AddStringToObject(root, "wx_app_id", string(payload, "wx_app_id")) &&
        cJSON_AddStringToObject(root, "wx_model_id", string(payload, "wx_model_id")) &&
        cJSON_AddStringToObject(root, "wx_session_token", string(payload, "wx_server_token")) &&
        cJSON_AddStringToObject(root, "wx_room_id", string(payload, "wx_room_id")) &&
        cJSON_AddStringToObject(root, "wx_payload", string(payload, "wx_payload")) &&
        cJSON_AddNumberToObject(root, "hangup_reason", reason) &&
        cJSON_PrintPreallocated(root, body, REJECT_BODY_CAPACITY, false);
    cJSON_Delete(root);
    if (!ok) {
        secure_clear(body, REJECT_BODY_CAPACITY);
        free(body);
        ESP_LOGE(TAG, "reject body invalid/too large");
        return -1;
    }
    /* SDK 2.5.0 completes this request synchronously. The optional data callback
     * is not a completion event and may be absent on a successful redirect. */
    int64_t begin = now_ms();
    ESP_LOGI(TAG, "REJECT begin: reason=%u bytes=%u", reason, (unsigned)strlen(body));
    int ret = rtc_service_request("/v1/wxvoip/reject", body, NULL);
    secure_clear(body, REJECT_BODY_CAPACITY);
    free(body);
    if (ret == 0)
        ESP_LOGI(TAG, "REJECT done: ret=0 elapsed_ms=%lld", (long long)(now_ms() - begin));
    else
        ESP_LOGE(TAG, "REJECT failed: ret=%d elapsed_ms=%lld", ret, (long long)(now_ms() - begin));
    return ret;
}
static void end_call(bool notify, unsigned reason)
{
    if (s_call.stage == CALL_IDLE || s_call.stage == CALL_CLOSING)
        return;
    ESP_LOGI(TAG, "end: source=%s state=%s reason=%u generation=%lu", notify ? "local" : "remote",
             call_stage_name(s_call.stage), reason, (unsigned long)s_call.generation);
    s_local_end = notify;
    s_reason = reason;
    atomic_store_explicit(&s_public_end_reason, reason,
                          memory_order_release);
    sample_audio_end();
    s_media_started = false;
    call_close(&s_call);
    s_close_deadline = now_ms() + 35000;
    if (s_connection) {
        if (notify) {
            char body[32];
            snprintf(body, sizeof(body), "{\"reason\":%u}", reason);
            int ret = rtc_send_command(s_connection, 0x2001, body);
            ESP_LOGI(TAG, "HANGUP submit: ret=%d", ret);
            /* Match upstream delayed disconnect: allow the queued command to
             * leave before releasing transport. This is not a retry/cooldown. */
            s_disconnect_at = now_ms() + 150;
        } else
            s_disconnect_at = now_ms();
    } else if (!s_connect_pending) {
        if (notify && s_payload)
            (void)reject_payload(s_payload, reason);
        if (notify && s_call.outgoing && !s_payload)
            ESP_LOGW(TAG,
                     "cancelled before room: no device cancel API; reject correlated late room");
        finish();
    }
}
static void connect_room(void)
{
    const char *peer = string(s_payload, "peer_id"), *token = string(s_payload, "token");
    s_connect_pending = true;
    s_audio_stream = 10;
    s_audio_subscribed = true;
    s_deadline = now_ms() + 30000;
    status();
    ESP_LOGI(TAG, "WHIP submit: generation=%lu peer_len=%u token_len=%u",
             (unsigned long)s_call.generation, (unsigned)strlen(peer), (unsigned)strlen(token));
    int rc = rtc_connect(s_call.generation, peer, token);
    if (rc == 0) {
        /* rtc_adapter owns copies through completion. Keep the wx_* fields
         * for pre-connect rejection, but not a second copy of the WHIP inputs. */
        cJSON_DeleteItemFromObjectCaseSensitive(s_payload, "peer_id");
        cJSON_DeleteItemFromObjectCaseSensitive(s_payload, "token");
    }
    if (rc != 0) {
        s_connect_pending = false;
        ESP_LOGE(TAG, "WHIP submit failed: %d", rc);
        end_call(true, 8);
    }
}
static void incoming(const cJSON *payload)
{
    const char *room = string(payload, "wx_room_id");
    const char *peer = string(payload, "peer_id"), *token = string(payload, "token");
    const char *id = string(payload, "wx_call_id");
    const char *type = string(payload, "wx_room_type");
    if (!room[0] || strlen(room) >= sizeof(s_room) || !peer[0] || strlen(peer) >= 2048 ||
        !token[0] || strlen(token) >= 1024 || strlen(id) >= sizeof(s_call_id)) {
        ESP_LOGE(TAG, "incoming parameters missing/oversized; refusing truncation");
        return;
    }
    if (s_room[0] && strcmp(room, s_room) == 0)
        return; /* QoS redelivery, never reject own active room. */
    for (unsigned i = 0; i < 4; ++i)
        if (strcmp(room, s_closed_rooms[i]) == 0)
            return;
    for (unsigned i = 0; i < 4; ++i) {
        if (id[0] && strcmp(id, s_closed_calls[i]) == 0) {
            reject_payload(payload, 7);
            return;
        }
    }
    platform_identity_t identity;
    platform_identity_snapshot(&identity);
    room_route_t route = voip_route_room(payload, identity.device_id, s_call.stage == CALL_DIALING,
                                         s_openid, s_app_id, s_call_id);
    memset(&identity, 0, sizeof(identity));
    bool outgoing = route == ROOM_OUTBOUND;
    if (!ready() || (!outgoing && (route != ROOM_INBOUND || s_call.stage != CALL_IDLE)) ||
        (type[0] && strcmp(type, "voice") != 0 && strcmp(type, "audio") != 0)) {
        reject_payload(payload, 5);
        return;
    }
    cJSON *copy = cJSON_Duplicate(payload, true);
    if (!copy) {
        reject_payload(payload, 8);
        return;
    }
    s_payload = copy;
    strcpy(s_room, room);
    if (outgoing) {
        if (id[0])
            bounded_copy(s_call_id, sizeof(s_call_id), id);
        call_join_outgoing(&s_call);
        ESP_LOGI(TAG, "outbound room arrived; connect immediately, wait for 0x2000/0x2001");
        connect_room();
    } else {
        call_begin(&s_call, false);
        atomic_store_explicit(&s_public_end_reason, 0U,
                              memory_order_release);
        s_deadline = now_ms() + 30000;
        ESP_LOGI(TAG, "INCOMING: enter answer or reject; generation=%lu",
                 (unsigned long)s_call.generation);
    }
}
static void signal(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    const char *channel = string(root, "channel"), *type = string(root, "type");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (strcmp(channel, "wx") == 0) {
        if (strcmp(type, "call_incoming") == 0)
            incoming(payload);
        else if (strcmp(type, "call_cancel") == 0) {
            const char *room = string(payload, "wx_room_id");
            if (room[0] && s_room[0] && strcmp(room, s_room) == 0)
                end_call(false, 1);
        } else if (strcmp(type, "callers_update") == 0) {
            s_contact_count = 0; /* Old authorization must not be used after invalidation. */
            request_contacts();
        }
    }
    cJSON_Delete(root);
}
static void http_result(const platform_event_t *p, const char *text)
{
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (p->operation == PLATFORM_PROFILE) {
        s_profile_pending = false;
        if (s_profile_dirty) {
            s_profile_ready = false;
            if (platform_is_online())
                request_profile();
            cJSON_Delete(root);
            return;
        }
        s_profile_ready = p->error == ESP_OK && platform_is_online();
        if (s_profile_ready) {
            ESP_LOGI(TAG, "VoIP profile ready");
            request_contacts();
        } else
            ESP_LOGE(TAG, "profile failed: result=%s msg=%.160s; enter contacts to retry profile",
                     esp_err_to_name(p->error), string(root, "msg"));
    } else if (p->operation == PLATFORM_CONTACTS) {
        s_contacts_pending = false;
        s_contact_count = 0;
        if (s_contacts_dirty) {
            if (platform_is_online())
                request_contacts();
            cJSON_Delete(root);
            return;
        }
        const cJSON *contacts = cJSON_GetObjectItemCaseSensitive(data, "contacts");
        if (p->error == ESP_OK && platform_is_online() && cJSON_IsArray(contacts)) {
            const cJSON *item;
            cJSON_ArrayForEach(item, contacts)
            {
                if (s_contact_count == MAX_CONTACTS) {
                    ESP_LOGW(TAG, "contact capacity %u reached", MAX_CONTACTS);
                    break;
                }
                contact_t *c = &s_contacts[s_contact_count];
                if (bounded_copy(c->openid, sizeof(c->openid), string(item, "wx_open_id")) &&
                    bounded_copy(c->appid, sizeof(c->appid), string(item, "wx_app_id")) &&
                    bounded_copy(c->remark, sizeof(c->remark), string(item, "remark")) &&
                    c->openid[0] && c->appid[0])
                    ++s_contact_count;
                else
                    ESP_LOGW(TAG, "invalid/oversized contact skipped");
            }
            list_contacts();
        } else
            ESP_LOGE(TAG, "contacts fetch failed; cache not usable");
    } else if (p->operation == PLATFORM_CALL && p->generation == s_call.generation) {
        s_call_request_pending = false;
        if (s_call.stage == CALL_CLOSING) {
            if (p->error == ESP_OK)
                bounded_copy(s_call_id, sizeof(s_call_id), string(data, "call_id"));
            finish();
            cJSON_Delete(root);
            return;
        }
        if (p->error != ESP_OK && s_call.stage == CALL_DIALING) {
            ESP_LOGE(TAG, "call request failed: %s; request is not retried",
                     esp_err_to_name(p->error));
            end_call(false, 8);
        } else if (p->error == ESP_OK) {
            if (!bounded_copy(s_call_id, sizeof(s_call_id), string(data, "call_id")))
                end_call(true, 8);
            else
                ESP_LOGI(TAG, "call request accepted; generation=%lu",
                         (unsigned long)p->generation);
        }
    }
    cJSON_Delete(root);
}
static void dial(unsigned index)
{
    if (!ready() || s_call.stage != CALL_IDLE || index == 0 || index > s_contact_count) {
        ESP_LOGW(TAG, "call rejected locally: check status and contact index");
        return;
    }
    const contact_t *c = &s_contacts[index - 1];
    cJSON *body = cJSON_CreateObject();
    char request[768];
    platform_identity_t identity;
    platform_identity_snapshot(&identity);
    bool ok = body && cJSON_AddStringToObject(body, "device_id", identity.device_id) &&
              cJSON_AddStringToObject(body, "wx_user_openid", c->openid) &&
              cJSON_AddStringToObject(body, "wx_app_id", c->appid) &&
              cJSON_AddStringToObject(body, "wx_room_type", "voice") &&
              cJSON_AddNumberToObject(body, "wx_version_type", s_version) &&
              cJSON_PrintPreallocated(body, request, sizeof(request), false);
    cJSON_Delete(body);
    memset(&identity, 0, sizeof(identity));
    if (!ok)
        return;
    call_begin(&s_call, true); /* Reserve direction BEFORE HTTP, MQTT may arrive first. */
    atomic_store_explicit(&s_public_end_reason, 0U,
                          memory_order_release);
    strcpy(s_openid, c->openid);
    strcpy(s_app_id, c->appid);
    s_deadline = now_ms() + 40000; /* Up to 10 s HTTP plus 30 s ringing. */
    esp_err_t err = platform_request(PLATFORM_CALL, s_call.generation, request);
    if (err != ESP_OK) {
        end_call(false, 8);
        return;
    }
    s_call_request_pending = true;
    ESP_LOGI(TAG, "DIALING: contact=%u wx_version_type=%u generation=%lu", index, s_version,
             (unsigned long)s_call.generation);
}
static void answer_incoming(void)
{
    if (ready() && call_accept(&s_call)) {
        ESP_LOGI(TAG, "ANSWER: generation=%lu", (unsigned long)s_call.generation);
        connect_room();
    } else {
        ESP_LOGW(TAG, "no ready incoming call");
    }
}
static void primary_action(void)
{
    call_primary_action_t action = call_primary_action(s_call.stage);
    switch (action) {
    case CALL_PRIMARY_DIAL_FIRST:
        if (!ready()) {
            ESP_LOGW(TAG, "BOOT: idle but service is not ready; check status");
        } else if (!s_contact_count) {
            ESP_LOGW(TAG, "BOOT: no authorized contact; refreshing contacts");
            request_contacts();
        } else {
            ESP_LOGI(TAG, "BOOT: idle -> call contact 1");
            dial(1);
        }
        break;
    case CALL_PRIMARY_ANSWER:
        ESP_LOGI(TAG, "BOOT: incoming -> answer");
        answer_incoming();
        break;
    case CALL_PRIMARY_HANGUP:
        ESP_LOGI(TAG, "BOOT single click: state=%s; double click is required to hang up",
                 call_stage_name(s_call.stage));
        break;
    case CALL_PRIMARY_NONE:
    default:
        ESP_LOGI(TAG, "BOOT: state=%s action ignored", call_stage_name(s_call.stage));
        break;
    }
}


static void double_action(void)
{
    if (s_call.stage == CALL_RINGING) {
        ESP_LOGI(TAG, "BOOT double click: incoming -> reject");
        end_call(true, 7);
    } else if (call_primary_action(s_call.stage) == CALL_PRIMARY_HANGUP) {
        ESP_LOGI(TAG, "BOOT double click: %s -> hangup",
                 call_stage_name(s_call.stage));
        end_call(true, 1);
    } else {
        ESP_LOGI(TAG, "BOOT double click: state=%s action ignored",
                 call_stage_name(s_call.stage));
    }
}
static void rtc_dispatch(const rtc_event_t *e)
{
    if (e->kind == RTC_STARTED) {
        s_sdk_ready = true;
        ESP_LOGI(TAG, "TiRTC SYS_STARTED");
        return;
    }
    if (e->kind == RTC_STOPPED) {
        rtc_uninit();
        s_sdk_initialized = s_sdk_ready = s_sdk_stopping = false;
        s_connect_pending = false;
        s_connection = s_closing = NULL;
        finish();
        s_need_shutdown = false;
        s_shutdown_failed = false;
        s_close_deadline = 0;
        if (s_resume_after_stop) {
            s_resume_after_stop = false;
            start_sdk();
        }
        return;
    }
    if (e->kind == RTC_UNEXPECTED) {
        rtc_disconnect(e->connection);
        return;
    }
    if (e->generation != s_call.generation) {
        if (e->kind == RTC_CONNECTED) {
            if (e->connection)
                rtc_disconnect(e->connection);
            else
                rtc_connect_inputs_release();
        }
        return;
    }
    if (e->kind == RTC_CONNECTED) {
        s_connect_pending = false;
        if (e->error || !e->connection) {
            rtc_connect_inputs_release();
            ESP_LOGE(TAG, "WHIP failed: %d", e->error);
            if (s_call.stage != CALL_CLOSING)
                end_call(true, 8);
            else
                finish();
            return;
        }
        s_connection = e->connection;
        /* Rejection credentials are needed until WHIP succeeds. Thereafter all
         * local/remote hangups use this connection, not the join JSON. */
        cJSON_Delete(s_payload);
        s_payload = NULL;
        if (s_call.stage == CALL_CLOSING) {
            if (s_local_end) {
                char body[32];
                snprintf(body, sizeof(body), "{\"reason\":%u}", s_reason);
                int ret = rtc_send_command(s_connection, 0x2001, body);
                ESP_LOGI(TAG, "late connection HANGUP submit: ret=%d", ret);
            }
            s_disconnect_at = now_ms() + (s_local_end ? 150 : 0);
        } else {
            call_transport_ready(&s_call, e->generation);
            /* After WHIP is established, 0x2000/0x2001 own answer and hangup.
             * Keep the scarce C3 internal RAM with RTC/media until this
             * connection is fully released. */
            platform_set_realtime_media(true);
        }
        ESP_LOGI(TAG, "WHIP connected: state=%s", call_stage_name(s_call.stage));
    } else if (e->kind == RTC_CLOSED) {
        if (e->connection != s_connection && e->connection != s_closing)
            return;
        rtc_connect_inputs_release();
        if (e->connection == s_connection)
            s_connection = NULL;
        if (e->connection == s_closing)
            s_closing = NULL;
        if (!s_connection && !s_closing) {
            sample_audio_end();
            call_close(&s_call);
            finish();
        }
    } else if (e->kind == RTC_ERROR) {
        ESP_LOGE(TAG, "RTC error=%d generation=%lu", e->error, (unsigned long)e->generation);
        end_call(false, 8);
    } else if (e->kind == RTC_COMMAND && (s_connection == e->connection || s_connect_pending)) {
        ESP_LOGI(TAG, "SIGNAL | generation=%lu cmd=0x%04lx bytes=%lu state=%s",
                 (unsigned long)e->generation, (unsigned long)e->command, (unsigned long)e->length,
                 call_stage_name(s_call.stage));
        if (e->command == 0x2000) {
            call_peer_answered(&s_call, e->generation);
        } else if (e->command == 0x2001) {
            unsigned reason;
            if (voip_parse_hangup(e->data, &reason))
                end_call(false, reason);
            else
                ESP_LOGW(TAG, "invalid HANGUP payload ignored");
        }
    } else if (e->kind == RTC_SUBSCRIBE) {
        s_audio_stream = e->stream;
        s_audio_subscribed = true;
        sample_audio_subscribed(s_audio_stream, true);
    } else if (e->kind == RTC_UNSUBSCRIBE && e->stream == s_audio_stream) {
        s_audio_subscribed = false;
        sample_audio_subscribed(s_audio_stream, false);
    }
}
static void dispatch(event_t *e)
{
    const char *text = e->slot >= 0 ? s_messages[e->slot] : NULL;
    if (e->kind == EVENT_PLATFORM) {
        switch (e->platform.kind) {
        case PLATFORM_ONLINE:
            if (s_need_shutdown || s_sdk_stopping)
                s_resume_after_stop = true;
            else
                start_sdk();
            request_profile();
            break;
        case PLATFORM_OFFLINE:
            s_profile_ready = false;
            s_profile_dirty = true;
            s_contacts_dirty = true;
            break;
        case PLATFORM_UNBOUND:
            s_profile_ready = false;
            s_profile_dirty = true;
            s_contact_count = 0;
            s_contacts_dirty = true;
            s_need_shutdown = s_sdk_initialized;
            end_call(true, 1);
            break;
        case PLATFORM_MESSAGE:
            if (text)
                signal(text);
            break;
        case PLATFORM_RESPONSE:
            http_result(&e->platform, text);
            break;
        }
    } else if (e->kind == EVENT_USER) {
        switch (e->command) {
        case VOIP_STATUS:
            status();
            break;
        case VOIP_CONTACTS:
            start_sdk();
            if (!s_profile_ready)
                request_profile();
            else
                request_contacts();
            break;
        case VOIP_CALL:
            dial(e->contact);
            break;
        case VOIP_ANSWER:
            answer_incoming();
            break;
        case VOIP_REJECT:
            if (s_call.stage == CALL_RINGING)
                end_call(true, 7);
            break;
        case VOIP_HANGUP:
            end_call(true, 1);
            break;
        case VOIP_RX_DUMP:
            if (s_call.stage == CALL_IDLE)
                sample_audio_dump();
            else
                ESP_LOGW(TAG, "rx-dump requires idle state");
            break;
        case VOIP_RX_RECORD:
            if (s_call.stage == CALL_ACTIVE)
                sample_audio_record();
            else
                ESP_LOGW(TAG, "rx-record requires an active call");
            break;
        case VOIP_RETRY:
            platform_retry();
            if (s_need_shutdown || s_sdk_stopping) {
                s_shutdown_failed = false;
                s_resume_after_stop = true;
            } else {
                start_sdk();
                request_profile();
            }
            break;
        case VOIP_PRIMARY_ACTION:
            primary_action();
            break;
        case VOIP_DOUBLE_ACTION:
            double_action();
            break;
        }
    }
    release_slot(e->slot);
}
static void task(void *arg)
{
    (void)arg;
    event_t event;
    rtc_event_t rtc;
    for (;;) {
        memory_probe_sample();
        call_stage_t previous = s_call.stage;
        uint32_t generation = s_call.generation;
        while (xQueueReceive(s_rtc_queue, &rtc, 0) == pdTRUE)
            rtc_dispatch(&rtc);
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(5)) == pdTRUE)
            dispatch(&event);
        int64_t now = now_ms();
        if (atomic_exchange(&s_event_failure, false)) {
            ESP_LOGE(
                TAG,
                "control event lost: fail current session and stop SDK; no automatic call retry");
            s_need_shutdown = true;
            end_call(false, 8);
        }
        if (s_need_shutdown && !s_sdk_initialized) {
            /* No SDK resources exist; a stop callback cannot arrive. */
            s_need_shutdown = false;
            if (s_resume_after_stop) {
                s_resume_after_stop = false;
                start_sdk();
            }
        }
        if (s_call.stage == CALL_ACTIVE && s_connection && !s_media_started) {
            s_deadline = 0;
            sample_audio_begin(s_connection);
            sample_audio_subscribed(s_audio_stream, s_audio_subscribed);
            s_media_started = true;
            ESP_LOGI(TAG, "CONNECTED (0x2000): start duration/audio");
        }
        if (s_deadline && now >= s_deadline && s_call.stage != CALL_IDLE) {
            ESP_LOGE(TAG, "session deadline: stage=%s", call_stage_name(s_call.stage));
            end_call(true, 6);
            s_deadline = 0;
            if (s_connect_pending)
                s_need_shutdown = true;
        }
        if (s_call.stage == CALL_CLOSING && s_connection && now >= s_disconnect_at) {
            tirtc_conn_t h = s_connection;
            s_connection = NULL;
            s_closing = h;
            int ret = rtc_disconnect(h);
            ESP_LOGI(TAG, "disconnect submitted: %d", ret);
            if (ret == TIRTC_E_INVALID_HANDLE) {
                s_closing = NULL;
                finish();
            } else if (ret != 0)
                s_need_shutdown = true;
        }
        if (s_need_shutdown && s_sdk_initialized && !s_sdk_stopping && !s_shutdown_failed &&
            (!s_connection || s_connect_pending)) {
            sample_audio_end();
            int ret = rtc_stop();
            if (ret == 0)
                s_sdk_stopping = true;
            else {
                s_shutdown_failed = true;
                ESP_LOGE(TAG, "SDK stop failed: %d; state retained, explicit retry required", ret);
            }
        }
        if (s_close_deadline && now >= s_close_deadline) {
            s_close_deadline = 0;
            ESP_LOGE(TAG,
                     "close still pending: HTTP=%d WHIP=%d handle=%d stop=%d; inspect SDK/network, "
                     "state retained",
                     s_call_request_pending, s_connect_pending, s_closing != NULL, s_sdk_stopping);
            if (s_sdk_initialized && !s_sdk_stopping)
                s_need_shutdown = true;
        }
        sample_audio_tick();
        atomic_store_explicit(&s_public_state, (int)s_call.stage,
                              memory_order_release);
        wechat_voip_direction_t direction =
            s_call.stage == CALL_IDLE
                ? WECHAT_VOIP_DIRECTION_NONE
                : (s_call.outgoing ? WECHAT_VOIP_DIRECTION_OUTGOING
                                   : WECHAT_VOIP_DIRECTION_INCOMING);
        atomic_store_explicit(&s_public_direction, (int)direction,
                              memory_order_release);
        if (previous != s_call.stage || generation != s_call.generation) {
            memory_watermark_t watermark;
            memory_probe_watermark(&watermark);
            ESP_LOGI(
                TAG,
                "FLOW | generation=%lu %s->%s internal_free=%uB largest=%uB min_free=%uB "
                "sampled_low=%uB sampled_largest_at_low=%uB sampled_largest_low=%uB "
                "control_stack_hwm=%uB",
                (unsigned long)s_call.generation, call_stage_name(previous),
                call_stage_name(s_call.stage),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)watermark.lowest_free, (unsigned)watermark.largest_at_lowest_free,
                (unsigned)watermark.lowest_largest, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}
esp_err_t wechat_voip_start(unsigned version)
{
    if (s_queue || version > 2)
        return ESP_ERR_INVALID_ARG;
    s_version = version;
    atomic_store(&s_public_state, WECHAT_VOIP_IDLE);
    atomic_store(&s_public_direction, WECHAT_VOIP_DIRECTION_NONE);
    atomic_store(&s_public_end_reason, 0U);
    s_queue = xQueueCreateStatic(8, sizeof(event_t), s_queue_buffer, &s_queue_storage);
    s_rtc_queue =
        xQueueCreateStatic(16, sizeof(rtc_event_t), s_rtc_queue_buffer, &s_rtc_queue_storage);
    rtc_adapter_init(rtc_event, sample_audio_receive);
    return xTaskCreateStatic(task, "voip", sizeof(s_task_stack), NULL, 5, s_task_stack,
                             &s_task_storage) != NULL
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
