// SPDX-License-Identifier: MIT
/* Guest session, grant, store and confirmation.
 *
 * The guest session runs under the temporary credentials of the report; the
 * code becomes visible only once it is subscribed. Only "auth_grant" counts,
 * and only the first usable one: later messages are counted and ignored, so a
 * redelivery never replaces what is being stored. A grant either carries both
 * credentials or keeps the stored identity (signed re-binding). A new identity
 * is stored before anything is acknowledged; a re-binding only compares, never
 * rewrites. The acknowledgement counts once the broker confirmed that very
 * publication (the platform core's guest_ack). */
#include <string.h>
#include "binding_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/json.h"
#include "xiaotai/net.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

static const char *TAG = "binding";

#define GRANT_TYPE "auth_grant"

/* ------------------------------------------------------------- parsing */

static bool credential(const xiaotai_json_doc_t *doc, int index, char *out, size_t capacity)
{
    size_t length = 0;
    if (index < 0 || doc->tokens[index].type != XIAOTAI_JSON_STRING) return false;
    if (xiaotai_json_string(doc, index, out, capacity, &length) != XIAOTAI_OK || length == 0U) return false;
    return true;
}

binding_grant_kind_t binding_grant_parse(const char *type, const char *json, size_t size, bool tolerant,
                                         xiaotai_identity_t *granted)
{
    if (type == NULL || strcmp(type, GRANT_TYPE) != 0) return BINDING_GRANT_OTHER;
    memset(granted, 0, sizeof(*granted));
    size_t count = 0;
    if (json == NULL || xiaotai_json_parse(json, size, NULL, 0, &count, NULL) != XIAOTAI_OK || count == 0U ||
        count > BINDING_JSON_TOKENS_MAX) {
        return BINDING_GRANT_MALFORMED;
    }
    xiaotai_json_token_t *tokens = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, count, sizeof(*tokens));
    if (tokens == NULL) return BINDING_GRANT_MALFORMED;
    binding_grant_kind_t kind = BINDING_GRANT_MALFORMED;
    xiaotai_json_doc_t doc;
    if (xiaotai_json_parse(json, size, tokens, count, &count, &doc) == XIAOTAI_OK &&
        tokens[0].type == XIAOTAI_JSON_OBJECT) {
        int payload = xiaotai_json_get(&doc, 0, "payload");
        uint8_t payload_type = payload >= 0 ? tokens[payload].type : 0U;
        if (payload < 0 || payload_type == XIAOTAI_JSON_NULL) {
            kind = BINDING_GRANT_KEEP;
        } else if (payload_type == XIAOTAI_JSON_STRING) {
            /* "" keeps the identity under the strict rules; any other string,
             * and "" under the tolerant ones, is not a grant. */
            kind = (!tolerant && tokens[payload].end == tokens[payload].start) ? BINDING_GRANT_KEEP
                                                                                 : BINDING_GRANT_MALFORMED;
        } else if (payload_type == XIAOTAI_JSON_OBJECT) {
            int id = xiaotai_json_get(&doc, payload, "device_id");
            int key = xiaotai_json_get(&doc, payload, "device_key");
            if (id < 0 && key < 0) {
                /* Strict: an object without either member keeps the identity;
                 * tolerant: only an empty object does. */
                kind = (!tolerant || tokens[payload].children == 0U) ? BINDING_GRANT_KEEP : BINDING_GRANT_MALFORMED;
            } else if (credential(&doc, id, granted->device_id, sizeof(granted->device_id)) &&
                       credential(&doc, key, granted->device_key, sizeof(granted->device_key))) {
                kind = BINDING_GRANT_IDENTITY;
            }
        }
    }
    xiaotai_mem_free(tokens);
    if (kind != BINDING_GRANT_IDENTITY) xiaotai_identity_wipe(granted, sizeof(*granted));
    return kind;
}

/* --------------------------------------------------------- confirmation */

static void ack_done(xiaotai_err_t result, void *ctx)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_serial_current(ctx) || binding_phase() != XIAOTAI_BINDING_CONFIRMING) return;
    if (result == XIAOTAI_OK) {
        binding_count(BINDING_STAT_CONFIRMATIONS);
        binding_succeed(attempt->identity_saved);
        return;
    }
    if (result == XIAOTAI_ERR_TIMEOUT) binding_count(BINDING_STAT_CONFIRM_TIMEOUTS);
    /* The identity is in storage already: the next retry reads it back and
     * reports signed instead of asking for another one. */
    binding_fail(result, "confirm", XIAOTAI_PLATFORM_NO_CODE);
}

