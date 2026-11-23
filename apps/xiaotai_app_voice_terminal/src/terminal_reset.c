// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/port/os.h"

#define EFFECT(n) XIAOTAI_APP_VOICE_TERMINAL_EFFECT_##n
#define INTENT(n) XIAOTAI_INTENT_APP_VOICE_TERMINAL_##n
#define RESET_POLL_MS 50U
#define RESET_HANDOFF_MS 30000U

static const terminal_environment_t *const features[] = {
#ifdef XIAOTAI_TERMINAL_WITH_ENVIRONMENT
    &terminal_with_environment,
#endif
    NULL,
};

typedef enum { IDLE, ENVIRONMENT, NETWORK, BINDING, CONFIRM } reset_step_t;
static struct {
    reset_step_t step;
    bool network, holding, handoff_slow;
    uint32_t token, timer, store, revision, own_failure;
    int64_t due;
    const char *sound;
} reset;

static void poll(void *ctx);
static void retry_store(void *ctx);

static void release(void)
{
    xiaotai_timer_cancel(reset.timer);
    reset.timer = 0;
    reset.step = IDLE;
    reset.sound = NULL;
    terminal.reset_pending = false;
    if (reset.holding) xiaotai_provisioning_restart_release();
    reset.holding = false;
    if (!xiaotai_restart_pending()) terminal_binding_resume();
}

static bool arm(void)
{
    const int64_t left = reset.due - xiaotai_time_us();
    const uint32_t delay = left <= 0 ? 0 : left < 1000 * RESET_POLL_MS
        ? (uint32_t)((left + 999) / 1000) : RESET_POLL_MS;
    return xiaotai_timer_start(delay, poll, NULL, &reset.timer) == XIAOTAI_OK;
}

static void hold(void)
{
    if (reset.holding) return;
    reset.holding = true;
    xiaotai_provisioning_restart_hold();
}

static void confirm_done(xiaotai_err_t result, void *ctx)
{
    if (reset.step != CONFIRM || reset.token != (uint32_t)(uintptr_t)ctx) return;
    if (result != XIAOTAI_OK) xiaotai_eventlog_warn("terminal", "confirm.failed", 0, result);
    release();
}

static void confirm(const char *effect)
{
    reset.step = CONFIRM;
    reset.sound = effect;
    const uint32_t ms = terminal_config.confirm_timeout_ms ? terminal_config.confirm_timeout_ms : 30000U;
    reset.due = xiaotai_time_us() + (int64_t)ms * 1000;
    void *const token = (void *)(uintptr_t)reset.token;
    if (!reset.timer && !arm()) {
        confirm_done(XIAOTAI_ERR_NO_MEM, token);
        return;
    }
    const xiaotai_feedback_options_t sound = {.done = confirm_done, .ctx = token, .valid_ms = ms};
    const xiaotai_err_t err = xiaotai_feedback_fire(effect, &sound);
    if (err != XIAOTAI_OK) confirm_done(err, token);
}

static void refuse(xiaotai_err_t error, bool network)
{
    terminal_refuse(network ? INTENT(RESET_NETWORK) : INTENT(RESET_BINDING), error, false);
    release();
}

static void network_failed(xiaotai_err_t error, uint32_t revision)
{
    reset.own_failure = revision;
    xiaotai_eventlog_warn("terminal", "network.reset.failed", 0, error);
    (void)xiaotai_feedback_fire(EFFECT(FAILED), NULL);
    release();
}

static void binding_done(xiaotai_err_t result, void *ctx)
{
    if (reset.step != BINDING || reset.token != (uint32_t)(uintptr_t)ctx) return;
    if (result == XIAOTAI_OK) confirm(TERMINAL_CONFIRM_AUDIO);
    else {
        xiaotai_eventlog_warn("terminal", "binding.reset.failed", 0, result);
        release();
    }
}

static void begin(void)
{
    if (xiaotai_restart_pending() || xiaotai_quiesced()) {
        refuse(XIAOTAI_ERR_BUSY, reset.network);
        return;
    }
    reset.due = xiaotai_time_us() + (int64_t)RESET_HANDOFF_MS * 1000;
    reset.step = reset.network ? NETWORK : BINDING;
    if (!reset.network) (void)xiaotai_feedback_fire(EFFECT(CONNECTED), NULL);
    terminal_binding_suspend();
    if (reset.network) terminal_silence_ring();
    hold();
    if (reset.network) {
        xiaotai_provisioning_status_t status;
        xiaotai_provisioning_status(&status);
        reset.revision = status.revision;
        const xiaotai_err_t err = xiaotai_provisioning_begin(XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT);
        if (err != XIAOTAI_OK) network_failed(err, 0);
    } else {
        const xiaotai_err_t err = xiaotai_binding_forget_local(terminal_config.direct_binding_reset, binding_done,
                                                               (void *)(uintptr_t)reset.token);
        if (err != XIAOTAI_OK) binding_done(err, (void *)(uintptr_t)reset.token);
        else if (reset.step == BINDING) {
            // Storage guarantees its result; the cue timeout starts after it.
            xiaotai_timer_cancel(reset.timer);
            reset.timer = 0;
        }
    }
}

