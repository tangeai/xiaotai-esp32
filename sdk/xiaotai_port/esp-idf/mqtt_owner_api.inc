// SPDX-License-Identifier: MIT
/* Included only by the hash-checked generated native client translation unit. */
#include "mqtt_owner_api.h"
#include "mqtt_native_api.h"
#include "esp_heap_caps.h"

bool xiaotai_idf_mqtt_owner_current(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = client ? xt_mqtt_owner_bridge(client) : NULL;
    return bridge && atomic_load_explicit(&bridge->owner, memory_order_acquire) == xTaskGetCurrentTaskHandle();
}

esp_err_t xiaotai_idf_mqtt_owner_attach(esp_mqtt_client_handle_t client,
                                       esp_event_handler_t handler, void *ctx)
{
    if (!client || !handler) return ESP_ERR_INVALID_ARG;
    if (client->xiaotai_owner_bridge || client->run) return ESP_ERR_INVALID_STATE;
    xt_mqtt_owner_bridge_t *bridge = heap_caps_calloc(1, sizeof(*bridge), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bridge) return ESP_ERR_NO_MEM;
    xt_mqtt_owner_prepare(client, bridge, handler, ctx);
    xt_mqtt_mailbox_stop(&bridge->wake);
#ifdef XIAOTAI_MQTT_NATIVE_ID_RESERVATION
    client->mqtt_state.connection.message_id_available = xt_mqtt_outbox_available_id;
    client->mqtt_state.connection.message_id_ctx = client->outbox;
#endif
    xEventGroupSetBits(client->status_bits, STOPPED_BIT);
    return ESP_OK;
}

esp_err_t xiaotai_idf_mqtt_owner_start(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = client ? xt_mqtt_owner_bridge(client) : NULL;
    if (!bridge || xiaotai_idf_mqtt_owner_current(client)) return ESP_ERR_INVALID_STATE;
    if (!(xEventGroupGetBits(client->status_bits) & STOPPED_BIT)) return ESP_ERR_INVALID_STATE;
    bridge->count = 0;
    bridge->callback_active = bridge->prepared = false;
    bridge->epoch = xt_mqtt_mailbox_start(&bridge->wake);
    xEventGroupClearBits(client->status_bits, XT_MQTT_OWNER_STARTED | XT_MQTT_OWNER_WORK);
    xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_READY);
    esp_err_t rc = esp_mqtt_client_start(client);
    if (rc != ESP_OK) {
        xt_mqtt_mailbox_stop(&bridge->wake);
        return rc;
    }
    /* The stock task sets run only when scheduled. Without this handshake an
     * immediate stop may fail and a successful destroy can free a live task. */
    xEventGroupWaitBits(client->status_bits, XT_MQTT_OWNER_STARTED, false, true, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t xiaotai_idf_mqtt_owner_stop(esp_mqtt_client_handle_t client)
{
    if (!client || !xt_mqtt_owner_bridge(client) || xiaotai_idf_mqtt_owner_current(client))
        return ESP_ERR_INVALID_STATE;
    if (xEventGroupGetBits(client->status_bits) & STOPPED_BIT) return ESP_OK;
    esp_err_t rc = esp_mqtt_client_stop(client);
    if (rc != ESP_OK) {
        /* With a confirmed start and no owner reentry the stock failure means
         * the task set run=false itself. It still owes its native exit fence. */
        xEventGroupWaitBits(client->status_bits, STOPPED_BIT, false, true, portMAX_DELAY);
    }
    return ESP_OK;
}

esp_err_t xiaotai_idf_mqtt_owner_destroy(esp_mqtt_client_handle_t client)
{
    xt_mqtt_owner_bridge_t *bridge = client ? xt_mqtt_owner_bridge(client) : NULL;
    if (!bridge || xiaotai_idf_mqtt_owner_current(client) ||
        !(xEventGroupGetBits(client->status_bits) & STOPPED_BIT)) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = esp_mqtt_client_destroy(client);
    if (rc == ESP_OK) heap_caps_free(bridge);
    return rc;
}

bool xiaotai_idf_mqtt_owner_wake(esp_mqtt_client_handle_t client)
{
    return client && xt_mqtt_owner_wake(client);
}

esp_err_t xiaotai_idf_mqtt_subscribe_connected(esp_mqtt_client_handle_t client,
                                             const esp_mqtt_topic_t *topics, int count, int *message_id)
{
    if (!message_id) return ESP_ERR_INVALID_ARG;
    *message_id = 0;
    if (!client || !topics || count < 1 || !xt_mqtt_owner_bridge(client)) return ESP_ERR_INVALID_ARG;
    /* ERROR/DISCONNECTED may still be pending on the owner while the native
     * state is already disconnected. Do not classify from the port mirror. */
    MQTT_API_LOCK(client);
    bool connected = client->run && client->state == MQTT_STATE_CONNECTED;
    MQTT_API_UNLOCK(client);
    if (!connected) return ESP_ERR_INVALID_STATE;
    if (!xt_mqtt_owner_sync_lock(client)) {
        MQTT_API_LOCK(client);
        connected = client->run && client->state == MQTT_STATE_CONNECTED;
        MQTT_API_UNLOCK(client);
        return connected ? ESP_FAIL : ESP_ERR_INVALID_STATE;
    }
    if (!client->run || client->state != MQTT_STATE_CONNECTED) {
        MQTT_API_UNLOCK(client);
        return ESP_ERR_INVALID_STATE;
    }
    /* The recursive native entry uses the same mutex and its exact short
     * transaction/write body. No other state transition can split this test
     * from submission; its allocation/quota/write failures stay plain FAIL. */
    const int id = esp_mqtt_client_subscribe_multiple(client, topics, count);
    MQTT_API_UNLOCK(client);
    if (id <= 0) return ESP_FAIL;
    *message_id = id;
    return ESP_OK;
}
