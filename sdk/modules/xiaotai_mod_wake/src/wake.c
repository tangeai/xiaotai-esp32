// SPDX-License-Identifier: MIT
#include "wake_internal.h"

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"

#define SOURCE "wake"
#define SWITCH "wake.enabled"

static struct {
    xiaotai_local_wake_config_t config;
    xiaotai_local_wake_status_t status;
    xiaotai_err_t gate_error;
    uint32_t handed_token;
    bool initialized, started, requested, unavailable;
} s;

static xiaotai_local_wake_gate_t gate(void)
{
    if (s.unavailable) return XIAOTAI_LOCAL_WAKE_OFF_UNAVAILABLE;
    if (!s.status.enabled) return XIAOTAI_LOCAL_WAKE_OFF_DISABLED;
    if (xiaotai_quiesced()) return XIAOTAI_LOCAL_WAKE_OFF_QUIESCED;
    if (xiaotai_activity_current(NULL)) return XIAOTAI_LOCAL_WAKE_OFF_ACTIVITY;
    return XIAOTAI_LOCAL_WAKE_LISTENING;
}

static xiaotai_err_t apply_gate(void)
{
    s.status.gate = gate();
    if (s.unavailable) return XIAOTAI_ERR_NOT_SUPPORTED;
    const bool listen = s.status.gate == XIAOTAI_LOCAL_WAKE_LISTENING;
    if (s.requested == listen) return s.gate_error;
    s.requested = listen;
    s.gate_error = xiaotai_wake_listen(listen);
    if (s.gate_error == XIAOTAI_ERR_NOT_SUPPORTED) {
        s.unavailable = true;
        s.status.gate = XIAOTAI_LOCAL_WAKE_OFF_UNAVAILABLE;
    } else if (s.gate_error != XIAOTAI_OK) {
        xiaotai_eventlog_error(SOURCE, "gate", 0, s.gate_error);
    }
    return s.gate_error;
}

static void on_gate(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (s.started) (void)apply_gate();
}

static void on_setting(const char *name, void *ctx)
{
    (void)ctx;
    int32_t value;
    const xiaotai_err_t error = xiaotai_setting_get_int(name, &value);
    if (error != XIAOTAI_OK) {
        xiaotai_eventlog_error(SOURCE, "setting", 0, error);
        return;
    }
    s.status.enabled = value != 0;
    if (s.started) (void)apply_gate();
}

