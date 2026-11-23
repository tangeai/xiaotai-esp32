// SPDX-License-Identifier: MIT
#pragma once
/* Same-component native integration. The native api_lock protects normal records;
 * only WAKE uses the independent atomic mailbox. No additional task exists. */
#include "mqtt_client_priv.h"
#include "mqtt_owner_mailbox.h"
#include "freertos/task.h"

#ifdef MQTT_DISABLE_API_LOCKS
#error "XiaoTai MQTT owner dispatch requires the native recursive API lock"
#endif

ESP_EVENT_DECLARE_BASE(MQTT_EVENTS);

#define XT_MQTT_OWNER_WORK (1U << 3)
#define XT_MQTT_OWNER_READY (1U << 4)
#define XT_MQTT_OWNER_STARTED (1U << 5)

typedef struct {
    esp_mqtt_event_t event;
    esp_mqtt_error_codes_t error;
    bool has_error;
} xt_mqtt_owner_record_t;

typedef struct {
    xt_mqtt_owner_mailbox_t wake;
    _Atomic(TaskHandle_t) owner;
    uint32_t epoch;
    esp_event_handler_t handler;
    void *ctx;
    xt_mqtt_owner_record_t records[2];
    unsigned count;
    bool callback_active, prepared;
} xt_mqtt_owner_bridge_t;

static inline xt_mqtt_owner_bridge_t *xt_mqtt_owner_bridge(esp_mqtt_client_handle_t client)
{
    return client->xiaotai_owner_bridge;
}

/* [management, stopped] The bridge and handler context outlive the native
 * task. A failed native start must be followed by mailbox_stop(). */
static inline void xt_mqtt_owner_prepare(esp_mqtt_client_handle_t client, xt_mqtt_owner_bridge_t *bridge,
                                          esp_event_handler_t handler, void *ctx)
{
    xt_mqtt_mailbox_init(&bridge->wake);
    bridge->epoch = xt_mqtt_mailbox_start(&bridge->wake);
    atomic_init(&bridge->owner, NULL);
    bridge->handler = handler;
    bridge->ctx = ctx;
    bridge->count = 0;
    bridge->callback_active = bridge->prepared = false;
    client->xiaotai_owner_bridge = bridge;
    xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_READY);
}

static inline void xt_mqtt_owner_emit(xt_mqtt_owner_bridge_t *bridge, xt_mqtt_owner_record_t *record)
{
    if (record->has_error) record->event.error_handle = &record->error;
    bridge->callback_active = true;
    bridge->handler(bridge->ctx, MQTT_EVENTS, record->event.event_id, &record->event);
    bridge->callback_active = false;
}

/* [native owner, api_lock held] Two events per pass keeps a handler that
 * produces more events from monopolizing the task. Remaining records retain
 * their order and wake the next native safe point. Never drain recursively. */
static inline void xt_mqtt_owner_drain(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    if (!bridge || bridge->owner != xTaskGetCurrentTaskHandle() || bridge->callback_active) return;
    unsigned budget = 2;
    while (bridge->count && budget--) {
        xt_mqtt_owner_record_t record = bridge->records[0];
        if (--bridge->count) bridge->records[0] = bridge->records[1];
        if (!bridge->count) xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_READY);
        xt_mqtt_owner_emit(bridge, &record);
    }
    if (bridge->count) xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_WORK);
}

static inline void xt_mqtt_owner_safe_point(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    if (!bridge) return;
    xEventGroupClearBits(client->status_bits, XT_MQTT_OWNER_WORK);
    xt_mqtt_owner_drain(client);
    if (!bridge->count && xt_mqtt_mailbox_take(&bridge->wake, bridge->epoch)) {
        xt_mqtt_owner_record_t wake = {.event = {.client = client, .event_id = MQTT_USER_EVENT}};
        xt_mqtt_owner_emit(bridge, &wake);
    }
}

static inline void xt_mqtt_owner_begin(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    if (bridge) {
        MQTT_API_LOCK(client);
        bridge->owner = xTaskGetCurrentTaskHandle();
        xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_STARTED);
        MQTT_API_UNLOCK(client);
    }
}