static bool disconnected_by_user(void)
{
    if (!binding_config()->reject_manual_disconnect_during_commit) return false;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    return net.user_disconnected;
}

static void confirm_begin(void)
{
    binding_attempt_t *attempt = binding_attempt();
    if (disconnected_by_user()) {
        binding_count(BINDING_STAT_CANCELLED);
        binding_fail(XIAOTAI_ERR_INVALID_STATE, "confirm", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    binding_enter(XIAOTAI_BINDING_CONFIRMING);
    if (attempt->guest == 0U) {
        binding_fail(attempt->guest_closed != XIAOTAI_OK ? attempt->guest_closed : XIAOTAI_ERR_PLATFORM_STALE,
                     "confirm", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    const xiaotai_binding_config_t *config = binding_config();
    uint32_t timeout_ms = config->confirm_ms != 0U ? config->confirm_ms : binding_window_left_ms(attempt);
    if (timeout_ms == 0U) {
        /* The window ended while the identity was being stored. */
        binding_count(BINDING_STAT_CONFIRM_TIMEOUTS);
        binding_fail(XIAOTAI_ERR_TIMEOUT, "confirm", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    xiaotai_err_t result =
        xiaotai_platform_guest_ack(attempt->guest, timeout_ms, ack_done, binding_serial_ctx(attempt->serial));
    if (result != XIAOTAI_OK) binding_fail(result, "confirm", XIAOTAI_PLATFORM_NO_CODE);
}

/* ---------------------------------------------------------------- store */

static void store_done(xiaotai_err_t result)
{
    binding_attempt_t *attempt = binding_attempt();
    if (attempt == NULL || binding_phase() != XIAOTAI_BINDING_STORING) return;
    xiaotai_identity_wipe(&attempt->granted, sizeof(attempt->granted));
    if (result != XIAOTAI_OK) {
        binding_count(BINDING_STAT_STORE_FAILURES);
        binding_fail(result, "store", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    binding_count(BINDING_STAT_STORES);
    attempt->identity_saved = true;
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "stored", xiaotai_identity_generation(), 0);
    if (attempt->cancel_latched) {
        /* The user left the network or the platform moved on while storing:
         * what is stored stays, nothing is acknowledged. */
        binding_count(BINDING_STAT_CANCELLED);
        binding_fail(XIAOTAI_ERR_INVALID_STATE, "store", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    confirm_begin();
}

static void store_begin(void)
{
    binding_attempt_t *attempt = binding_attempt();
    if (disconnected_by_user()) {
        binding_count(BINDING_STAT_CANCELLED);
        binding_fail(XIAOTAI_ERR_INVALID_STATE, "grant", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    if (binding_config()->require_active_presave_window && binding_window_left_ms(attempt) == 0U) {
        binding_expire();
        return;
    }
    binding_enter(XIAOTAI_BINDING_STORING);
    if (!attempt->grant_has_identity || xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID) {
        /* Keep or re-confirm the stored identity (checked on receipt). */
        if (attempt->cancel_latched) {
            binding_count(BINDING_STAT_CANCELLED);
            binding_fail(XIAOTAI_ERR_INVALID_STATE, "store", XIAOTAI_PLATFORM_NO_CODE);
            return;
        }
        confirm_begin();
        return;
    }
    void *ctx = NULL;
    xiaotai_err_t result = binding_storage_begin(store_done, &ctx);
    if (result == XIAOTAI_OK) {
        result = xiaotai_identity_save(&attempt->granted, binding_storage_done, ctx);
        if (result == XIAOTAI_OK) return;
        binding_storage_cancel();
    }
    xiaotai_identity_wipe(&attempt->granted, sizeof(attempt->granted));
    binding_count(BINDING_STAT_STORE_FAILURES);
    binding_fail(result, "store", XIAOTAI_PLATFORM_NO_CODE);
}

/* -------------------------------------------------------- guest session */

/* A grant this attempt cannot use: ignored under the tolerant rules, the
 * attempt's end otherwise. */
static void unusable(binding_attempt_t *attempt, binding_stat_t stat, xiaotai_err_t error)
{
    binding_count(stat);
    xiaotai_identity_wipe(&attempt->granted, sizeof(attempt->granted));
    xiaotai_eventlog_warn(BINDING_LOG_SOURCE, stat == BINDING_STAT_GRANTS_MISMATCHED ? "grant.mismatch" : "grant.bad",
                          0, error);
    if (binding_config()->ignore_unusable_grants) return;
    binding_fail(error, "grant", XIAOTAI_PLATFORM_NO_CODE);
}

static void guest_message(uint32_t id, const xiaotai_platform_signal_t *signal, void *ctx)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_serial_current(ctx) || attempt->guest != id) return;
    if (binding_phase() != XIAOTAI_BINDING_WAITING || attempt->grant_received) {
        binding_count(BINDING_STAT_GRANTS_IGNORED);
        return;
    }
    if (binding_config()->resume_unbound_window_on_reconnect) {
        if (binding_window_left_ms(attempt) == 0U) {
            binding_expire();
            return;
        }
        if (!binding_code_visible() || !xiaotai_platform_guest_subscribed(id)) {
            binding_count(BINDING_STAT_GRANTS_IGNORED);
            return;
        }
    }
    bool tolerant = binding_config()->ignore_unusable_grants;
    binding_grant_kind_t kind = binding_grant_parse(signal->type, signal->json, signal->size, tolerant,
                                                    &attempt->granted);
    bool stored = xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID;
    switch (kind) {
    case BINDING_GRANT_OTHER:
        binding_count(BINDING_STAT_GRANTS_IGNORED);
        return;
    case BINDING_GRANT_MALFORMED:
        unusable(attempt, BINDING_STAT_GRANTS_MALFORMED, XIAOTAI_FAIL);
        return;
    case BINDING_GRANT_KEEP:
        /* Keeping needs something to keep. */
        if (!stored) {
            unusable(attempt, BINDING_STAT_GRANTS_MALFORMED, XIAOTAI_FAIL);
            return;
        }
        attempt->grant_has_identity = false;
        break;
    case BINDING_GRANT_IDENTITY:
        if (stored && !xiaotai_identity_matches(attempt->granted.device_id, attempt->granted.device_key)) {
            unusable(attempt, BINDING_STAT_GRANTS_MISMATCHED, XIAOTAI_ERR_INVALID_ARG);
            return;
        }
        attempt->grant_has_identity = true;
        break;
    }
    attempt->grant_received = true;
    binding_count(BINDING_STAT_GRANTS);
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "granted", signal->generation, attempt->grant_has_identity);
    binding_window_stop();
    binding_voice_halt();
    store_begin();
}

static void connect_timeout(void *ctx)
{
    if (!binding_serial_current(ctx) || binding_phase() != XIAOTAI_BINDING_CONNECTING) return;
    binding_count(BINDING_STAT_CONNECT_TIMEOUTS);
    binding_fail(XIAOTAI_ERR_TIMEOUT, "connect", XIAOTAI_PLATFORM_NO_CODE);
}

static void guest_ready(uint32_t id, void *ctx)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_serial_current(ctx) || attempt->guest != id) return;
    if (binding_config()->resume_unbound_window_on_reconnect && !xiaotai_platform_guest_subscribed(id)) return;
    if (binding_can_resume_window(attempt) && !attempt->network_suspended && !binding_code_visible()) {
        if (binding_window_left_ms(attempt) == 0U) binding_expire();
        else binding_restore_code();
        return;
    }
    if (binding_phase() != XIAOTAI_BINDING_CONNECTING) return;
    /* The ready callback can run before the due timer in this owner turn. */
    if (!binding_window_left_ms(attempt)) {
        connect_timeout(ctx);
        return;
    }
    binding_timer_stop();
    binding_window_arm();
    if (binding_attempt() != attempt) return;
    const uint32_t left_ms = binding_window_left_ms(attempt);
    if (!left_ms) {
        connect_timeout(ctx);
        return;
    }
    binding_enter(XIAOTAI_BINDING_WAITING);
    XIAOTAI_LOGI(TAG, "code visible: left_ms=%u", (unsigned)left_ms);
    binding_publish_code((left_ms + 999U) / 1000U);
}

static void guest_unready(uint32_t id, void *ctx)
{
    const binding_attempt_t *attempt = binding_attempt();
    if (binding_serial_current(ctx) && attempt->guest == id && binding_can_resume_window(attempt))
        binding_hide_code();
}

static void resume_after_retire(void *ctx)
{
    if (binding_serial_current(ctx)) binding_resume_window();
}

static void guest_closed(uint32_t id, xiaotai_err_t reason, void *ctx)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_serial_current(ctx) || attempt->guest != id) return;
    attempt->guest = 0;
    attempt->guest_closed = reason != XIAOTAI_OK ? reason : XIAOTAI_FAIL;
    xiaotai_eventlog_warn(BINDING_LOG_SOURCE, "guest.closed", 0, attempt->guest_closed);
    xiaotai_binding_phase_t phase = binding_phase();
    /* Storing goes on; the confirmation then fails for want of a session. */
    if (phase == XIAOTAI_BINDING_STORING || phase == XIAOTAI_BINDING_CONFIRMING) return;
    if (reason == XIAOTAI_ERR_PLATFORM_OFFLINE && binding_can_resume_window(attempt) &&
        attempt->network_suspended) {
        /* The platform retires the old guest after this callback returns. */
        if (xiaotai_run(resume_after_retire, ctx) != XIAOTAI_OK)
            binding_timer_arm(1, resume_after_retire);
        return;
    }
    binding_fail(attempt->guest_closed, binding_step_name(phase), XIAOTAI_PLATFORM_NO_CODE);
}

static xiaotai_err_t open_guest(binding_attempt_t *attempt, int64_t deadline_us)
{
    const xiaotai_binding_config_t *config = binding_config();
    const xiaotai_platform_guest_t guest = {
        .client_id = attempt->temp_client_id,
        .token = attempt->temp_token,
        .ready = guest_ready,
        .message = guest_message,
        .closed = guest_closed,
        .ctx = binding_serial_ctx(attempt->serial),
        .subscribe_timeout_s = config->guest_subscribe_timeout_s,
        .initial_subscribe_deadline_us = deadline_us,
        .unready = config->resume_unbound_window_on_reconnect && !attempt->signed_report ? guest_unready : NULL,
    };
    return xiaotai_platform_guest_open(&guest, &attempt->guest);
}

static int64_t connect_deadline(const binding_attempt_t *attempt)
{
    const xiaotai_binding_config_t *config = binding_config();
    uint32_t seconds = config->connect_s;
    if (config->guest_subscribe_timeout_s && config->guest_subscribe_timeout_s < seconds)
        seconds = config->guest_subscribe_timeout_s;
    const int64_t bound = xiaotai_time_us() + (int64_t)seconds * 1000000;
    return bound < attempt->deadline_us ? bound : attempt->deadline_us;
}

void binding_resume_window(void)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_can_resume_window(attempt) || !attempt->network_suspended || attempt->guest) return;
    int64_t expiry_s = 0;
    if (binding_window_left_ms(attempt) == 0U ||
        (xiaotai_platform_token_expiry(attempt->temp_token, &expiry_s) == XIAOTAI_OK &&
         xiaotai_clock_unix_ms() / 1000 >= expiry_s)) {
        binding_expire();
        return;
    }
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (net.state != XIAOTAI_NET_UP || net.user_disconnected || !platform.clock_synced ||
        platform.phase != XIAOTAI_PLATFORM_NO_IDENTITY || platform.authorized || xiaotai_quiesced()) return;
    const xiaotai_err_t result = open_guest(attempt, connect_deadline(attempt));
    if (result == XIAOTAI_OK) {
        attempt->network_suspended = false;
        attempt->guest_closed = XIAOTAI_OK;
    } else if (result == XIAOTAI_ERR_TIMEOUT) binding_expire();
    else if (result != XIAOTAI_ERR_PLATFORM_OFFLINE) binding_fail(result, "connect", XIAOTAI_PLATFORM_NO_CODE);
}

void binding_connect_begin(void)
{
    binding_attempt_t *attempt = binding_attempt();
    if (attempt == NULL) return;
    const xiaotai_binding_config_t *config = binding_config();
    const uint32_t limit_ms = (uint32_t)config->connect_s * 1000U;
    const int64_t deadline_us = config->guest_subscribe_timeout_s ? connect_deadline(attempt) : 0;
    void *ctx = binding_serial_ctx(attempt->serial);
    xiaotai_err_t result = open_guest(attempt, deadline_us);
    if (result != XIAOTAI_OK) {
        attempt->guest = 0;
        binding_fail(result, "connect", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    /* The subscription must be confirmed within connect_s, and before the
     * window ends. */
    if (deadline_us) {
        const int64_t left_us = deadline_us - xiaotai_time_us();
        if (left_us <= 0) connect_timeout(ctx);
        else binding_timer_arm((uint32_t)((left_us + 999) / 1000), connect_timeout);
    } else {
        const uint32_t window_ms = binding_window_left_ms(attempt);
        binding_timer_arm(window_ms < limit_ms ? window_ms : limit_ms, connect_timeout);
    }
}

void binding_guest_close(binding_attempt_t *attempt)
{
    if (attempt == NULL || attempt->guest == 0U) return;
    uint32_t id = attempt->guest;
    attempt->guest = 0;
    xiaotai_platform_guest_close(id);
}