xiaotai_err_t xiaotai_local_wake_configure(const xiaotai_local_wake_config_t *config)
{
    if (s.initialized) return XIAOTAI_ERR_INVALID_STATE;
    s.config = config ? *config : (xiaotai_local_wake_config_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_local_wake_set_enabled(bool enabled)
{
    if (!s.started || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (s.config.user_switch) {
        const xiaotai_err_t error = xiaotai_setting_set_int(SWITCH, enabled);
        if (error != XIAOTAI_OK) return error;
    } else {
        s.status.enabled = enabled;
    }
    return apply_gate();
}

static void on_switch(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!args || args->value < -1 || args->value > 1) return;
    const xiaotai_err_t error = xiaotai_local_wake_set_enabled(args->value < 0 ? !s.status.enabled : args->value == 1);
    if (error != XIAOTAI_OK) xiaotai_eventlog_error(SOURCE, "switch", 0, error);
}

static void on_preroll_lost(uint32_t token, void *ctx)
{
    (void)ctx;
    if (s.handed_token == token) s.handed_token = 0;
    /* AI owns the token from the call to start, including a failed start. */
    (void)xiaotai_ai_talk_preroll_lost(token);
}

const char *local_wake_outcome_name(xiaotai_local_wake_outcome_t outcome)
{
    static const char *const names[] = {"started", "announced", "busy", "no_preroll", "refused"};
    return (unsigned)outcome < sizeof(names) / sizeof(names[0]) ? names[outcome] : "unknown";
}

static xiaotai_local_wake_outcome_t react(const xiaotai_wake_detection_t *detection, bool injected)
{
    xiaotai_local_wake_detection_t result = {
        .sequence = detection->sequence, .score_milli = detection->score_milli,
        .threshold_milli = detection->threshold_milli,
    };
    uint32_t token = 0, session = 0;
    if (injected) ++s.status.injected;
    else ++s.status.detections;

    if (!s.status.enabled || xiaotai_quiesced() || xiaotai_activity_current(NULL)) {
        result.outcome = XIAOTAI_LOCAL_WAKE_BUSY;
        ++s.status.busy;
    } else if (s.config.announce_only) {
        result.outcome = XIAOTAI_LOCAL_WAKE_ANNOUNCED;
        ++s.status.announced;
    } else {
        if (!injected) {
            const xiaotai_media_preroll_t request = {
                .wake_us = detection->captured_us, .keep_before_ms = s.config.keep_before_ms,
                .ttl_ms = s.config.preroll_ttl_ms, .lost = on_preroll_lost,
            };
            result.error = xiaotai_media_preroll_hold(&request, &token);
        }
        if (result.error != XIAOTAI_OK) {
            result.outcome = XIAOTAI_LOCAL_WAKE_NO_PREROLL;
            ++s.status.no_preroll;
        } else {
            const xiaotai_ai_talk_start_t request = {.origin = XIAOTAI_AI_TALK_ORIGIN_WAKE, .preroll_token = token};
            result.error = xiaotai_ai_talk_start(&request, &session);
            if (result.error == XIAOTAI_OK) {
                result.outcome = XIAOTAI_LOCAL_WAKE_STARTED;
                result.session = session;
                s.handed_token = token;
                ++s.status.started;
                (void)apply_gate();
            } else {
                result.outcome = XIAOTAI_LOCAL_WAKE_REFUSED;
                ++s.status.refused;
            }
        }
    }
    s.status.last = result;
    if (result.error == XIAOTAI_OK) xiaotai_eventlog_note(SOURCE, local_wake_outcome_name(result.outcome),
                                                       result.session, (int32_t)result.sequence);
    else xiaotai_eventlog_error(SOURCE, local_wake_outcome_name(result.outcome), result.sequence, result.error);

    const xiaotai_args_t args = {.value = (int32_t)result.outcome};
    const xiaotai_err_t error = xiaotai_event_emit(XIAOTAI_EVENT_WAKE_DETECTED, &args, &result, sizeof(result));
    if (error != XIAOTAI_OK) xiaotai_eventlog_error(SOURCE, "event", result.session, error);
    return result.outcome;
}

static void on_detection(const xiaotai_wake_detection_t *detection, void *ctx)
{
    (void)ctx;
    if (s.started && detection) (void)react(detection, false);
}

xiaotai_local_wake_outcome_t local_wake_inject(const xiaotai_wake_detection_t *phrase)
{
    return react(phrase, true);
}

void xiaotai_local_wake_status(xiaotai_local_wake_status_t *status)
{
    if (status) *status = s.status;
}

xiaotai_err_t local_wake_gate_error(void) { return s.gate_error; }

xiaotai_err_t xiaotai_module_wake_init(void)
{
    if (s.initialized || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    s.initialized = true;
    s.status.enabled = true;
    s.status.gate = XIAOTAI_LOCAL_WAKE_OFF_DISABLED;
    if (!s.config.keep_before_ms) s.config.keep_before_ms = 800;
    if (!s.config.preroll_ttl_ms) s.config.preroll_ttl_ms = 19000;
    if (!s.config.preroll_ms) s.config.preroll_ms = 20000;
    const xiaotai_media_requirement_t media = {.preroll_ms = s.config.preroll_ms};
    xiaotai_err_t error = xiaotai_media_require(&media);
    if (error == XIAOTAI_OK) error = xiaotai_wake_subscribe(on_detection, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_ACTIVITY, on_gate, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, on_gate, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_intent_handle(XIAOTAI_INTENT_WAKE_ENABLE, on_switch, NULL);
    if (error == XIAOTAI_OK && s.config.user_switch) {
        const xiaotai_setting_t setting = {.name = SWITCH, .key = "wake_on", .kind = XIAOTAI_SETTING_BOOL,
                                          .min = 0, .max = 1, .default_value = 1, .changed = on_setting};
        error = xiaotai_setting_register(&setting);
        if (error == XIAOTAI_OK) on_setting(SWITCH, NULL);
    }
    return error == XIAOTAI_OK ? local_wake_at_register() : error;
}

void xiaotai_module_wake_start(void)
{
    if (!s.initialized || s.started || !xiaotai_on_owner()) return;
    s.started = true;
    (void)apply_gate();
}

void xiaotai_local_wake_reset(void)
{
    /* Host tests stop the framework and engine before resetting modules. */
    s.config = (xiaotai_local_wake_config_t){0};
    s.status = (xiaotai_local_wake_status_t){0};
    s.gate_error = XIAOTAI_OK;
    s.handed_token = 0;
    s.initialized = s.started = s.requested = s.unavailable = false;
}
