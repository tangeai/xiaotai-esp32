// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"
#include "signal_pool.h"
#include "signal_scan.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"

enum { RAM_BYTES = 12 * 1024, OWNER_CHANNEL_BYTES = 128, NO_SLOT = -1, RETRY_MS = 10 };
enum { TOPIC_FOREIGN, TOPIC_CMD, TOPIC_NOTIFY };
enum { RX_IDLE, RX_COLLECT, RX_SCAN, RX_DISCARD };

typedef struct {
    const char *type, *channel;
    xiaotai_platform_signal_fn_t handler;
    void *ctx;
} subscription_t;

typedef struct {
    char *text, *large_channel;
    size_t total, large_bytes;
    platform_envelope_t envelope;
    uint32_t generation;
    int32_t ack_id;
    int next;
    bool occupied, command, unbind;
} signal_slot_t;

typedef struct {
    uint8_t mode, topic;
    int slot;
    int32_t id;
    size_t total, received;
    uint32_t generation;
    platform_signal_scan_t scan;
} rx_t;

#define STAT_LIST(X) \
    X(signals) X(signals_delivered) X(signals_acknowledged) X(signal_post_retries) \
    X(signals_malformed) X(signals_oversized) X(signals_fragment_errors) \
    X(signals_no_slot) X(signals_stale) X(signals_unauthorized)
#define DECLARE_STAT(name) atomic_uint name;
static struct {
    xiaotai_critical_t lock;
    bool lock_initialized;
    atomic_bool live;
    signal_slot_t *slots;
    subscription_t *subs;
    size_t slot_count, limit, used, sub_count, sub_limit;
    char *arena, *channel_scratch;
    bool external, wildcard;
    rx_t rx;

    /* One owner job at a time: accepted slots cannot overtake the FIFO head. */
    int head, tail;
    uint32_t next_token, queued_token;
    bool owner_running, verify_pending, verify_posted;
    uint32_t verify_generation;
    int64_t retry_at;
    platform_signal_binding_fn binding;
    void *binding_ctx;
    atomic_uint revisions;
    atomic_uint binding_pending;
    STAT_LIST(DECLARE_STAT)
} s;
#undef DECLARE_STAT

static void lock(void) { xiaotai_critical_enter(&s.lock); }
static void unlock(void) { xiaotai_critical_exit(&s.lock); }

static bool registry_open(void)
{
    return atomic_load(&platform.initialized) && !atomic_load(&platform.started);
}

xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                                xiaotai_platform_signal_fn_t handler, void *ctx)
{
    if (!type || !handler) return XIAOTAI_ERR_INVALID_ARG;
    if (!registry_open()) return XIAOTAI_ERR_INVALID_STATE;
    if (platform.provisioning) return XIAOTAI_OK;
    if (s.sub_count == s.sub_limit) {
        if (s.sub_limit > SIZE_MAX / sizeof(subscription_t) - 8U) return XIAOTAI_ERR_NO_MEM;
        const size_t capacity = s.sub_limit ? s.sub_limit + 8U : 16U;
        subscription_t *subs = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, capacity * sizeof(*subs));
        if (!subs) return XIAOTAI_ERR_NO_MEM;
        if (s.sub_count) memcpy(subs, s.subs, s.sub_count * sizeof(*subs));
        xiaotai_mem_free(s.subs);
        s.subs = subs;
        s.sub_limit = capacity;
    }
    s.subs[s.sub_count++] = (subscription_t){type, channel, handler, ctx};
    s.wildcard |= channel == NULL;
    return XIAOTAI_OK;
}

