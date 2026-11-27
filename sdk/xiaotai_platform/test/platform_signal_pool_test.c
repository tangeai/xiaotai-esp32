// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_control.c::platform_control
// Replaces legacy: tests/regression/test_p4_mqtt_commands.py
// Replaces legacy: tests/xmini/xmini_business/test_business.c::wechat_outgoing
// Replaces legacy: tests/regression/test_c3_mqtt_frames.py
// Replaces legacy: tests/regression/test_c3_binding_grant.py
// Replaces legacy: tests/regression/test_mqtt_readiness.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "signal_pool.h"
#include "xiaotai/framework.h"
#include "xiaotai_mem_host.h"

static const char cmd[] = "device/sn_kit/cmd", notify[] = "device/sn_kit/notify", ack[] = "device/sn_kit/ack";
static uint32_t current = 11;
static uint32_t expected_prior;
static int64_t now;
static unsigned acks, received, weak, confirmed;
static int32_t last_ack;
static bool refuse, abort_on_delivery;
static char channels[16][240];
static char payload_tail[16];
static const char *last_json;
static size_t last_size;
static void (*job)(void *);
static void *job_ctx;

uint32_t __wrap_xiaotai_identity_generation(void) { return current; }
int64_t __wrap_xiaotai_time_us(void) { return now; }

