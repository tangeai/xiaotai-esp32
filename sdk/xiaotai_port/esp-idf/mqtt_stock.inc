// SPDX-License-Identifier: MIT
/* Stock esp-mqtt may run an event loop on the caller of a synchronous write.
 * The port owns dispatch; neither that callback nor WAKE runs application code. */
enum { STOCK_SLOTS = 8, STOCK_DATA_SLOTS = STOCK_SLOTS - 2, STOCK_OWNER_PRIORITY = 5 };

_Static_assert(XIAOTAI_MQTT_SUBSCRIBE_MAX <= XIAOTAI_MQTT_TOPIC_MAX, "SUBACK must fit inline");

typedef struct {
    esp_mqtt_event_t event;
    esp_mqtt_error_codes_t error;
    char topic[XIAOTAI_MQTT_TOPIC_MAX];
} stock_record_t;

struct mqtt_stock {
    xiaotai_mutex_t mailbox, writes;
    xiaotai_signal_t ready, joined;
    xiaotai_task_t owner;
    bool owner_live, ending, wake, link_lost, link_up;
    unsigned head, count, gap_after;
    int gap_error;
    size_t payload_capacity;
    char *payloads;
    stock_record_t entries[STOCK_SLOTS];
};

static bool stock_running(const xiaotai_mqtt_t *c)
{
    unsigned state = atomic_load_explicit(&c->state, memory_order_acquire);
    return state == STARTING || state == STARTED;
}

static bool stock_event_copy(mqtt_stock_t *s, unsigned tail, int32_t id,
                             const esp_mqtt_event_t *native, int *copy_error)
{
    stock_record_t *record = &s->entries[tail];
    record->event = (esp_mqtt_event_t){.event_id = id};
    if (id == MQTT_EVENT_ERROR) {
        if (native && native->error_handle) {
            record->error = *native->error_handle;
            record->event.error_handle = &record->error;
        }
        return true;
    }
    if (id == MQTT_EVENT_CONNECTED || id == MQTT_EVENT_DISCONNECTED) return true;
    if (!native) return false;
    record->event.msg_id = native->msg_id;
    if (id == MQTT_EVENT_PUBLISHED) return true;
    if (native->data_len < 0 || (size_t)native->data_len > s->payload_capacity ||
        (native->data_len && !native->data) ||
        (id == MQTT_EVENT_SUBSCRIBED && (size_t)native->data_len > XIAOTAI_MQTT_SUBSCRIBE_MAX)) {
        /* Preserve the rejected control packet ID without copying invalid data. */
        return id == MQTT_EVENT_SUBSCRIBED;
    }
    if (id == MQTT_EVENT_DATA && native->topic &&
        (native->topic_len < 0 || (size_t)native->topic_len > sizeof(record->topic))) return false;

    record->event.data_len = native->data_len;
    if (native->data_len) {
        char *data;
        if (!XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND) {
            data = s->payloads + (size_t)tail * s->payload_capacity;
        } else if (id == MQTT_EVENT_DATA) {
            data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)native->data_len);
            if (!data) {
                *copy_error = XIAOTAI_ERR_NO_MEM;
                return false;
            }
        } else {
            /* SUBACK has no topic; its bounded grants fit in the record. */
            data = record->topic;
        }
        memcpy(data, native->data, (size_t)native->data_len);
        record->event.data = data;
    }
    if (id == MQTT_EVENT_DATA) {
        if (native->topic) {
            memcpy(record->topic, native->topic, (size_t)native->topic_len);
            record->event.topic = record->topic;
            record->event.topic_len = native->topic_len;
        }
        record->event.total_data_len = native->total_data_len;
        record->event.current_data_offset = native->current_data_offset;
        record->event.qos = native->qos;
        record->event.retain = native->retain;
        record->event.dup = native->dup;
    }
    return true;
}

static void stock_event(void *arg, esp_event_base_t base, int32_t id, void *event_ptr)
{
    (void)base;
    xiaotai_mqtt_t *c = arg;
    mqtt_stock_t *s = c->stock;
    const unsigned state = atomic_load_explicit(&c->state, memory_order_acquire);
    if (state != STARTING && state != STARTED && state != STOPPING) return;
    switch (id) {
    case MQTT_EVENT_CONNECTED: case MQTT_EVENT_DISCONNECTED: case MQTT_EVENT_ERROR:
    case MQTT_EVENT_PUBLISHED: case MQTT_EVENT_SUBSCRIBED: case MQTT_EVENT_DATA: break;
    default: return;
    }

    /* Never wait for a slot while the SDK might hold its recursive API lock.
     * Serialize admission and copying, including on-demand allocation, so
     * a failed allocation has the same position as a full mailbox. No native
     * calls or application handlers run under this mutex. */
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    if (id == MQTT_EVENT_CONNECTED || id == MQTT_EVENT_DISCONNECTED) {
        s->link_up = id == MQTT_EVENT_CONNECTED;
    }
    unsigned tail = (s->head + s->count) % STOCK_SLOTS;
    bool full = s->count >= (id == MQTT_EVENT_DATA ? STOCK_DATA_SLOTS : STOCK_SLOTS);
    int copy_error = full ? XIAOTAI_ERR_NO_MEM : id;
    if (!(id == MQTT_EVENT_DATA && s->gap_error) && !full && stock_event_copy(s, tail, id, event_ptr, &copy_error)) {
        ++s->count;
    } else {
        if (!s->gap_error) {
            s->gap_error = copy_error;
            s->gap_after = s->count;
        }
        if (id == MQTT_EVENT_CONNECTED || id == MQTT_EVENT_DISCONNECTED) s->link_lost = true;
    }
    xiaotai_mutex_unlock(&s->mailbox);
    xiaotai_signal_give(&s->ready);
}