xiaotai_err_t platform_signals_binding_register(platform_signal_binding_fn fn, void *ctx)
{
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!registry_open() || s.binding) return XIAOTAI_ERR_INVALID_STATE;
    if (!platform.provisioning) {
        s.binding = fn;
        s.binding_ctx = ctx;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t platform_signals_start(void)
{
    if (platform.provisioning) return XIAOTAI_OK;
    if (atomic_load(&s.live)) return XIAOTAI_ERR_INVALID_STATE;
    s.slot_count = platform.config.signal_slots;
    s.limit = platform.need.signal_bytes ? platform.need.signal_bytes : platform.config.signal_bytes;
    s.external = platform.external;
    s.head = s.tail = NO_SLOT;
    if (!s.slot_count || s.limit < 2U || (platform.config.reserve_unbind_signal_slot && s.slot_count < 2U))
        return XIAOTAI_ERR_INVALID_SIZE;
    if (!s.lock_initialized) {
        xiaotai_critical_init(&s.lock);
        s.lock_initialized = true;
    }
    const bool resident = s.external && !platform.config.signal_payload_on_demand;
    s.slots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, s.slot_count, sizeof(*s.slots));
    if (resident) s.arena = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s.slot_count * s.limit);
    if (s.external && s.wildcard) s.channel_scratch = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s.limit);
    if (!s.slots || (resident && !s.arena) || (s.external && s.wildcard && !s.channel_scratch)) {
        xiaotai_mem_free(s.slots);
        xiaotai_mem_free(s.arena);
        xiaotai_mem_free(s.channel_scratch);
        s.slots = NULL;
        s.arena = s.channel_scratch = NULL;
        return XIAOTAI_ERR_NO_MEM;
    }
    atomic_store(&s.live, true);
    return XIAOTAI_OK;
}

static size_t ordinary_slots(void)
{
    return s.slot_count - (size_t)platform.config.reserve_unbind_signal_slot;
}

static void release(int index)
{
    signal_slot_t *slot = &s.slots[index];
    if (slot->text) {
        xiaotai_identity_wipe(slot->text, slot->total + 1U);
        if (!s.arena) xiaotai_mem_free(slot->text);
    }
    if (slot->large_channel) {
        xiaotai_identity_wipe(slot->large_channel, slot->large_bytes);
        xiaotai_mem_free(slot->large_channel);
    }
    lock();
    if (!s.external && (size_t)index < ordinary_slots()) s.used -= slot->total + 1U + slot->large_bytes;
    *slot = (signal_slot_t){0};
    unlock();
}

static int reserve(size_t total)
{
    int index = NO_SLOT;
    lock();
    if (s.external || total + 1U <= RAM_BYTES - s.used) {
        for (size_t i = 0; i < ordinary_slots(); ++i) {
            if (s.slots[i].occupied) continue;
            index = (int)i;
            s.slots[i] = (signal_slot_t){.occupied = true, .total = total, .next = NO_SLOT};
            if (!s.external) s.used += total + 1U;
            break;
        }
    }
    unlock();
    if (index != NO_SLOT) {
        s.slots[index].text = s.arena ? s.arena + (size_t)index * s.limit :
            xiaotai_mem_alloc(XIAOTAI_MEM_BULK, total + 1U);
        if (!s.slots[index].text) { release(index); return NO_SLOT; }
    }
    return index;
}

static int reserve_unbind(uint32_t generation)
{
    const size_t index = ordinary_slots();
    int result = NO_SLOT;
    lock();
    if (!s.slots[index].occupied) {
        /* The binding owner only consumes the generation and ACK ID. Do not
         * charge payload RAM or borrow any ordinary body's admission budget. */
        s.slots[index] = (signal_slot_t){.occupied = true, .command = true, .unbind = true,
            .generation = generation, .next = NO_SLOT};
        result = (int)index;
    }
    unlock();
    return result;
}

static void clear_rx(void)
{
    if (s.rx.mode == RX_COLLECT) release(s.rx.slot);
    s.rx = (rx_t){0};
}

static void dropped_fragment(uint8_t topic)
{
    atomic_fetch_add(&s.signals_fragment_errors, 1U);
    if (topic == TOPIC_NOTIFY) atomic_fetch_add(&s.revisions, 1U);
    platform_wake();
}

void platform_signals_disconnected(void)
{
    if (!atomic_load(&s.live)) return;
    if (s.rx.mode == RX_COLLECT || s.rx.mode == RX_SCAN) dropped_fragment(s.rx.topic);
    clear_rx();
}

static uint32_t token_next(void)
{
    if (++s.next_token == 0U) ++s.next_token;
    return s.next_token;
}

static bool same(const xiaotai_mqtt_event_t *e, const char *topic)
{
    return topic && e->topic && e->topic_size == strlen(topic) && !memcmp(e->topic, topic, e->topic_size);
}

static int32_t acknowledge(xiaotai_mqtt_t *client, const char *topic)
{
    static const char body[] = "{\"ack\":true}";
    int32_t id = 0;
    if (xiaotai_mqtt_publish(client, topic, body, sizeof(body) - 1U, 1U, XIAOTAI_MQTT_SEND_QUEUED, &id) != XIAOTAI_OK)
        return 0;
    atomic_fetch_add(&s.signals_acknowledged, 1U);
    return id;
}

