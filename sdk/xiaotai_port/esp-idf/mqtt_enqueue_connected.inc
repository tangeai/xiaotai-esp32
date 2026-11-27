// SPDX-License-Identifier: MIT
/* Appended to the exact esp-mqtt 1.1.0 client source in an isolated build. */
#ifdef MQTT_DISABLE_API_LOCKS
#error "XiaoTai connected enqueue needs the native MQTT API mutex"
#endif

/* wire_bytes is the complete validated MQTT 3.1.1 PUBLISH frame size from
 * mqtt_port_publish_size(), not just the payload size. */
esp_err_t xiaotai_idf_mqtt_enqueue_connected(esp_mqtt_client_handle_t client, const char *topic,
                                             const char *data, int len, int qos, size_t wire_bytes,
                                             int *message_id);

esp_err_t xiaotai_idf_mqtt_enqueue_connected(esp_mqtt_client_handle_t client, const char *topic,
                                             const char *data, int len, int qos, size_t wire_bytes,
                                             int *message_id)
{
    if (!message_id) return ESP_ERR_INVALID_ARG;
    *message_id = 0;
    size_t checked_wire;
    if (!client || (!data && len) ||
        !xt_mqtt_queue_wire_size(topic, len, qos, &checked_wire) || checked_wire != wire_bytes)
        return ESP_ERR_INVALID_ARG;

    /* No api_lock: the native worker may hold it across network I/O. Every
     * holder of this separate short lock does CPU/heap work only. */
    xt_mqtt_outbox_lock(client->outbox);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (client->state == MQTT_STATE_CONNECTED &&
        client->mqtt_state.connection.information.protocol_ver == MQTT_PROTOCOL_V_3_1_1) {
        if (!xt_mqtt_queue_fits(client, wire_bytes)) {
            result = ESP_ERR_NO_MEM;
        } else {
            /* The native codec reserves five header bytes before compacting
             * to the real fixed header (2..5 bytes). Keep the allocation base. */
            const size_t remaining = 2U + strlen(topic) + (qos ? 2U : 0U) + (size_t)len;
            const size_t capacity = remaining + 5U;
            uint8_t *buffer = heap_caps_malloc(capacity, MQTT_OUTBOX_MEMORY);
            if (!buffer) {
                xt_mqtt_outbox_note_failure(client->outbox, XT_MQTT_QUEUE_ALLOC);
                result = ESP_ERR_NO_MEM;
            } else {
                mqtt_connection_t codec = {0};
                codec.buffer = buffer;
                codec.buffer_length = capacity;
#if MQTT_MSG_ID_INCREMENTAL
                codec.last_message_id = client->mqtt_state.connection.last_message_id;
#endif
                codec.message_id_available = xt_mqtt_outbox_available_id;
                codec.message_id_ctx = client->outbox;
                uint16_t id = 0;
                mqtt_message_t *packet = mqtt_msg_publish(&codec, topic, data, len, qos, 0, &id);
                if (packet->length != wire_bytes || packet->fragmented_msg_total_length || (qos && !id)) {
                    xt_mqtt_outbox_note_failure(client->outbox, qos && !id ? XT_MQTT_QUEUE_IDS : XT_MQTT_QUEUE_CODEC);
                    result = qos && !id ? ESP_ERR_NO_MEM : ESP_FAIL;
                } else {
                    memmove(buffer, packet->data, packet->length);
                    if (xt_mqtt_outbox_adopt(client->outbox, buffer, (int)wire_bytes, id,
                                              mqtt_get_type(buffer), qos, platform_tick_get_ms())) {
#if MQTT_MSG_ID_INCREMENTAL
                        client->mqtt_state.connection.last_message_id = codec.last_message_id;
#endif
                        *message_id = id;
                        buffer = NULL;
                        result = ESP_OK;
                    } else result = ESP_ERR_NO_MEM;
                }
                free(buffer);
            }
        }
    }
    xt_mqtt_outbox_unlock(client->outbox);
#ifdef XIAOTAI_MQTT_OWNER_BRIDGE
    if (result == ESP_OK && xt_mqtt_owner_bridge(client))
        xEventGroupSetBits(client->status_bits, XT_MQTT_OWNER_WORK);
#endif
    return result;
}
