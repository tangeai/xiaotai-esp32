// SPDX-License-Identifier: MIT
/* One native outbox. This lock never covers I/O, callbacks or logging.
 * Client transactions may nest the ordinary outbox operations below. */
void xt_mqtt_outbox_lock(outbox_handle_t outbox)
{
    xSemaphoreTakeRecursive(outbox->admission, portMAX_DELAY);
}

void xt_mqtt_outbox_unlock(outbox_handle_t outbox)
{
    xSemaphoreGiveRecursive(outbox->admission);
}

void xt_mqtt_outbox_note_failure(outbox_handle_t outbox, unsigned phase)
{
    if (phase >= 4U) return;
    xt_mqtt_outbox_lock(outbox);
    ++outbox->failures[phase];
    xt_mqtt_outbox_unlock(outbox);
}

void xt_mqtt_outbox_failures(outbox_handle_t outbox, uint32_t counts[4])
{
    xt_mqtt_outbox_lock(outbox);
    memcpy(counts, outbox->failures, sizeof(outbox->failures));
    xt_mqtt_outbox_unlock(outbox);
}

/* The caller retains buffer on failure; on success this is the native node's
 * sole packet allocation, released by the original outbox lifetime. */
outbox_item_handle_t xt_mqtt_outbox_adopt(outbox_handle_t outbox, uint8_t *buffer,
                                         int length, int id, int type, int qos, outbox_tick_t tick)
{
    outbox_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        xt_mqtt_outbox_note_failure(outbox, XT_MQTT_QUEUE_ALLOC);
        return NULL;
    }
    item->buffer = (char *)buffer;
    item->len = length;
    item->msg_id = id;
    item->msg_type = type;
    item->msg_qos = qos;
    item->tick = tick;
    item->pending = QUEUED;
    xt_mqtt_outbox_lock(outbox);
    STAILQ_INSERT_TAIL(outbox->list, item, next);
    outbox->size += length;
    xt_mqtt_outbox_unlock(outbox);
    return item;
}