static bool is_unbind(const signal_slot_t *slot)
{
    return (!platform.config.unbind_command_only || slot->command) &&
           platform_json_span_equals(slot->text, slot->envelope.type, "unbind");
}

/* An exact channel subscriber can reuse its registration string. A wildcard
 * may see any decoded channel: common ones fit owner stack, larger ones use
 * one external-memory scratch or a charged allocation reserved before ACK. */
static bool prepare_channel(signal_slot_t *slot)
{
    if (s.external || !s.wildcard || !slot->envelope.has_channel || is_unbind(slot)) return true;
    for (size_t i = 0; i < s.sub_count; ++i) {
        const subscription_t *sub = &s.subs[i];
        if (sub->channel || !platform_json_span_equals(slot->text, slot->envelope.type, sub->type)) continue;
        size_t length = 0;
        (void)platform_json_span_text(slot->text, slot->envelope.channel, NULL, 0, &length);
        if (length < OWNER_CHANNEL_BYTES) return true;
        const size_t bytes = length + 1U;
        lock();
        const bool fits = bytes <= RAM_BYTES - s.used;
        if (fits) {
            s.used += bytes;
            slot->large_bytes = bytes;
        }
        unlock();
        if (!fits) return false;
        slot->large_channel = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
        return slot->large_channel != NULL;
    }
    return true;
}

static void dispatch(signal_slot_t *slot)
{
    const bool unbind = slot->unbind;
    if (slot->generation != xiaotai_identity_generation()) {
        /* A queued confirmation can upgrade a still-running check of that
         * binding. Other old messages never cross the generation fence. */
        if (!unbind || !platform_binding_prior_generation(slot->generation)) {
            atomic_fetch_add(&s.signals_stale, 1U);
            return;
        }
    }
    if (unbind) {
        if (s.binding) s.binding(true, slot->ack_id, slot->generation, s.binding_ctx);
        return;
    }
    const char *wild_channel = NULL;
    char local[OWNER_CHANNEL_BYTES];
    for (size_t i = 0; i < s.sub_count; ++i) {
        const subscription_t *sub = &s.subs[i];
        if (!platform_json_span_equals(slot->text, slot->envelope.type, sub->type)) continue;
        const char *channel = sub->channel;
        if (channel) {
            if (slot->envelope.has_channel ?
                !platform_json_span_equals(slot->text, slot->envelope.channel, channel) : *channel != '\0') continue;
        } else {
            if (!wild_channel) {
                wild_channel = "";
                if (slot->envelope.has_channel) {
                    char *out = s.external ? s.channel_scratch :
                        slot->large_channel ? slot->large_channel : local;
                    const size_t capacity = s.external ? s.limit :
                        slot->large_channel ? slot->large_bytes : sizeof(local);
                    xiaotai_err_t err = platform_json_span_text(slot->text, slot->envelope.channel, out, capacity, NULL);
                    if (err != XIAOTAI_OK) return; /* validated type/length at admission */
                    wild_channel = out;
                }
            }
            channel = wild_channel;
        }
        const xiaotai_platform_signal_t signal = {
            .type = sub->type, .channel = channel, .json = slot->text, .size = slot->total,
            .generation = slot->generation, .command = slot->command,
        };
        sub->handler(&signal, sub->ctx);
    }
    if (wild_channel == s.channel_scratch && s.channel_scratch)
        xiaotai_identity_wipe(s.channel_scratch, strlen(s.channel_scratch) + 1U);
    xiaotai_identity_wipe(local, sizeof(local));
    atomic_fetch_add(&s.signals_delivered, 1U);
}

static void owner_run(void *ctx)
{
    if (!atomic_load(&s.live)) return;
    lock();
    if (!atomic_load(&s.live) || !s.queued_token || s.queued_token != (uint32_t)(uintptr_t)ctx) {
        unlock();
        return;
    }
    s.owner_running = true;
    const bool verify = s.verify_posted;
    const uint32_t generation = s.verify_generation;
    if (verify) s.verify_pending = s.verify_posted = false;
    const int index = verify ? NO_SLOT : s.head;
    if (index != NO_SLOT) {
        s.head = s.slots[index].next;
        if (s.head == NO_SLOT) s.tail = NO_SLOT;
    }
    unlock();
    if (verify && s.binding && generation == xiaotai_identity_generation())
        s.binding(false, 0, generation, s.binding_ctx);
    if (index != NO_SLOT) {
        dispatch(&s.slots[index]);
        if (s.slots[index].unbind) atomic_fetch_sub(&s.binding_pending, 1U);
        release(index);
    }
    lock();
    s.queued_token = 0;
    s.owner_running = false;
    unlock();
    platform_wake();
}

