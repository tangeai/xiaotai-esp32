// SPDX-License-Identifier: MIT
/* Dormant overlay for the pinned native codec. Returning 0 uses its existing
 * encoding failure path; explicit protocol response IDs are unchanged. */
#ifdef MQTT_DISABLE_API_LOCKS
#error "XiaoTai packet-ID reservation requires the native MQTT API mutex"
#endif

static uint16_t append_message_id(mqtt_connection_t *connection, uint16_t message_id)
{
    if (!message_id && connection->message_id_available) {
        if (connection->outbound_message.length + 2 > connection->buffer_length) return 0;
#if MQTT_MSG_ID_INCREMENTAL
        const uint16_t first = connection->last_message_id == UINT16_MAX ? 1U : connection->last_message_id + 1U;
#else
        uint16_t first = (uint16_t)platform_random(UINT16_MAX);
        if (!first) first = 1U;
#endif
        message_id = connection->message_id_available(connection->message_id_ctx, first);
        if (!message_id) return 0;
#if MQTT_MSG_ID_INCREMENTAL
        connection->last_message_id = message_id;
#endif
    }
    /* No callback: the stock codec's allocation behavior stays unchanged. */
    while (!message_id) {
#if MQTT_MSG_ID_INCREMENTAL
        message_id = ++connection->last_message_id;
#else
        message_id = platform_random(UINT16_MAX);
#endif
    }
    if (connection->outbound_message.length + 2 > connection->buffer_length) return 0;
    connection->buffer[connection->outbound_message.length++] = message_id >> 8;
    connection->buffer[connection->outbound_message.length++] = message_id & 0xff;
    return message_id;
}