static inline void xt_mqtt_owner_end(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    if (!bridge) return;
    MQTT_API_LOCK(client);
    bridge->prepared = true; /* Ending callbacks cannot generate another write. */
    xt_mqtt_owner_safe_point(client);
    xt_mqtt_mailbox_stop(&bridge->wake);
    bridge->owner = NULL;
    xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_READY);
    MQTT_API_UNLOCK(client);
}

static inline bool xt_mqtt_owner_wake(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    if (!bridge || !xt_mqtt_mailbox_request(&bridge->wake)) return false;
    /* Interrupts the native reconnect wait, not DNS/TLS or socket poll. */
    xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_WORK);
    return true;
}

/* [synchronous caller, NOT holding api_lock] Reserves room for the operation's
 * at most ERROR + DISCONNECTED. Waiting is only allowed for synchronous
 * publish/subscribe/stop; a queued publish must never use this wait path.
 * True returns with api_lock held once. False releases the attempted lock:
 * the caller returns its existing write-failure result before native I/O,
 * packet ID generation or outbox acceptance. No new ERROR is generated. */
static inline bool xt_mqtt_owner_sync_lock(esp_mqtt_client_handle_t client)
{
    for (;;) {
        MQTT_API_LOCK(client);
        xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
        if (!bridge) return true;
        if (!client->run || bridge->prepared) {
            MQTT_API_UNLOCK(client);
            return false;
        }
        if (bridge->owner == xTaskGetCurrentTaskHandle()) {
            if (!bridge->callback_active && !bridge->prepared) xt_mqtt_owner_drain(client);
            /* Uniformly reserve worst-case ERROR + DISCONNECTED. A prepared
             * normal event also owns this order boundary until delivered. */
            if (bridge->count || bridge->prepared) {
                MQTT_API_UNLOCK(client);
                return false;
            }
            return true;
        }
        if (!bridge->count) return true;
        MQTT_API_UNLOCK(client);
        xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_WORK);
        xEventGroupWaitBits(client->status_bits, XT_MQTT_OWNER_READY, false, true, portMAX_DELAY);
    }
}

/* [native dispatch, api_lock held] The port routes only its bounded synchronous
 * write paths here. Unsupported direct calls to vendor APIs are not an extra
 * producer. A nonzero return is an integration invariant failure, never OK. */
static inline esp_err_t xt_mqtt_owner_dispatch(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = xt_mqtt_owner_bridge(client);
    xt_mqtt_owner_record_t record = {.event = client->event};
    record.event.client = client;
    record.event.protocol_ver = client->mqtt_state.connection.information.protocol_ver;
    if (record.event.event_id == MQTT_EVENT_ERROR && record.event.error_handle) {
        record.error = *record.event.error_handle;
        record.has_error = true;
    }
    if (bridge->owner == xTaskGetCurrentTaskHandle() && !bridge->callback_active) {
        /* The current normal event was generated before any new calls made
         * by older callbacks. Hold its place while draining earlier records. */
        bridge->prepared = true;
        xt_mqtt_owner_drain(client);
        bridge->prepared = false;
        xt_mqtt_owner_emit(bridge, &record);
        xt_mqtt_owner_drain(client);
        return ESP_OK;
    }
    if ((record.event.event_id != MQTT_EVENT_ERROR && record.event.event_id != MQTT_EVENT_DISCONNECTED) ||
        bridge->count == 2U) return ESP_ERR_INVALID_STATE;
    /* These two events carry no loaned topic or payload. Error fields above
     * are copied before native code may overwrite its persistent error box. */
    record.event.topic = NULL;
    record.event.topic_len = 0;
    record.event.data = NULL;
    record.event.data_len = 0;
    record.event.error_handle = NULL;
    bridge->records[bridge->count++] = record;
    xEventGroupClearBits(client->status_bits, XT_MQTT_OWNER_READY);
    xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_WORK);
    return ESP_OK;
}