xiaotai_err_t __wrap_xiaotai_run(void (*fn)(void *), void *ctx)
{
    if (refuse) return XIAOTAI_ERR_NO_MEM;
    assert(!job);
    job = fn;
    job_ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t __wrap_xiaotai_mqtt_publish(xiaotai_mqtt_t *client, const char *topic, const void *data,
                                       size_t size, uint8_t qos, xiaotai_mqtt_send_t send, int32_t *id)
{
    assert(client == (void *)(uintptr_t)1 && !strcmp(topic, ack));
    assert(size == sizeof("{\"ack\":true}") - 1U && !memcmp(data, "{\"ack\":true}", size));
    assert(qos == 1 && send == XIAOTAI_MQTT_SEND_QUEUED && id);
    *id = (int32_t)++acks;
    return XIAOTAI_OK;
}

static void callback(const xiaotai_platform_signal_t *signal, void *ctx)
{
    platform_envelope_t parsed;
    assert(signal->generation == current && !strcmp(signal->type, ctx));
    assert(signal->json[signal->size] == '\0');
    assert(platform_json_envelope(signal->json, signal->size, &parsed) == XIAOTAI_OK);
    assert(received < sizeof(channels) / sizeof(channels[0]) && strlen(signal->channel) < sizeof(channels[0]));
    if (abort_on_delivery) {
        /* Abort the different message being assembled, not this owner loan. */
        const char tail = signal->json[signal->size - 3U];
        platform_signals_disconnected();
        assert(signal->json[signal->size - 3U] == tail && signal->json[signal->size] == '\0');
        assert(platform_json_envelope(signal->json, signal->size, &parsed) == XIAOTAI_OK);
    }
    payload_tail[received] = signal->json[signal->size - 3U];
    strcpy(channels[received++], signal->channel);
    last_json = signal->json;
    last_size = signal->size;
}

static void binding(bool is_confirmed, int32_t identity, uint32_t generation, void *ctx)
{
    assert(generation == xiaotai_identity_generation() || (is_confirmed && generation == expected_prior));
    assert(ctx == &current);
    if (is_confirmed) ++confirmed;
    else ++weak;
    last_ack = identity;
}

static void boot_pool_mode(bool external, uint8_t slots, uint16_t bytes, bool on_demand, bool control)
{
    memset(&platform, 0, sizeof(platform));
    assert(xiaotai_mutex_init(&platform.lock) == XIAOTAI_OK);
    platform.external = external;
    platform.config.signal_slots = slots;
    platform.config.signal_bytes = bytes;
    platform.config.signal_payload_on_demand = on_demand;
    platform.config.reserve_unbind_signal_slot = control;
    if (control) {
        platform.config.unbind_command_only = true;
        platform.config.drop_malformed_commands = true;
        platform.config.tolerate_signal_loss = true;
    }
    atomic_store(&platform.initialized, true);
    assert(xiaotai_platform_signal_subscribe("t", NULL, callback, "t") == XIAOTAI_OK);
    assert(xiaotai_platform_signal_subscribe("t", "wx", callback, "t") == XIAOTAI_OK);
    assert(xiaotai_platform_signal_subscribe("unbind", NULL, callback, "unbind") == XIAOTAI_OK);
    assert(xiaotai_platform_signal_subscribe("call_incoming", NULL, callback, "call_incoming") == XIAOTAI_OK);
    assert(platform_signals_binding_register(binding, &current) == XIAOTAI_OK);
    atomic_store(&platform.started, true);
    assert(platform_signals_start() == XIAOTAI_OK);
    current = 11;
    expected_prior = 0;
    now = 100000;
    acks = received = weak = confirmed = last_ack = 0;
    refuse = abort_on_delivery = false;
    last_json = NULL;
    last_size = 0;
}

static void boot_pool(bool external, uint8_t slots, uint16_t bytes, bool on_demand)
{
    boot_pool_mode(external, slots, bytes, on_demand, false);
}

static void boot(bool external, uint8_t slots, uint16_t bytes)
{
    boot_pool(external, slots, bytes, false);
}

static void close_pool(void)
{
    platform_signals_reset();
    xiaotai_mutex_deinit(&platform.lock);
}

static void fragment_from(const char *topic, int32_t id, const char *data, size_t total, size_t offset, size_t size,
                          uint32_t generation, bool authorized)
{
    const xiaotai_mqtt_event_t e = {.kind = XIAOTAI_MQTT_DATA, .message_id = id, .topic = topic,
        .topic_size = strlen(topic), .data = (const uint8_t *)data, .total = total, .offset = offset, .size = size};
    platform_signals_message((void *)(uintptr_t)1, &e, cmd, notify, ack, generation, authorized);
}

static void fragment(const char *topic, int32_t id, const char *data, size_t total, size_t offset, size_t size,
                     bool authorized)
{
    fragment_from(topic, id, data, total, offset, size, current, authorized);
}

static void split(const char *topic, int32_t id, const char *json, size_t size, size_t at)
{
    fragment(topic, id, NULL, size, 0, 0, true); /* topic-only fragment */
    fragment(topic, id, json, size, 0, at, true);
    fragment(topic, id, json + at, size, at, size - at, true);
}

static void whole(const char *topic, int32_t id, const char *text)
{
    const size_t size = strlen(text);
    fragment(topic, id, text, size, 0, size, true);
}

static void owner(void)
{
    assert(job);
    void (*call)(void *) = job;
    void *ctx = job_ctx;
    job = NULL;
    job_ctx = NULL;
    call(ctx);
}

static void test_fragment_boundary(void)
{
    boot(true, 2, 128);
    const char json[] = "{\"type\":\"t\"}";
    const size_t length = sizeof(json) - 1U, first = 5U;
    fragment(cmd, 1, NULL, length, 0, 0, true);
    assert(!acks && !received && !job);
    fragment(cmd, 1, json, length, 0, first, true);
    assert(!acks && !received && !job);
    fragment(cmd, 1, json + first, length, first, length - first, true);
    assert(acks == 1 && !received && job);
    owner();
    assert(acks == 1 && received == 1 && !job);

    fragment(cmd, 2, json, length, 0, first, true);
    platform_signals_disconnected();
    fragment(cmd, 2, json + first, length, first, length - first, true);
    assert(acks == 1 && received == 1 && !job);

    split(notify, 2, json, length, first);
    owner();
    assert(acks == 1 && received == 2 && !job);
    close_pool();
}

static void test_outcomes(void)
{
    boot(true, 1, 128);
    xiaotai_mem_host_clear();
    const char valid[] = "{\"t\\u0079pe\":\"t\",\"channel\":\"w\\u0078\"}";
    split(cmd, 1, valid, sizeof(valid) - 1U, 9U);
    assert(acks == 1 && received == 0 && job);
    const char bad[] = "{\"type\":\"x\",\"channel\":\"\\ud800\"}";
    split(cmd, 2, bad, sizeof(bad) - 1U, 22U);
    assert(acks == 2);
    split(notify, 3, valid, sizeof(valid) - 1U, 12U);
    split(notify, 4, bad, sizeof(bad) - 1U, 23U);
    split(cmd, 5, valid, sizeof(valid) - 1U, 18U);
    fragment(notify, 6, valid, 128, 0, 3, true);
    fragment(notify, 6, NULL, 128, 3, 0, true);
    whole("device/sn_kit/notifies", 7, valid);
    fragment(notify, 8, valid, sizeof(valid) - 1U, 0, 8, true);
    fragment(cmd, 8, valid + 8, sizeof(valid) - 1U, 8, 8, true);
    fragment(cmd, 9, valid, sizeof(valid) - 1U, 0, sizeof(valid) - 1U, false);
    assert(acks == 2 && received == 0 && weak == 0 && xiaotai_mem_host_allocations() == 0);

    owner();
    assert(received == 2 && !strcmp(channels[0], "wx") && !strcmp(channels[1], "wx"));
    for (size_t i = 0; i <= last_size; ++i) assert(last_json[i] == '\0'); /* reserved arena, cleared slot */
    (void)platform_signals_step();
    owner();
    assert(weak == 1 && last_ack == 0 && confirmed == 0);
    const xiaotai_platform_stats_t stats = platform.stats;
    assert(stats.signals == 1 && stats.signals_delivered == 1 && stats.signals_acknowledged == 2);
    assert(stats.signals_malformed == 2 && stats.signals_no_slot == 2 && stats.signals_oversized == 1);
    assert(stats.signals_fragment_errors == 2 && stats.signals_unauthorized == 1);
    assert(platform.status.notify_revision == 3);
    close_pool();
}

static void test_fifo_and_reset(void)
{
    boot(false, 3, 128);
    refuse = true;
    whole(cmd, 1, "{\"type\":\"t\",\"channel\":\"first\"}");
    whole(cmd, 2, "{\"type\":\"t\",\"channel\":\"second\"}");
    whole(cmd, 3, "{\"type\":\"t\",\"channel\":\"third\"}");
    assert(acks == 3 && !job);
    assert(platform_signals_step() == 10 && platform.stats.signal_post_retries == 1);
    now += 10000;
    assert(platform_signals_step() == 10 && platform.stats.signal_post_retries == 2);
    refuse = false;
    now += 10000;
    (void)platform_signals_step();
    for (unsigned i = 0; i < 3; ++i) {
        owner();
        (void)platform_signals_step();
    }
    assert(received == 3 && !strcmp(channels[0], "first") && !strcmp(channels[1], "second") &&
           !strcmp(channels[2], "third"));
    whole(cmd, 4, "{\"type\":\"t\"}");
    ++current;
    owner();
    (void)platform_signals_step();
    assert(received == 3 && platform.stats.signals_stale == 1 && acks == 4);

    whole(cmd, 5, "{\"type\":\"t\"}");
    assert(job);
    close_pool();
    boot(false, 2, 128);
    owner(); /* delayed job from retired pool cannot touch allocated slots */
    assert(received == 0 && !job);
    whole(cmd, 6, "{\"type\":\"t\"}");
    owner();
    assert(received == 1 && !strcmp(channels[0], ""));
    close_pool();
}

static void padded(char *to, size_t size, const char *key)
{
    const int head = snprintf(to, size + 1U, "{\"type\":\"t\",\"%s\":\"", key);
    assert(head > 0 && (size_t)head + 2U <= size);
    memset(to + head, 'a', size - (size_t)head - 2U);
    memcpy(to + size - 2U, "\"}", 3);
}

static size_t bulk_bytes(void)
{
    xiaotai_mem_host_ledger_entry_t records[32];
    size_t count = 0, bytes = 0;
    assert(xiaotai_mem_host_ledger_snapshot(records, sizeof(records) / sizeof(records[0]), &count));
    for (size_t i = 0; i < count; ++i) if (records[i].kind == XIAOTAI_MEM_BULK) bytes += records[i].bytes;
    return bytes;
}

static size_t test_external_capacity(bool on_demand)
{
    static char json[8192];
    xiaotai_mem_host_clear();
    boot_pool(true, 8, sizeof(json), on_demand);
    const size_t idle = bulk_bytes();
    padded(json, sizeof(json) - 1U, "other");
    for (unsigned i = 0; i < 8U; ++i) {
        json[sizeof(json) - 4U] = (char)('a' + i);
        split(cmd, (int32_t)i + 1, json, sizeof(json) - 1U, 512U);
    }
    assert(acks == 8 && received == 0 && bulk_bytes() == idle + (on_demand ? 8U * sizeof(json) : 0U));
    const uint32_t attempted = xiaotai_mem_host_allocations();
    split(cmd, 9, json, sizeof(json) - 1U, 512U);
    fragment(cmd, 10, json, sizeof(json), 0, sizeof(json), true);
    assert(acks == 8 && xiaotai_mem_host_allocations() == attempted);
    for (unsigned i = 0; i < 8U; ++i) {
        owner();
        (void)platform_signals_step();
        assert(received == i + 1U && payload_tail[i] == (char)('a' + i));
    }
    owner(); /* verification after the over-capacity message, not an ACK */
    assert(weak == 1 && bulk_bytes() == idle && !job);
    assert(platform.stats.signals_no_slot == 1 && platform.stats.signals_oversized == 1);
    close_pool();
    assert(bulk_bytes() == 0 && xiaotai_mem_host_outstanding() == 0);
    return idle;
}

static void test_external_on_demand_abort(void)
{
    static char wide_channel[201];
    const char next[] = "{\"type\":\"t\",\"channel\":\"short\"}";
    xiaotai_mem_host_clear();
    boot_pool(true, 8, 8192, true);
    const size_t idle = bulk_bytes();
    padded(wide_channel, sizeof(wide_channel) - 1U, "channel");
    fragment(notify, 1, wide_channel, sizeof(wide_channel) - 1U, 0, 20, true);
    assert(bulk_bytes() == idle + sizeof(wide_channel));
    split(cmd, 2, next, sizeof(next) - 1U, 12U); /* overtaking partial fragment */
    assert(acks == 1 && bulk_bytes() == idle + sizeof(next) && job);
    fragment(notify, 3, wide_channel, sizeof(wide_channel) - 1U, 0, 20, true);
    assert(bulk_bytes() == idle + sizeof(next) + sizeof(wide_channel));
    abort_on_delivery = true;
    owner(); /* disconnect frees the partial message without invalidating this callback */
    abort_on_delivery = false;
    assert(received == 1 && !strcmp(channels[0], "short") && bulk_bytes() == idle);

    xiaotai_mem_host_fail_nth(1);
    split(cmd, 4, next, sizeof(next) - 1U, 12U);
    assert(!xiaotai_mem_host_fail_pending() && acks == 1 && bulk_bytes() == idle && job);
    xiaotai_mem_failure_t failure;
    xiaotai_mem_failure(&failure);
    assert(failure.has_first && failure.failures == 1 && failure.requested_size == sizeof(next));
    owner(); /* no payload admitted or acknowledged; only verify binding */
    assert(weak == 1 && received == 1);
    split(cmd, 5, wide_channel, sizeof(wide_channel) - 1U, 27U);
    assert(acks == 2 && bulk_bytes() == idle + sizeof(wide_channel));
    owner();
    assert(received == 2 && strlen(channels[1]) > 128U && bulk_bytes() == idle);
    (void)platform_signals_step();
    assert(platform.stats.signals_no_slot == 1 && platform.stats.signals_fragment_errors == 2);

    whole(cmd, 6, next);
    fragment(notify, 7, wide_channel, sizeof(wide_channel) - 1U, 0, 20, true);
    assert(job && bulk_bytes() == idle + sizeof(next) + sizeof(wide_channel));
    close_pool(); /* frees both queued and partial allocations */
    assert(bulk_bytes() == 0 && xiaotai_mem_host_outstanding() == 0);
    owner(); /* delayed owner job now has no slot to touch */
    assert(received == 2);
}

static void test_budget(void)
{
    boot(false, 2, 8192);
    static char large[8192], next[4097], long_channel[201];
    padded(large, sizeof(large) - 1U, "other");
    padded(next, sizeof(next) - 1U, "other");
    padded(long_channel, sizeof(long_channel) - 1U, "channel");
    xiaotai_mem_host_clear();
    split(cmd, 1, large, strlen(large), 512);
    assert(acks == 1 && xiaotai_mem_host_allocations() == 1);
    split(notify, 2, next, strlen(next), 999);
    assert(acks == 1 && xiaotai_mem_host_allocations() == 1); /* exactly one byte over 12 KiB */
    owner();
    (void)platform_signals_step();
    owner();
    assert(weak == 1 && xiaotai_mem_host_outstanding() == 0);

    split(cmd, 3, long_channel, strlen(long_channel), 47);
    assert(acks == 2 && xiaotai_mem_host_outstanding() == 2);
    owner();
    assert(xiaotai_mem_host_outstanding() == 0 && received == 2 && strlen(channels[1]) > 128);
    const char malformed[] = "{\"type\": 52}";
    const uint32_t attempts = xiaotai_mem_host_allocations();
    xiaotai_mem_host_fail_nth(1);
    split(cmd, 4, malformed, strlen(malformed), 4);
    assert(acks == 3 && xiaotai_mem_host_outstanding() == 0);
    assert(xiaotai_mem_host_allocations() == attempts + 1U);
    assert(!job);
    xiaotai_mem_host_clear();
    fragment(notify, 5, large, strlen(large), 0, 20, true);
    assert(xiaotai_mem_host_outstanding() == 1);
    platform_signals_disconnected();
    assert(xiaotai_mem_host_outstanding() == 0);
    padded(large, sizeof(large) - 1U, "channel");
    whole(cmd, 6, large);
    /* Its legal JSON fits, but not both views the wildcard handler needs. */
    assert(acks == 3 && xiaotai_mem_host_outstanding() == 0);
    owner();
    assert(weak == 2 && last_ack == 0 && received == 2);
    close_pool();
}

static void test_xmini_budget_exact(void)
{
    static char first[8192], second[4096];
    xiaotai_mem_host_clear();
    boot(false, 4, sizeof(first));
    const size_t idle = bulk_bytes();
    padded(first, sizeof(first) - 1U, "other");
    padded(second, sizeof(second) - 1U, "other");
    first[sizeof(first) - 4U] = 'L';
    second[sizeof(second) - 4U] = 'R';

    /* The old WX fixture's lengths, using inert envelopes the typed pool delivers. */
    split(cmd, 1, first, sizeof(first) - 1U, 512U);
    assert(job && acks == 1 && !received && bulk_bytes() == idle + sizeof(first));
    split(cmd, 2, second, sizeof(second) - 1U, 512U);
    assert(job && acks == 2 && !received && bulk_bytes() == idle + 12288U);

    owner();
    assert(received == 1 && payload_tail[0] == 'L' && bulk_bytes() == idle + sizeof(second));
    (void)platform_signals_step();
    owner();
    assert(received == 2 && payload_tail[1] == 'R' && bulk_bytes() == idle && !job);
    (void)platform_signals_step();
    assert(platform.stats.signals_delivered == 2 && !platform.stats.signals_no_slot && !weak);
    close_pool();
    assert(bulk_bytes() == 0 && xiaotai_mem_host_outstanding() == 0);
}

static void test_compatibility(void)
{
    boot(false, 1, 128);
    platform.config.unbind_command_only = true;
    platform.config.drop_malformed_commands = true;
    platform.config.tolerate_signal_loss = true;
    whole(cmd, 1, "{\"type\": null}");
    assert(acks == 0 && !job);
    whole(notify, 2, "{\"type\": \"unbind\"}");
    whole(notify, 3, "{\"type\": \"unbind\"}");
    owner();
    (void)platform_signals_step();
    assert(!job && weak == 0 && confirmed == 0 && received == 1 && platform.status.notify_revision == 1);
    whole(cmd, 4, "{\"type\": \"unbind\"}");
    owner();
    assert(acks == 1 && last_ack == 1 && confirmed == 1 && received == 1);
    platform.config.unbind_command_only = false;
    whole(notify, 5, "{\"type\": \"unbind\"}");
    owner();
    assert(acks == 1 && last_ack == 0 && confirmed == 2 && received == 1);
    const char reused[] = "{\"type\":\"t\"}";
    fragment(cmd, 11, reused, sizeof(reused) - 1U, 0, 2, true);
    fragment(cmd, 11, reused + 4, sizeof(reused) - 1U, 4, 2, true);
    whole(cmd, 11, reused);
    owner();
    assert(acks == 2 && received == 2);
    close_pool();
}

static void test_xmini_formal_ack_admission(void)
{
    boot(false, 4, 128);
    platform.config.drop_malformed_commands = true;
    platform.config.tolerate_signal_loss = true;
    const char command[] = "{\"type\":\"call_incoming\"}";
    whole(cmd, 1, "{\"type\":\"call_incoming\"} bad");
    assert(acks == 0 && received == 0 && !job);

    refuse = true;
    for (int32_t id = 2; id < 6; ++id) whole(cmd, id, command);
    assert(acks == 4 && received == 0 && !job);
    whole(cmd, 6, command);
    assert(acks == 4 && received == 0 && !job);
    refuse = false;
    for (unsigned i = 0; i < 4; ++i) {
        now += 100000;
        (void)platform_signals_step();
        owner();
    }
    whole(cmd, 7, command);
    owner();
    assert(acks == 5 && received == 5);
    whole(notify, 8, command);
    owner();
    (void)platform_signals_step();
    assert(acks == 5 && received == 6 && platform.stats.signals_no_slot == 1);
    close_pool();
}

static void test_reserved_unbind_signal(void)
{
    xiaotai_mem_host_clear();
    boot_pool_mode(false, 4, 128, false, true);
    const char ordinary[] = "{\"type\":\"t\"}";
    const char unbind[] = "{\"ty\\u0070e\":\"un\\u0062ind\"}";
    const size_t n = sizeof(unbind) - 1U, cut = 7U;
    const size_t idle = bulk_bytes();
    for (int32_t id = 1; id <= 3; ++id) whole(notify, id, ordinary);
    const size_t full = bulk_bytes();
    assert(full == idle + 3U * sizeof(ordinary));
    whole(cmd, 4, ordinary);
    whole(notify, 5, unbind);
    whole(cmd, 6, "{\"type\":\"t\",\"nested\":{\"type\":\"unbind\"}}");
    whole(cmd, 7, "{\"type\":\"unbind\"} trailing");
    fragment(cmd, 8, unbind, n, 0, n, false);
    fragment(cmd, 10, unbind, n, 0, cut, true);
    platform_signals_disconnected();
    fragment(cmd, 10, unbind + cut, n, cut, n - cut, true);
    assert(!acks && !confirmed && !received && bulk_bytes() == full);

    split(cmd, 11, unbind, n, cut);
    whole(cmd, 12, unbind);
    assert(acks == 1 && !confirmed && !received && bulk_bytes() == full);
    for (unsigned i = 0; i < 4U; ++i) {
        (void)platform_signals_step();
        owner();
        assert(received == (i < 3U ? i + 1U : 3U) && confirmed == (unsigned)(i == 3U));
    }
    (void)platform_signals_step();
    assert(last_ack == 1 && !weak && !job && bulk_bytes() == idle);
    assert(platform.stats.signals == 4U && platform.stats.signals_no_slot == 4U &&
           platform.stats.signals_malformed == 1U && platform.stats.signals_unauthorized == 1U);
    whole(cmd, 13, ordinary);
    owner();
    assert(acks == 2 && received == 4);
    close_pool();
    assert(bulk_bytes() == 0 && xiaotai_mem_host_outstanding() == 0);
}

static void test_admitted_unbind_generation(bool reserved, bool binding_check)
{
    boot_pool_mode(false, 2, 128, false, reserved);
    assert(xiaotai_signal_init(&platform.wake) == XIAOTAI_OK);
    atomic_store(&platform.running, true);
    const char unbind[] = "{\"type\":\"unbind\"}";
    const size_t n = sizeof(unbind) - 1U, cut = 7U;
    const uint32_t previous = current;
    fragment(cmd, 1, unbind, n, 0, cut, true);
    ++current;
    if (binding_check) {
        /* Weak unbind reconciliation already advanced identity and revoked
         * the old MQTT receive generation while this prefix was in flight. */
        platform.binding.phase = PLATFORM_BIND_VERIFY;
        platform.binding.generation = expected_prior = previous;
        platform.binding.revised_generation = current;
    }
    atomic_store(&platform.session.receive_generation, 0);
    fragment_from(cmd, 1, unbind + cut, n, cut, n - cut, previous, false);
    assert(acks == 1 && job && !confirmed);
    owner();
    (void)platform_signals_step();
    assert(confirmed == (unsigned)binding_check && platform.stats.signals_stale == (unsigned)!binding_check);

    /* A different message on that retired client never gets an admitted prefix. */
    fragment_from(cmd, 2, unbind, n, 0, n, previous, false);
    (void)platform_signals_step();
    assert(acks == 1 && !job && platform.stats.signals_unauthorized == 1);
    atomic_store(&platform.running, false);
    xiaotai_signal_deinit(&platform.wake);
    close_pool();
}

static void test_reserved_unbind_at_byte_limit(void)
{
    static char first[8192], second[4096];
    xiaotai_mem_host_clear();
    boot_pool_mode(false, 4, sizeof(first), false, true);
    const size_t idle = bulk_bytes();
    padded(first, sizeof(first) - 1U, "other");
    padded(second, sizeof(second) - 1U, "other");
    whole(notify, 1, first);
    whole(notify, 2, second);
    assert(bulk_bytes() == idle + sizeof(first) + sizeof(second));
    const uint32_t allocations = xiaotai_mem_host_allocations();
    whole(cmd, 3, "{\"type\":\"t\"}");
    whole(cmd, 4, "{\"type\":\"unbind\"}");
    assert(acks == 1 && !received && !confirmed && xiaotai_mem_host_allocations() == allocations);
    for (unsigned i = 0; i < 3U; ++i) {
        (void)platform_signals_step();
        owner();
    }
    assert(received == 2 && confirmed == 1 && last_ack == 1 && bulk_bytes() == idle);
    close_pool();
    assert(bulk_bytes() == 0 && xiaotai_mem_host_outstanding() == 0);
}

static bool test_legacy_s3_delivery(void)
{
    boot(true, 4, 128);
    fragment(cmd, 70, "te", 4, 0, 2, true);
    assert(!acks && !received);
    fragment(cmd, 70, "st", 4, 2, 2, true);
    if (job) owner();
    fprintf(stderr, "S3 2+2 bytes: ACK=%u owner=%u\n", acks, received);
    const bool old_literal = acks == 1 && received == 1;
    close_pool();

    boot(true, 4, 128);
    refuse = true;
    static const char valid[] = "{\"type\":\"t\"}";
    fragment(cmd, 71, NULL, sizeof(valid) - 1U, 0, 0, true);
    fragment(cmd, 71, valid, sizeof(valid) - 1U, 0, sizeof(valid) - 1U, true);
    fprintf(stderr, "S3 owner full: ACK=%u owner=%u\n", acks, received);
    const bool old_full = !acks && !received;
    refuse = false;
    now += 100000;
    (void)platform_signals_step();
    if (job) owner();
    fprintf(stderr, "S3 owner recovered: ACK=%u owner=%u\n", acks, received);
    close_pool();
    return old_literal && old_full;
}

int main(int argc, char **argv)
{
    xiaotai_mem_host_ledger_start();
    if (argc == 2 && !strcmp(argv[1], "legacy_s3_delivery")) return test_legacy_s3_delivery() ? 0 : 1;
    const xiaotai_platform_config_t invalid = {.signal_slots = 1, .reserve_unbind_signal_slot = true};
    assert(xiaotai_platform_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    test_fragment_boundary();
    test_outcomes();
    test_fifo_and_reset();
    test_budget();
    test_xmini_budget_exact();
    test_compatibility();
    test_xmini_formal_ack_admission();
    test_reserved_unbind_signal();
    test_reserved_unbind_at_byte_limit();
    test_admitted_unbind_generation(false, true);
    test_admitted_unbind_generation(false, false);
    test_admitted_unbind_generation(true, true);
    test_admitted_unbind_generation(true, false);
    const size_t resident = test_external_capacity(false);
    const size_t on_demand = test_external_capacity(true);
    assert(resident == on_demand + 8U * 8192U);
    test_external_on_demand_abort();
    puts("Signal admission, fragment loss, ACK policies, owner FIFO and bounded storage passed");
    return 0;
}