static void schedule(void)
{
    const int64_t now = xiaotai_time_us();
    lock();
    if (s.queued_token || (s.head == NO_SLOT && !s.verify_pending) || now < s.retry_at) {
        unlock();
        return;
    }
    s.verify_posted = s.head == NO_SLOT && s.verify_pending;
    const uint32_t token = s.queued_token = token_next();
    unlock();
    if (xiaotai_run(owner_run, (void *)(uintptr_t)token) == XIAOTAI_OK) return;
    lock();
    s.queued_token = 0;
    s.verify_posted = false;
    s.retry_at = now + RETRY_MS * INT64_C(1000);
    unlock();
    atomic_fetch_add(&s.signal_post_retries, 1U);
}

static void no_slot(uint8_t topic, uint32_t generation)
{
    atomic_fetch_add(&s.signals_no_slot, 1U);
    if (topic == TOPIC_NOTIFY) atomic_fetch_add(&s.revisions, 1U);
    if (!platform.config.tolerate_signal_loss && s.binding) {
        lock();
        s.verify_pending = true;
        s.verify_generation = generation;
        unlock();
        schedule();
    }
    platform_wake();
}

void platform_signals_message(xiaotai_mqtt_t *client, const xiaotai_mqtt_event_t *e,
                              const char *cmd, const char *notify, const char *ack,
                              uint32_t generation, bool authorized)
{
    if (!atomic_load(&s.live)) return;
    const uint8_t topic = same(e, cmd) ? TOPIC_CMD : same(e, notify) ? TOPIC_NOTIFY : TOPIC_FOREIGN;
    rx_t *rx = &s.rx;
    if (e->offset == 0 && (rx->mode == RX_IDLE || rx->received || rx->id != e->message_id ||
        rx->topic != topic || rx->total != e->total || rx->generation != generation)) {
        if (rx->mode == RX_COLLECT || rx->mode == RX_SCAN) dropped_fragment(rx->topic);
        clear_rx();
        *rx = (rx_t){.mode = RX_DISCARD, .topic = topic, .id = e->message_id,
                     .total = e->total, .generation = generation, .slot = NO_SLOT};
        if (!authorized) {
            atomic_fetch_add(&s.signals_unauthorized, 1U);
            platform_wake();
        } else if (topic == TOPIC_FOREIGN || !e->total || e->size > e->total || (e->size && !e->data)) {
            dropped_fragment(topic);
        } else if (e->total >= s.limit) {
            atomic_fetch_add(&s.signals_oversized, 1U);
            if (topic == TOPIC_NOTIFY) atomic_fetch_add(&s.revisions, 1U);
            platform_wake();
        } else {
            rx->slot = reserve(rx->total);
            rx->mode = rx->slot == NO_SLOT ? RX_SCAN : RX_COLLECT;
        }
    }
    if (rx->mode == RX_DISCARD) {
        if (e->offset <= e->total && e->size <= e->total - e->offset) {
            rx->received = e->offset + e->size;
            if (rx->received == rx->total) *rx = (rx_t){0};
        }
        return;
    }
    if (rx->mode == RX_IDLE || rx->id != e->message_id || rx->topic != topic ||
        rx->total != e->total || rx->received != e->offset || rx->generation != generation ||
        e->size > rx->total - rx->received || (e->size && !e->data)) {
        dropped_fragment(rx->mode == RX_IDLE ? topic : rx->topic);
        clear_rx();
        /* With a broken fragment there is no valid zero-byte prefix to keep:
         * offset zero may start a new publication with this same packet ID. */
        *rx = (rx_t){.mode = RX_DISCARD, .topic = topic, .id = e->message_id,
                     .total = e->total, .received = 1, .generation = generation, .slot = NO_SLOT};
        return;
    }
    /* A stream admitted with a zero-byte topic fragment remains the same
     * message when the next callback still reports offset zero. */
    if (rx->mode == RX_SCAN) platform_signal_scan_feed(&rx->scan, e->data, e->size);
    else if (e->size) memcpy(s.slots[rx->slot].text + rx->received, e->data, e->size);
    rx->received += e->size;
    if (rx->received != rx->total) return;

    int index = rx->slot;
    signal_slot_t *slot = index == NO_SLOT ? NULL : &s.slots[index];
    bool valid;
    if (slot) {
        slot->text[slot->total] = '\0';
        slot->generation = generation;
        slot->command = topic == TOPIC_CMD;
        valid = platform_json_envelope(slot->text, slot->total, &slot->envelope) == XIAOTAI_OK;
    } else valid = platform_signal_scan_valid(&rx->scan);
    const bool control = valid && platform.config.reserve_unbind_signal_slot && topic == TOPIC_CMD && s.binding &&
        (slot ? is_unbind(slot) : platform_signal_scan_unbind(&rx->scan));
    *rx = (rx_t){0};

    if (!valid) {
        atomic_fetch_add(&s.signals_malformed, 1U);
        if (topic == TOPIC_CMD && !platform.config.drop_malformed_commands) (void)acknowledge(client, ack);
        if (slot) release(index);
        platform_wake();
        return;
    }
    if (control) {
        /* The first fragment admitted this generation. The owner decides
         * whether a prior generation can confirm an in-flight binding check. */
        if (slot) release(index);
        index = reserve_unbind(generation);
        slot = index == NO_SLOT ? NULL : &s.slots[index];
    }
    if (!slot || (!control && !prepare_channel(slot))) {
        if (slot) release(index);
        no_slot(topic, generation);
        return;
    }
    slot->unbind = control || is_unbind(slot);
    if (slot->unbind) atomic_fetch_add(&s.binding_pending, 1U);
    if (slot->command) {
        const bool tracking = slot->unbind && platform_signaling_unbind_issuing(client);
        slot->ack_id = acknowledge(client, ack);
        if (tracking) platform_signaling_unbind_queued(slot->ack_id);
    }
    atomic_fetch_add(&s.signals, 1U);
    lock();
    if (s.tail != NO_SLOT) s.slots[s.tail].next = index;
    else s.head = index;
    s.tail = index;
    unlock();
    schedule();
    platform_wake();
}

