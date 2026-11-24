// SPDX-License-Identifier: MIT
/* The native admission transaction serializes encoding through enqueue.
 * Standalone codec tests use the API mutex; QUEUED uses the short outbox lock.
 * Every live outbound transaction is in this native box; packet ID 0 is QoS0.
 * Scan 256 IDs at a time rather than keeping an 8-KiB bitmap per client. */
uint16_t xt_mqtt_outbox_available_id(void *ctx, uint16_t first)
{
    outbox_handle_t outbox = ctx;
    if (!outbox || !first) return 0;
    for (uint32_t checked = 0; checked < UINT16_MAX; checked += 256U) {
        uint32_t used[8] = {0};
        const uint32_t count = UINT16_MAX - checked < 256U ? UINT16_MAX - checked : 256U;
        outbox_item_t *item;
        STAILQ_FOREACH(item, outbox->list, next) {
            if (item->msg_id < 1 || item->msg_id > UINT16_MAX) continue;
            const uint32_t id = (uint32_t)item->msg_id;
            const uint32_t distance = id >= first ? id - first : UINT16_MAX - first + id;
            if (distance < checked || distance - checked >= count) continue;
            const uint32_t bit = distance - checked;
            used[bit / 32U] |= UINT32_C(1) << (bit % 32U);
        }
        for (uint32_t bit = 0; bit < count; ++bit) {
            if (used[bit / 32U] & (UINT32_C(1) << (bit % 32U))) continue;
            const uint32_t offset = first + checked + bit;
            return offset > UINT16_MAX ? (uint16_t)(offset - UINT16_MAX) : (uint16_t)offset;
        }
    }
    return 0;
}
