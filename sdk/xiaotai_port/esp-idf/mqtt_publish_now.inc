// SPDX-License-Identifier: MIT
/* The generator supplies xt_mqtt_publish_transaction from the pinned native
 * publish body; only local-failure rollback differs. Not wired into a port. */
esp_err_t xiaotai_idf_mqtt_publish_now(esp_mqtt_client_handle_t client, const char *topic,
                                     const char *data, int len, int qos, size_t wire_bytes,
                                     int *message_id);

esp_err_t xiaotai_idf_mqtt_publish_now(esp_mqtt_client_handle_t client, const char *topic,
                                     const char *data, int len, int qos, size_t wire_bytes,
                                     int *message_id)
{
    if (!message_id) return ESP_ERR_INVALID_ARG;
    *message_id = 0;
    if (!client || !topic || !*topic || len < 0 || (!!len != !!data) ||
        qos < 0 || qos > 1 || wire_bytes <= (size_t)len) return ESP_ERR_INVALID_ARG;
#ifndef XIAOTAI_MQTT_NATIVE_ID_RESERVATION
    if (qos) return ESP_ERR_NOT_SUPPORTED;
#endif

    /* SEND_NOW writes synchronously, so this is the blocking native lock,
     * unlike the queued-admission proof's zero-tick try-lock. */
#ifdef XIAOTAI_MQTT_OWNER_BRIDGE
    if (!xt_mqtt_owner_sync_lock(client)) return ESP_FAIL;
#else
    MQTT_API_LOCK(client);
#endif
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (client->state == MQTT_STATE_CONNECTED &&
        client->mqtt_state.connection.information.protocol_ver == MQTT_PROTOCOL_V_3_1_1) {
        const uint64_t limit = client->config->outbox_limit;
        const uint64_t used = outbox_get_size(client->outbox);
        if (qos && limit && (used > limit || wire_bytes > limit - used)) {
            result = ESP_ERR_NO_MEM;
        } else {
#ifdef XIAOTAI_MQTT_NATIVE_ID_RESERVATION
            client->mqtt_state.connection.message_id_available = xt_mqtt_outbox_available_id;
            client->mqtt_state.connection.message_id_ctx = client->outbox;
#endif
            const int id = xt_mqtt_publish_transaction(client, topic, data, len, qos, 0);
            if (id >= 0) {
                *message_id = qos ? id : 0;
                result = ESP_OK;
            } else result = id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }
    MQTT_API_UNLOCK(client);
    return result;
}
