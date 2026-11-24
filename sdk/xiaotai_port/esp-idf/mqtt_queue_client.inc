// SPDX-License-Identifier: MIT
/* Shared by the generated native synchronous and queued transactions. */
static bool xt_mqtt_queue_wire_size(const char *topic, int length, int qos, size_t *wire)
{
    if (!topic || !*topic || length < 0 || qos < 0 || qos > 1) return false;
    const size_t topic_bytes = strlen(topic);
    if (topic_bytes > UINT16_MAX) return false;
    const size_t remaining = 2U + topic_bytes + (qos ? 2U : 0U) + (size_t)length;
    if (remaining > 268435455U) return false;
    size_t header = 1U, encoded = remaining;
    do { ++header; encoded /= 128U; } while (encoded);
    *wire = header + remaining;
    return true;
}

static void xt_mqtt_queue_state(esp_mqtt_client_handle_t client, mqtt_client_state_t state)
{
    xt_mqtt_outbox_lock(client->outbox);
    client->state = state;
    xt_mqtt_outbox_unlock(client->outbox);
}

static bool xt_mqtt_queue_fits(esp_mqtt_client_handle_t client, size_t wire)
{
    const uint64_t limit = client->config->outbox_limit;
    const uint64_t used = outbox_get_size(client->outbox);
    if (!limit || (used <= limit && wire <= limit - used)) return true;
    xt_mqtt_outbox_note_failure(client->outbox, XT_MQTT_QUEUE_QUOTA);
    return false;
}