static void stock_owner(void *arg)
{
    xiaotai_mqtt_t *c = arg;
    mqtt_stock_t *s = c->stock;
    for (;;) {
        xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
        if (s->gap_error && !s->gap_after) {
            int error = s->gap_error;
            bool link_lost = s->link_lost, link_up = s->link_up;
            s->gap_error = 0;
            s->link_lost = false;
            xiaotai_mutex_unlock(&s->mailbox);
            /* Keep records admitted before the gap. Drop later DATA until the
             * owner has reset its fragment state and reported the first loss. */
            c->topic_size = 0;
            protocol_error(c, error, 0);
            if (link_lost) {
                native_event(c, NULL, MQTT_EVENT_DISCONNECTED, NULL);
                if (link_up) native_event(c, NULL, MQTT_EVENT_CONNECTED, NULL);
            }
            continue;
        }
        if (s->count) {
            esp_mqtt_event_t *event = &s->entries[s->head].event;
            xiaotai_mutex_unlock(&s->mailbox);
            native_event(c, NULL, event->event_id, event);
            if (XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND && event->event_id == MQTT_EVENT_DATA && event->data) {
                xiaotai_mem_free(event->data);
            }
            xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
            s->head = (s->head + 1U) % STOCK_SLOTS;
            --s->count;
            if (s->gap_after) --s->gap_after;
            xiaotai_mutex_unlock(&s->mailbox);
            continue;
        }
        bool ending = s->ending, wake = s->wake;
        s->wake = false;
        xiaotai_mutex_unlock(&s->mailbox);
        if (ending) break;
        if (wake) native_event(c, NULL, MQTT_USER_EVENT, NULL);
        else xiaotai_signal_take(&s->ready, XIAOTAI_WAIT_FOREVER);
    }
    xiaotai_signal_give(&s->joined);
    xiaotai_task_exit();
}

static xiaotai_err_t stock_attach(xiaotai_mqtt_t *c)
{
    mqtt_stock_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    c->stock = s;
    s->payload_capacity = c->config.value.rx_buffer_bytes;
    if (s->payload_capacity < XIAOTAI_MQTT_SUBSCRIBE_MAX) s->payload_capacity = XIAOTAI_MQTT_SUBSCRIBE_MAX;
    if (!XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND) {
        s->payloads = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, STOCK_SLOTS * s->payload_capacity);
    }
    xiaotai_err_t rc = (s->payloads || XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND)
                           ? xiaotai_mutex_init(&s->mailbox) : XIAOTAI_ERR_NO_MEM;
    if (rc == XIAOTAI_OK) {
        rc = xiaotai_mutex_init(&s->writes);
        if (rc == XIAOTAI_OK) {
            rc = xiaotai_signal_init(&s->ready);
            if (rc == XIAOTAI_OK) {
                rc = xiaotai_signal_init(&s->joined);
                if (rc == XIAOTAI_OK) {
                    rc = esp_mqtt_client_register_event(c->native, MQTT_EVENT_ANY, stock_event, c);
                    if (rc == XIAOTAI_OK) return rc;
                    xiaotai_signal_deinit(&s->joined);
                }
                xiaotai_signal_deinit(&s->ready);
            }
            xiaotai_mutex_deinit(&s->writes);
        }
        xiaotai_mutex_deinit(&s->mailbox);
    }
    xiaotai_mem_free(s->payloads);
    xiaotai_mem_free(s);
    c->stock = NULL;
    return rc;
}

static bool stock_owner_current(xiaotai_mqtt_t *c)
{
    mqtt_stock_t *s = c->stock;
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    bool current = s->owner_live && xiaotai_task_is_current(&s->owner);
    xiaotai_mutex_unlock(&s->mailbox);
    return current;
}

static void stock_join(mqtt_stock_t *s)
{
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    s->ending = true;
    xiaotai_mutex_unlock(&s->mailbox);
    xiaotai_signal_give(&s->ready);
    xiaotai_signal_take(&s->joined, XIAOTAI_WAIT_FOREVER);
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    s->owner_live = false;
    xiaotai_mutex_unlock(&s->mailbox);
}