uint32_t platform_signals_step(void)
{
    if (!atomic_load(&s.live)) return XIAOTAI_WAIT_FOREVER;
    schedule();
    platform_lock();
#define MERGE_STAT(name) platform.stats.name += atomic_exchange(&s.name, 0);
    STAT_LIST(MERGE_STAT)
#undef MERGE_STAT
    platform.status.notify_revision += atomic_exchange(&s.revisions, 0);
    platform_unlock();
    lock();
    const bool pending = !s.queued_token && (s.verify_pending || s.head != NO_SLOT);
    unlock();
    return pending ? RETRY_MS : XIAOTAI_WAIT_FOREVER;
}

bool platform_signals_binding_pending(void) { return atomic_load(&s.binding_pending) != 0; }

void platform_signals_reset(void)
{
    if (atomic_exchange(&s.live, false)) {
        /* Queued owner jobs retain a number, not a slot pointer. Only a job
         * already inside its handler owns memory that reset must wait for. */
        for (;;) {
            lock();
            const bool running = s.owner_running;
            if (!running) s.queued_token = 0;
            unlock();
            if (!running) break;
            xiaotai_delay_ms(1);
        }
        for (size_t i = 0; i < s.slot_count; ++i) if (s.slots[i].occupied) release((int)i);
    }
    xiaotai_mem_free(s.slots);
    xiaotai_mem_free(s.arena);
    xiaotai_mem_free(s.channel_scratch);
    xiaotai_mem_free(s.subs);
    s.arena = s.channel_scratch = NULL;
    s.slots = NULL;
    s.subs = NULL;
    s.sub_count = s.sub_limit = s.used = 0;
    s.wildcard = s.verify_pending = s.verify_posted = false;
    s.retry_at = 0;
    s.binding = NULL;
    s.binding_ctx = NULL;
    s.rx = (rx_t){0};
#define RESET_STAT(name) atomic_store(&s.name, 0);
    STAT_LIST(RESET_STAT)
#undef RESET_STAT
    atomic_store(&s.revisions, 0);
    atomic_store(&s.binding_pending, 0);
}