void terminal_reset_request(bool network)
{
    if (xiaotai_intent_from_external()) return;
    if (reset.step != IDLE || xiaotai_restart_pending()) {
        terminal_refuse(network ? INTENT(RESET_NETWORK) : INTENT(RESET_BINDING), XIAOTAI_ERR_BUSY, false);
        return;
    }
    reset.token = xiaotai_generation_next();
    reset.network = network;
    reset.handoff_slow = false;
    reset.step = ENVIRONMENT;
    reset.due = xiaotai_time_us() + (int64_t)RESET_HANDOFF_MS * 1000;
    terminal.reset_pending = true;
    if (!arm()) {
        refuse(XIAOTAI_ERR_NO_MEM, network);
        return;
    }
    for (size_t i = 0; features[i]; ++i) {
        bool waiting = false;
        const xiaotai_err_t err = features[i]->prepare(&waiting);
        if (err != XIAOTAI_OK) {
            refuse(err, network);
            return;
        }
        if (waiting) return;
    }
    begin();
}

void terminal_reset_environment_result(bool cancelled, xiaotai_err_t error)
{
    if (reset.step != ENVIRONMENT) return;
    if (cancelled) begin();
    else refuse(error != XIAOTAI_OK ? error : XIAOTAI_ERR_INVALID_STATE, reset.network);
}

static void network_progress(const xiaotai_provisioning_status_t *status)
{
    if (reset.step != NETWORK || status->revision == reset.revision) return;
    if (status->error != XIAOTAI_OK) {
        network_failed(status->error, status->revision);
    } else if ((status->provision.restart_required && status->restart_pending && xiaotai_restart_pending()) ||
               (status->phase == XIAOTAI_PROVISIONING_WAITING && !status->link.control_pending)) {
        confirm(EFFECT(CONFIRM));
    }
}

static void poll(void *ctx)
{
    (void)ctx;
    reset.timer = 0;
    if (reset.step == NETWORK) {
        xiaotai_provisioning_status_t status;
        xiaotai_provisioning_status(&status);
        network_progress(&status);
        const int64_t now = xiaotai_time_us();
        if (reset.step == NETWORK && now >= reset.due &&
            (status.link.control_pending || status.phase == XIAOTAI_PROVISIONING_STARTING)) {
            // An accepted write may still commit. Keep the hold until its result.
            if (!reset.handoff_slow) xiaotai_eventlog_warn("terminal", "network.reset.slow", 0, XIAOTAI_ERR_TIMEOUT);
            reset.handoff_slow = true;
            reset.due = now + (int64_t)RESET_HANDOFF_MS * 1000;
        }
    }
    if (reset.step == IDLE || reset.timer) return;
    if (xiaotai_time_us() < reset.due && arm()) return;
    if (reset.step == CONFIRM) {
        const char *const sound = reset.sound;
        confirm_done(XIAOTAI_ERR_TIMEOUT, (void *)(uintptr_t)reset.token);
        (void)xiaotai_feedback_cancel(sound, true);
    } else if (reset.step == NETWORK) network_failed(XIAOTAI_ERR_TIMEOUT, 0);
    else refuse(XIAOTAI_ERR_TIMEOUT, reset.network);
}

static void store_failed(void)
{
    if (reset.store) return;
    (void)xiaotai_feedback_fire(EFFECT(REFUSED), NULL);
    const uint32_t ms = terminal_config.store_retry_ms ? terminal_config.store_retry_ms : 2000U;
    if (xiaotai_timer_start(ms, retry_store, NULL, &reset.store) != XIAOTAI_OK)
        xiaotai_eventlog_warn("terminal", "store.retry.timer", 0, XIAOTAI_ERR_NO_MEM);
}

static void store_result(xiaotai_err_t result, bool restart, void *ctx)
{
    (void)ctx;
    if (result != XIAOTAI_OK) {
        store_failed();
        return;
    }
    xiaotai_timer_cancel(reset.store);
    reset.store = 0;
    if (reset.step != IDLE) return;
    reset.token = xiaotai_generation_next();
    terminal.reset_pending = true;
    if (restart) hold();
    confirm(EFFECT(CONFIRM));
}

static void retry_store(void *ctx)
{
    (void)ctx;
    reset.store = 0;
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    if (status.phase == XIAOTAI_PROVISIONING_STORE_FAILED && xiaotai_provisioning_retry() != XIAOTAI_OK)
        store_failed();
}

static void provisioning_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_CHANGED) == 0) {
        network_progress(&status);
    } else if (strcmp(event->args.text, "begin") == 0 || strcmp(event->args.text, "restart") == 0) {
        if (reset.step == NETWORK) network_progress(&status);
        else if (status.error != XIAOTAI_OK && (!reset.own_failure || reset.own_failure != status.revision)) {
            terminal.portal_error = true;
            terminal_refresh();
            (void)xiaotai_feedback_fire(EFFECT(FAILED), NULL);
        }
    }
}

xiaotai_err_t terminal_reset_init(void)
{
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, provisioning_event, NULL},
        {XIAOTAI_EVENT_PROVISIONING_FAILED, provisioning_event, NULL},
    };
    for (size_t i = 0; features[i]; ++i) {
        const xiaotai_err_t err = features[i]->init();
        if (err != XIAOTAI_OK) return err;
    }
    for (size_t i = 0; i < sizeof(events) / sizeof(*events); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&events[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return xiaotai_provisioning_observe_store(store_result, NULL);
}

void terminal_reset_teardown(void)
{
    memset(&reset, 0, sizeof(reset));
}