static xiaotai_err_t stock_start(xiaotai_mqtt_t *c)
{
    mqtt_stock_t *s = c->stock;
    const uint32_t event_stack = c->config.value.event_stack_bytes;
    if (event_stack && (event_stack < 2560U || event_stack > 16384U)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    s->ending = s->wake = s->link_up = false;
    xiaotai_mutex_unlock(&s->mailbox);
    const xiaotai_task_config_t owner = {
        .name = "xt_mqtt_events", .entry = stock_owner, .arg = c,
        .stack_bytes = event_stack ? event_stack : c->config.value.task_stack_bytes,
        .stack = XIAOTAI_MEM_INTERNAL,
        .priority = STOCK_OWNER_PRIORITY, .core = XIAOTAI_CORE_ANY,
    };
    xiaotai_task_t handle;
    xiaotai_err_t rc = xiaotai_task_create(&owner, &handle);
    if (rc != XIAOTAI_OK) return rc;
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    s->owner = handle;
    s->owner_live = true;
    xiaotai_mutex_unlock(&s->mailbox);
    rc = esp_mqtt_client_start(c->native);
    if (rc != XIAOTAI_OK) stock_join(s);
    return rc;
}

static xiaotai_err_t stock_stop(xiaotai_mqtt_t *c)
{
    mqtt_stock_t *s = c->stock;
    xiaotai_mutex_lock(&s->writes, XIAOTAI_WAIT_FOREVER);
    const xiaotai_err_t rc = esp_mqtt_client_stop(c->native);
    xiaotai_mutex_unlock(&s->writes);
    if (rc == XIAOTAI_OK) stock_join(s);
    return rc;
}

static xiaotai_err_t stock_destroy(xiaotai_mqtt_t *c)
{
    xiaotai_err_t rc = esp_mqtt_client_destroy(c->native);
    if (rc != XIAOTAI_OK) return rc;
    mqtt_stock_t *s = c->stock;
    xiaotai_signal_deinit(&s->joined);
    xiaotai_signal_deinit(&s->ready);
    xiaotai_mutex_deinit(&s->writes);
    xiaotai_mutex_deinit(&s->mailbox);
    xiaotai_mem_free(s->payloads);
    xiaotai_mem_free(s);
    return XIAOTAI_OK;
}

static bool stock_wake(xiaotai_mqtt_t *c)
{
    mqtt_stock_t *s = c->stock;
    xiaotai_mutex_lock(&s->mailbox, XIAOTAI_WAIT_FOREVER);
    bool active = stock_running(c) && s->owner_live;
    if (active) {
        s->wake = true;
        xiaotai_signal_give(&s->ready);
    }
    xiaotai_mutex_unlock(&s->mailbox);
    return active;
}

static bool stock_connected(xiaotai_mqtt_t *c)
{
    return stock_running(c) && atomic_load_explicit(&c->connected, memory_order_acquire) &&
           esp_mqtt_client_get_state(c->native) == MQTT_CLIENT_STATE_CONNECTED;
}

static xiaotai_err_t stock_subscribe(xiaotai_mqtt_t *c, const esp_mqtt_topic_t *topics, int count, int *id)
{
    xiaotai_err_t rc = XIAOTAI_ERR_INVALID_STATE;
    xiaotai_mutex_lock(&c->stock->writes, XIAOTAI_WAIT_FOREVER);
    if (stock_connected(c)) {
        *id = esp_mqtt_client_subscribe_multiple(c->native, topics, count);
        rc = *id > 0 ? XIAOTAI_OK : XIAOTAI_FAIL;
    }
    xiaotai_mutex_unlock(&c->stock->writes);
    return rc;
}

static xiaotai_err_t stock_publish(xiaotai_mqtt_t *c, const char *topic, const char *body, int size,
                                   int qos, xiaotai_mqtt_send_t send, size_t bytes, int *id)
{
    xiaotai_err_t rc = XIAOTAI_ERR_INVALID_STATE;
    const bool queued = send == XIAOTAI_MQTT_SEND_QUEUED;
    xiaotai_mutex_lock(&c->stock->writes, XIAOTAI_WAIT_FOREVER);
    if (stock_connected(c)) {
        const size_t limit = c->config.value.outbox_limit_bytes;
        /* A conservative encoded-size check among port writers. The public
         * SDK API does not make broker state and outbox admission atomic. */
        if (limit && (queued || qos) && (size_t)esp_mqtt_client_get_outbox_size(c->native) + bytes > limit) {
            rc = XIAOTAI_ERR_NO_MEM;
        } else {
            *id = queued ? esp_mqtt_client_enqueue(c->native, topic, body, size, qos, 0, true)
                         : esp_mqtt_client_publish(c->native, topic, body, size, qos, 0);
            rc = *id >= 0 && (!qos || *id > 0) ? XIAOTAI_OK : (*id == -2 ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL);
        }
    }
    xiaotai_mutex_unlock(&c->stock->writes);
    return rc;
}
