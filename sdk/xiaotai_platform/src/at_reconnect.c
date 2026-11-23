// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include "platform_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"

#define ROUND_US INT64_C(30000000)

static struct {
    xiaotai_critical_t lock;
    xiaotai_signal_t wake;
    xiaotai_eventlog_topic_t *topic;
    bool initialized, received;
    uint32_t next_ticket, waiting;
    xiaotai_err_t result;
    atomic_bool busy;
    uint8_t round, count;
} diag;

static void suspended(xiaotai_err_t result, void *ctx)
{
    const uint32_t ticket = (uint32_t)(uintptr_t)ctx;
    xiaotai_critical_enter(&diag.lock);
    const bool current = ticket == diag.waiting;
    if (current) { diag.result = result; diag.received = true; }
    xiaotai_critical_exit(&diag.lock);
    if (current) xiaotai_signal_give(&diag.wake);
}

static xiaotai_err_t wait_suspended(int64_t due)
{
    for (;;) {
        const uint32_t wait = platform_remaining_ms(due);
        if (!wait) return XIAOTAI_ERR_TIMEOUT;
        xiaotai_critical_enter(&diag.lock);
        const bool received = diag.received;
        const xiaotai_err_t result = diag.result;
        xiaotai_critical_exit(&diag.lock);
        if (received) return result;
        (void)xiaotai_signal_take(&diag.wake, wait);
    }
}

static xiaotai_err_t wait_online(uint32_t revision, int64_t due)
{
    for (;;) {
        const uint32_t wait = platform_remaining_ms(due);
        if (!wait) return XIAOTAI_ERR_TIMEOUT;
        xiaotai_platform_status_t status;
        xiaotai_platform_status(&status);
        if (status.signaling && status.phase == XIAOTAI_PLATFORM_ONLINE && status.notify_revision != revision)
            return XIAOTAI_OK;
        xiaotai_delay_ms(wait < 10U ? wait : 10U);
    }
}

static bool reconnect_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    xiaotai_platform_status_t previous;
    xiaotai_platform_status(&previous);
    if (previous.held || previous.gated || !previous.signaling) {
        (void)xiaotai_at_fail(reply, previous.held || previous.gated ? XIAOTAI_ERR_BUSY : XIAOTAI_ERR_PLATFORM_OFFLINE,
                             "RECONNECT");
        return false;
    }
    const int64_t begin = xiaotai_time_us();
    const int64_t due = begin + ROUND_US;
    xiaotai_critical_enter(&diag.lock);
    if (++diag.next_ticket == 0) ++diag.next_ticket;
    const uint32_t ticket = diag.waiting = diag.next_ticket;
    diag.received = false;
    xiaotai_critical_exit(&diag.lock);
    uint32_t hold = 0;
    xiaotai_err_t err = xiaotai_platform_signaling_hold(suspended, (void *)(uintptr_t)ticket, &hold);
    if (err == XIAOTAI_OK) err = wait_suspended(due);
    xiaotai_critical_enter(&diag.lock);
    diag.waiting = 0;
    xiaotai_critical_exit(&diag.lock);
    const bool measured = err == XIAOTAI_OK && platform_signaling_heap_reaped();
    if (hold) xiaotai_platform_signaling_release(hold);
    if (err == XIAOTAI_OK) err = wait_online(previous.notify_revision, due);
    if (err != XIAOTAI_OK) {
        (void)xiaotai_at_fail(reply, err, "RECONNECT");
        return false;
    }

    const uint32_t elapsed = (uint32_t)((xiaotai_time_us() - begin) / 1000);
    ++diag.round;
    if (measured) {
        xiaotai_mem_stats_t memory;
        xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &memory);
        const uint32_t values[] = {diag.round, (uint32_t)memory.free, (uint32_t)memory.largest_block, elapsed};
        xiaotai_eventlog_post(diag.topic, values);
        xiaotai_eventlog_note("platform", "reconnect", diag.round, (int32_t)elapsed);
        xiaotai_at_printf(reply, "+PLATFORM-RECONNECT: round=%u free=%zu largest=%zu ms=%" PRIu32,
                         (unsigned)diag.round, memory.free, memory.largest_block, elapsed);
    } else {
        xiaotai_eventlog_warn("platform", "reconnect.heap_unavailable", diag.round, (int32_t)elapsed);
        xiaotai_at_printf(reply, "+PLATFORM-RECONNECT: round=%u free=unavailable largest=unavailable ms=%" PRIu32,
                         (unsigned)diag.round, elapsed);
    }
    return diag.round < diag.count;
}

static void reconnect_release(void *ctx)
{
    (void)ctx;
    atomic_store(&diag.busy, false);
}

static xiaotai_err_t reconnect(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    uint32_t rounds = 1;
    if (argc && !xiaotai_at_parse_u32(argv[0], 1, 50, &rounds)) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_exchange(&diag.busy, true)) return XIAOTAI_ERR_BUSY;
    diag.round = 0;
    diag.count = (uint8_t)rounds;
    const xiaotai_at_stream_t stream = {.next = reconnect_next, .release = reconnect_release};
    const xiaotai_err_t err = xiaotai_at_stream(reply, &stream);
    if (err != XIAOTAI_OK) reconnect_release(NULL);
    return err;
}

xiaotai_err_t xiaotai_platform_at_dev_register(void)
{
    static const char *const fields[] = {"round", "free", "largest", "ms"};
    static const xiaotai_eventlog_topic_desc_t topic = {
        .source = "platform", .name = "reconnect", .level = XIAOTAI_EVENTLOG_INFO,
        .fields = fields, .count = 4, .interval_ms = 1,
    };
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "RECONNECT", .min_args = 0, .max_args = 1, .usage = "[<1..50>]", .run = reconnect},
    };
    if (!diag.initialized) {
        xiaotai_critical_init(&diag.lock);
        const xiaotai_err_t err = xiaotai_signal_init(&diag.wake);
        if (err != XIAOTAI_OK) return err;
        diag.initialized = true;
    }
    const xiaotai_err_t err = xiaotai_eventlog_topic_register(&topic, &diag.topic);
    return err == XIAOTAI_OK ? xiaotai_at_verbs_register("AT+PLATFORM", verbs, 1) : err;
}
