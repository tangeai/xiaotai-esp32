// SPDX-License-Identifier: MIT
#include <string.h>
#include "guest.h"
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/clock.h"

static const char *const event_name[PLATFORM_EVENT_COUNT] = {
    XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, XIAOTAI_EVENT_PLATFORM_PAUSED, XIAOTAI_EVENT_PLATFORM_OFFLINE,
    XIAOTAI_EVENT_PLATFORM_ONLINE, XIAOTAI_EVENT_PLATFORM_PROFILE, XIAOTAI_EVENT_PLATFORM_DISCOVERED,
    XIAOTAI_EVENT_PLATFORM_CLOCK, XIAOTAI_EVENT_PLATFORM_IDLE,
};

static uint16_t event_bit(platform_event_t event) { return (uint16_t)(1U << event); }

static bool event_emit(platform_event_t event, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    if (xiaotai_event_emit(event_name[event], &args, NULL, 0) == XIAOTAI_OK) return true;
    ++platform.stats.event_post_retries;
    return false;
}

void platform_event_publish(platform_event_t event, int32_t value)
{
    const bool waiting = platform.event_pending != 0;
    if (event == PLATFORM_EVENT_OFFLINE) platform_event_cancel_online();
    if (!waiting && event_emit(event, value)) return;
    platform.event_values[event] = value;
    platform.event_pending |= event_bit(event);
}

void platform_events_flush(void)
{
    platform_lock();
    if (platform.status.exchanges) platform.event_pending &= (uint16_t)~event_bit(PLATFORM_EVENT_IDLE);
    for (platform_event_t i = 0; i < PLATFORM_EVENT_COUNT; ++i) {
        if (!(platform.event_pending & event_bit(i))) continue;
        if (!event_emit(i, platform.event_values[i])) break;
        platform.event_pending &= (uint16_t)~event_bit(i);
    }
    platform_unlock();
}

void platform_event_cancel_online(void)
{
    platform.event_pending &= (uint16_t)~(event_bit(PLATFORM_EVENT_ONLINE) | event_bit(PLATFORM_EVENT_PROFILE));
}

bool platform_event_binding_offline(void)
{
    return event_emit(PLATFORM_EVENT_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_BINDING);
}

static bool session_network_current(uint32_t revision)
{
    return platform.network_up && platform.network_revision == revision &&
        !atomic_load(&platform.stopped);
}

bool platform_session_current(uint32_t revision)
{
    return !platform.gate && !platform_binding_in_transition() && session_network_current(revision);
}

static xiaotai_err_t setup_body(void *ctx, const uint8_t *data, size_t size)
{
    platform_setup_reply_t *reply = ctx;
    if (!reply->body) {
        const int64_t length = reply->response.content_length;
        if (length > PLATFORM_SETUP_BODY_BYTES - 1) return XIAOTAI_ERR_INVALID_SIZE;
        reply->capacity = length > 0 ? (size_t)length : PLATFORM_SETUP_BODY_BYTES - 1U;
        if (platform.response && platform.response_limit >= reply->capacity) reply->body = platform.response;
        else {
            reply->body = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, reply->capacity);
            reply->allocated = reply->body != NULL;
        }
        if (!reply->body) return XIAOTAI_ERR_NO_MEM;
    }
    if (size > reply->capacity - reply->size) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(reply->body + reply->size, data, size);
    reply->size += size;
    return XIAOTAI_OK;
}

void platform_setup_reply_release(platform_setup_reply_t *reply, bool wipe)
{
    if (wipe && reply->body) xiaotai_identity_wipe(reply->body, reply->size);
    if (reply->allocated) xiaotai_mem_free(reply->body);
}

xiaotai_err_t platform_session_http(uint32_t revision, const xiaotai_http_request_t *req,
                                    platform_setup_reply_t *reply)
{
    /* Setup follows the same worker and client owner as queued exchanges. It
     * neither opens a second client beside a kept connection nor replays a
     * signed login on failure. */
    xiaotai_http_t *client = platform.client;
    if (!client) {
        const xiaotai_http_options_t options = {.no_delay = platform.config.http_no_delay};
        const xiaotai_err_t err = xiaotai_http_create(&options, &client);
        if (err != XIAOTAI_OK) return err;
    } else xiaotai_http_close(client);
    platform_lock();
    const bool admitted = platform_session_current(revision);
    if (admitted) platform.client = platform.session.http = client;
    platform.keep_until = 0;
    platform_unlock();
    reply->response = (xiaotai_http_response_t){.sink = setup_body, .sink_ctx = reply,
        .sink_limit = PLATFORM_SETUP_BODY_BYTES - 1U};
    xiaotai_err_t err = admitted ? xiaotai_http_perform(client, req, &reply->response) : XIAOTAI_ERR_PLATFORM_STALE;
    platform_lock();
    platform.client = platform.session.http = NULL;
    if (!platform_session_current(revision)) err = XIAOTAI_ERR_PLATFORM_STALE;
    platform_unlock();
    xiaotai_http_destroy(client);
    return err;
}

static bool session_clear(void)
{
    platform_session_t *s = &platform.session;
    platform_holds_drop_bounded();
    atomic_store(&s->receive_generation, 0);
    const bool was_live = platform.status.authorized || platform.status.signaling;
    platform.status.authorized = platform.status.signaling = platform.status.profile_ready = false;
    platform_profile_offline();
    ++platform.session_revision;
    platform.connection_close = true;
    if (platform.token) xiaotai_identity_wipe(platform.token, XIAOTAI_PLATFORM_BEARER_MAX);
    s->expiry = s->refresh_at = 0;
    s->refresh_retry = false;
    s->resume_link = false;
    s->auth_pending = false;
    s->ready_pending = false;
    if (s->link_live) {
        s->link_live = false;
        s->destroy_pending = true;
    }
    return was_live;
}

static void session_end(xiaotai_platform_offline_t reason)
{
    if (session_clear()) platform_event_publish(PLATFORM_EVENT_OFFLINE, reason);
}

void platform_session_binding_clean(void)
{
    platform_session_t *s = &platform.session;
    s->link_live = s->destroy_pending = s->destroy_failed = s->pause_after_destroy = false;
    (void)session_clear();
    s->retry_at = 0;
}

void platform_session_network(bool up, const uint8_t *mac, bool net_event, uint32_t generation)
{
    if (!platform.objects || platform.session.boot_failed) return;
    platform_lock();
    if (xiaotai_platform_start_failed()) {
        platform_unlock();
        return;
    }
    platform_session_t *s = &platform.session;
    if (up && net_event && !generation) up = false;
    if (up && !net_event && !mac) up = false;
    const bool same = up && ((net_event && s->net_event && s->address_generation == generation) ||
        (!net_event && !s->net_event && s->mac_valid && memcmp(mac, s->mac, sizeof(s->mac)) == 0));
    if (same && (s->mac_valid || !mac)) {
        platform_unlock();
        return;
    }
    if (platform.network_up && !same) {
        ++platform.network_revision;
        if (s->http) xiaotai_http_cancel(s->http);
        else if (platform.client && platform.io) xiaotai_http_cancel(platform.client);
        if (!platform_binding_in_transition()) {
            session_end(XIAOTAI_PLATFORM_OFFLINE_NETWORK);
            s->pause_after_destroy = false;
            platform.status.phase = XIAOTAI_PLATFORM_STOPPED;
            platform.status.last_error = XIAOTAI_OK;
        }
    }
    s->net_event = up && net_event;
    s->address_generation = up ? generation : 0;
    s->mac_valid = up && mac;
    platform.network_up = s->mac_valid;
    if (s->mac_valid) memcpy(s->mac, mac, sizeof(s->mac));
    else memset(s->mac, 0, sizeof(s->mac));
    s->clock_deadline = s->retry_at = 0;
    platform_unlock();
    platform_wake();
}

xiaotai_err_t platform_session_link_register(const platform_session_link_t *link)
{
    if (!link || !link->start || !link->destroy) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&platform.started) || platform.session.link.start) return XIAOTAI_ERR_INVALID_STATE;
    platform.session.link = *link;
    return XIAOTAI_OK;
}

xiaotai_err_t platform_session_guest_register(xiaotai_err_t (*before_login)(void *), void *ctx)
{
    if (!before_login) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&platform.started) || platform.session.before_login) return XIAOTAI_ERR_INVALID_STATE;
    platform.session.before_login = before_login;
    platform.session.guest_ctx = ctx;
    return XIAOTAI_OK;
}

static void link_ready(void)
{
    platform_binding_online();
    platform.status.signaling = true;
    platform.status.phase = XIAOTAI_PLATFORM_ONLINE;
    platform.status.last_error = XIAOTAI_OK;
    ++platform.status.notify_revision;
    ++platform.stats.signaling_connects;
    platform_profile_online();
    platform_event_publish(PLATFORM_EVENT_ONLINE, 0);
}

void platform_session_link_event(uint32_t epoch, platform_link_event_t event)
{
    if (!platform.objects) return;
    platform_lock();
    platform_session_t *s = &platform.session;
    if (s->link_live && s->link_epoch == epoch && platform.status.authorized && platform.network_up &&
        platform.status.generation == xiaotai_identity_generation()) {
        if (event == PLATFORM_LINK_READY && !platform.status.signaling && !platform.status.held) {
            if (s->link_starting) s->ready_pending = true;
            else link_ready();
        } else if (event == PLATFORM_LINK_DROPPED) {
            s->ready_pending = false;
            if (platform.status.signaling) {
                platform_holds_drop_bounded();
                platform.status.signaling = platform.status.profile_ready = false;
                platform_profile_offline();
                platform.status.phase = XIAOTAI_PLATFORM_CONNECTING;
                ++platform.stats.signaling_drops;
                if (platform.config.requests_need_signaling) {
                    ++platform.session_revision;
                    platform.connection_close = true;
                }
                platform_event_publish(PLATFORM_EVENT_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
            }
        } else if (event == PLATFORM_LINK_AUTH_REFUSED && platform.config.auth_failure != XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE)
            s->auth_pending = true;
    }
    platform_unlock();
    platform_wake();
}

void platform_session_auth_failed(void)
{
    platform_lock();
    if (platform.status.authorized && platform.config.auth_failure != XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE)
        platform.session.auth_pending = true;
    platform_unlock();
    platform_wake();
}

xiaotai_err_t xiaotai_platform_retry(void)
{
    if (!platform.objects || !atomic_load(&platform.running) || platform.session.boot_failed)
        return XIAOTAI_ERR_PLATFORM_OFFLINE;
    platform_lock();
    xiaotai_err_t err = XIAOTAI_OK;
    if (platform.gate) err = XIAOTAI_ERR_PLATFORM_GATED;
    else if (!platform.session.step_running && (!platform.status.authorized || platform.session.destroy_pending) &&
             (!platform_binding_in_transition() || platform.session.destroy_failed)) {
        platform.session.retry_at = 0;
        platform.session.destroy_failed = false;
        if (platform.status.phase == XIAOTAI_PLATFORM_PAUSED || platform.status.phase == XIAOTAI_PLATFORM_NO_IDENTITY ||
            platform.status.phase == XIAOTAI_PLATFORM_UNBOUND) platform.status.phase = platform.status.authorized ?
                XIAOTAI_PLATFORM_CONNECTING : XIAOTAI_PLATFORM_STOPPED;
    }
    platform_unlock();
    if (err == XIAOTAI_OK) platform_wake();
    return err;
}

static void pause(xiaotai_err_t err)
{
    platform.status.phase = XIAOTAI_PLATFORM_PAUSED;
    platform.status.last_error = err;
    platform_event_publish(PLATFORM_EVENT_PAUSED, err);
}

static void failed(xiaotai_err_t err)
{
    platform.status.last_error = err;
    if (platform.config.retry_ms == XIAOTAI_PLATFORM_NO_RETRY) pause(err);
    else platform.session.retry_at = xiaotai_time_us() + (int64_t)platform.config.retry_ms * 1000;
}

static void autologin(int64_t now)
{
    platform.session.last_autologin = now;
    platform.session.autologin_done = true;
    platform.session.retry_at = 0;
    platform.session.refresh_retry = false;
    platform.status.phase = XIAOTAI_PLATFORM_LOGIN;
}

static bool ready_for_login(uint32_t revision, uint32_t generation)
{
    return !platform.status.held && platform_session_current(revision) && generation == xiaotai_identity_generation();
}

static uint32_t claim_link(platform_session_t *s)
{
    if (++s->link_epoch == 0) ++s->link_epoch;
    s->link_live = s->link_starting = true;
    atomic_store(&s->receive_generation, platform.status.generation);
    return s->link_epoch;
}

static void start_link(uint32_t revision, uint32_t generation, uint32_t epoch,
                       const char *broker, const char *device, const char *token, bool resuming)
{
    platform_session_t *s = &platform.session;
    const xiaotai_err_t err = s->link.start(s->link.ctx, epoch, broker, device, token);
    platform_lock();
    s->link_starting = false;
    if (err == XIAOTAI_OK) ++platform.stats.signaling_rebuilds;
    if (platform_binding_in_transition()) {
        /* The binding transition owns this handle now. Keep credentials and
         * the handle until its OFFLINE handlers have finished. */
        s->ready_pending = false;
        platform_unlock();
        return;
    }
    /* Login HTTP already committed. Only network or identity invalidation
     * interrupts that provider, not an HTTP maintenance gate. */
    const bool current = session_network_current(revision) && generation == xiaotai_identity_generation();
    if (err != XIAOTAI_OK || !current) {
        if (resuming && current) {
            s->link_live = false;
            atomic_store(&s->receive_generation, 0);
            s->destroy_pending = true;
            s->retry_at = xiaotai_time_us() + 1000000;
            platform.status.last_error = err;
        } else {
            if (s->link_live) session_end(err != XIAOTAI_OK ? XIAOTAI_PLATFORM_OFFLINE_SIGNALING :
                XIAOTAI_PLATFORM_OFFLINE_MAINTENANCE);
            if (err != XIAOTAI_OK && current) failed(err);
        }
    } else {
        s->resume_link = false;
        if (s->ready_pending && !s->auth_pending && !platform.status.held) link_ready();
    }
    s->ready_pending = false;
    platform_unlock();
}

static void resume_link(uint32_t revision, uint32_t generation)
{
    platform_session_t *s = &platform.session;
    xiaotai_identity_t identity = {0};
    char token[XIAOTAI_PLATFORM_BEARER_MAX] = {0}, broker[XIAOTAI_PLATFORM_URL_MAX];
    xiaotai_err_t err = xiaotai_identity_get(&identity);
    if (err == XIAOTAI_OK) err = xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, broker, sizeof(broker));
    uint32_t epoch = 0;
    platform_lock();
    if (!platform.status.held && platform.status.authorized && s->resume_link &&
        session_network_current(revision) && generation == xiaotai_identity_generation()) {
        if (err == XIAOTAI_OK) {
            strcpy(token, platform.token);
            epoch = claim_link(s);
        } else {
            s->retry_at = xiaotai_time_us() + 1000000;
            platform.status.last_error = err;
        }
    }
    platform_unlock();
    if (epoch) start_link(revision, generation, epoch, broker, identity.device_id, token, true);
    xiaotai_identity_wipe(token, sizeof(token));
    xiaotai_identity_wipe(&identity, sizeof(identity));
}

static void login(uint32_t revision, uint32_t generation, const uint8_t mac[6])
{
    platform_session_t *s = &platform.session;
    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U], token[XIAOTAI_PLATFORM_BEARER_MAX] = {0};
    bool unbound = false;
    int64_t expiry = 0;
    xiaotai_err_t err = s->before_login ? s->before_login(s->guest_ctx) : platform_guest_before_login(NULL);
    if (err == XIAOTAI_ERR_BUSY) {
        platform_lock();
        if (ready_for_login(revision, generation)) s->retry_at = xiaotai_time_us() + PLATFORM_OWNER_REPOST_MS * INT64_C(1000);
        platform_unlock();
        return;
    }
    if (err == XIAOTAI_OK) {
        platform_lock();
        ++platform.stats.logins;
        platform_unlock();
        err = platform_login(revision, mac, device, token, &unbound, &expiry);
    }
    char broker[XIAOTAI_PLATFORM_URL_MAX];
    if (err == XIAOTAI_OK && !unbound)
        err = xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, broker, sizeof(broker));
    bool start = false;
    uint32_t epoch = 0;
    platform_lock();
    if (ready_for_login(revision, generation)) {
        if (err != XIAOTAI_OK) {
            ++platform.stats.login_failures;
            failed(err);
        } else if (unbound) {
            platform_binding_login_unbound();
        } else {
            platform.status.authorized = true;
            platform.status.phase = XIAOTAI_PLATFORM_CONNECTING;
            platform.status.generation = generation;
            platform.status.last_error = XIAOTAI_OK;
            s->expiry = expiry;
            s->refresh_at = platform.config.token_refresh ?
                platform_token_refresh_at(expiry, xiaotai_clock_unix_ms() / 1000) : 0;
            strcpy(platform.token, token);
            start = s->link.start != NULL;
            if (start) epoch = claim_link(s);
        }
    }
    platform_unlock();
    if (start) start_link(revision, generation, epoch, broker, device, token, false);
    xiaotai_identity_wipe(token, sizeof(token));
}

uint32_t platform_session_step(void)
{
    platform_session_t *s = &platform.session;
    platform_lock();
    const uint32_t hold_wait = platform_holds_check();
    if (!s->enabled) {
        platform_holds_finish(XIAOTAI_OK);
        platform_unlock();
        return XIAOTAI_WAIT_FOREVER;
    }
    if (platform_binding_in_transition()) {
        platform_unlock();
        return XIAOTAI_WAIT_FOREVER;
    }
    const int64_t now = xiaotai_time_us();
    if (s->auth_pending) {
        session_end(XIAOTAI_PLATFORM_OFFLINE_AUTH);
        if (platform.config.auth_failure == XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN &&
            (!s->autologin_done || now - s->last_autologin > (int64_t)XIAOTAI_PLATFORM_RELOGIN_INTERVAL_MS * 1000))
            autologin(now);
        else if (s->destroy_pending) {
            platform.status.phase = XIAOTAI_PLATFORM_STOPPED;
            s->pause_after_destroy = true;
        }
        else pause(XIAOTAI_FAIL);
    }
    if (platform.status.held && s->link_live) {
        atomic_store(&s->receive_generation, 0);
        if (platform.status.signaling && platform.config.requests_need_signaling) {
            ++platform.session_revision;
            platform.connection_close = true;
        }
        platform.status.signaling = platform.status.profile_ready = false;
        platform_profile_offline();
        platform.status.phase = XIAOTAI_PLATFORM_CONNECTING;
        platform.event_pending &= (uint16_t)~(event_bit(PLATFORM_EVENT_ONLINE) | event_bit(PLATFORM_EVENT_PROFILE));
        s->link_live = s->ready_pending = false;
        s->resume_link = platform.status.authorized;
        s->destroy_pending = true;
    }
    if (s->destroy_pending && !s->destroy_failed) {
        const bool bounded = s->bounded_hold != 0;
        platform_unlock();
        const xiaotai_err_t err = s->link.destroy(s->link.ctx);
        platform_lock();
        if (err == XIAOTAI_OK) {
            s->destroy_pending = false;
            if (s->pause_after_destroy) pause(XIAOTAI_FAIL);
        } else {
            s->destroy_failed = true;
            pause(err);
            platform_holds_finish(err);
            /* The owner or the worker may have released the lease while
             * destroy was in flight. The failure still gets one safe retry. */
            if (bounded && !s->bounded_hold) platform_holds_retry_destroy();
        }
        s->pause_after_destroy = false;
        platform_unlock();
        return 0;
    }
    if (platform.status.held) {
        if (!s->destroy_pending) platform_holds_finish(XIAOTAI_OK);
        else if (s->destroy_failed) platform_holds_finish(platform.status.last_error);
        /* The transport cannot start and release its hold until it has a
         * service endpoint. Login/subscription and ordinary holds still wait. */
        if (platform.status.discovered || !platform_holds_allow_discovery()) {
            platform_unlock();
            return hold_wait;
        }
    }
    if (platform.network_up && platform.status.authorized && s->resume_link && !s->destroy_pending) {
        if (platform.status.generation != xiaotai_identity_generation()) {
            session_end(XIAOTAI_PLATFORM_OFFLINE_BINDING);
            platform.status.phase = XIAOTAI_PLATFORM_STOPPED;
            platform_unlock();
            return 0;
        }
        if (platform.config.hold_relogin_on_release) {
            if (!platform.gate) {
                /* A hold has no OFFLINE event, even when this product logs in
                 * again instead of keeping the token on the final release. */
                (void)session_clear();
                platform.status.phase = XIAOTAI_PLATFORM_LOGIN;
                s->retry_at = 0;
                platform_unlock();
                return 0;
            }
            platform_unlock();
            return XIAOTAI_WAIT_FOREVER;
        }
        const int64_t wall = xiaotai_clock_unix_ms() / 1000;
        const bool token_expired = s->expiry && wall >= s->expiry;
        const bool renewal_due = s->refresh_at && wall >= s->refresh_at;
        if (!token_expired && (platform.gate || !renewal_due)) {
            const uint32_t revision = platform.network_revision, generation = platform.status.generation;
            const uint32_t wait = s->retry_at > now ? platform_remaining_ms(s->retry_at) : 0;
            platform_unlock();
            if (!wait) resume_link(revision, generation);
            return wait;
        }
        if (token_expired && !platform.gate && !renewal_due) {
            session_end(XIAOTAI_PLATFORM_OFFLINE_REFRESH);
            autologin(now);
            platform_unlock();
            return 0;
        }
    }
    if (!platform.network_up || platform.gate || s->destroy_pending || platform.status.phase == XIAOTAI_PLATFORM_PAUSED ||
        platform.status.phase == XIAOTAI_PLATFORM_NO_IDENTITY || platform.status.phase == XIAOTAI_PLATFORM_UNBOUND) {
        platform_unlock();
        return XIAOTAI_WAIT_FOREVER;
    }
    if (platform.status.authorized) {
        const int64_t wall = xiaotai_clock_unix_ms() / 1000;
        if (!s->refresh_at || wall < s->refresh_at) {
            const uint32_t wait = s->refresh_at ? 1000U : XIAOTAI_WAIT_FOREVER;
            platform_unlock();
            return wait;
        }
        ++platform.stats.token_renewals;
        session_end(XIAOTAI_PLATFORM_OFFLINE_REFRESH);
        const int64_t cooldown = (int64_t)XIAOTAI_PLATFORM_RELOGIN_INTERVAL_MS * 1000;
        if (platform.config.auth_failure == XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN && s->autologin_done &&
            now - s->last_autologin < cooldown) {
            platform.status.phase = XIAOTAI_PLATFORM_LOGIN;
            s->retry_at = s->last_autologin + cooldown;
            s->refresh_retry = true;
        } else autologin(now);
        platform_unlock();
        return 0;
    }
    if (s->retry_at > now) {
        const uint32_t wait = platform_remaining_ms(s->retry_at);
        platform_unlock();
        return wait;
    }
    if (s->refresh_retry) autologin(now);

    const uint32_t revision = platform.network_revision;
    const uint32_t generation = xiaotai_identity_generation();
    platform.status.generation = generation;
    const bool clock_valid = xiaotai_clock_synced();
    const bool clock = !platform.status.clock_synced || !clock_valid, discovery = !platform.status.discovered;
    if (!clock && !discovery && xiaotai_identity_state() != XIAOTAI_IDENTITY_VALID) {
        platform.status.phase = XIAOTAI_PLATFORM_NO_IDENTITY;
        platform.status.last_error = XIAOTAI_OK;
        platform_event_publish(PLATFORM_EVENT_BINDING, 0);
        platform_unlock();
        return XIAOTAI_WAIT_FOREVER;
    }
    platform.status.phase = clock ? XIAOTAI_PLATFORM_CLOCK : discovery ? XIAOTAI_PLATFORM_DISCOVERY : XIAOTAI_PLATFORM_LOGIN;
    s->step_running = true;
    uint32_t slice_ms = 0;
    if (clock && !clock_valid) {
        if (!s->clock_deadline) {
            const uint32_t budget = xiaotai_clock_sync_budget_ms();
            s->clock_deadline = now + (int64_t)(budget ? budget : 1000U) * 1000;
            ++platform.stats.clock_attempts;
        }
        slice_ms = platform_remaining_ms(s->clock_deadline);
        if (slice_ms > 1000) slice_ms = 1000;
    }
    if (discovery && !clock) ++platform.stats.discoveries;
    uint8_t mac[sizeof(s->mac)];
    memcpy(mac, s->mac, sizeof(mac));
    platform_unlock();

    xiaotai_err_t err = XIAOTAI_OK;
    if (clock) err = xiaotai_clock_synced() ? XIAOTAI_OK : slice_ms ? xiaotai_clock_sync(slice_ms) : XIAOTAI_ERR_TIMEOUT;
    else if (discovery) err = platform_discover(revision);
    else login(revision, generation, mac);

    platform_lock();
    s->step_running = false;
    if (platform_session_current(revision)) {
        if (clock) {
            if (err == XIAOTAI_OK) {
                s->clock_deadline = s->retry_at = 0;
                const bool first = !platform.status.clock_synced;
                platform.status.clock_synced = true;
                platform.status.last_error = XIAOTAI_OK;
                if (first) platform_event_publish(PLATFORM_EVENT_CLOCK, 0);
            } else if (err != XIAOTAI_ERR_TIMEOUT || !platform_remaining_ms(s->clock_deadline)) {
                s->clock_deadline = 0;
                failed(err);
            }
        } else if (discovery) {
            if (err == XIAOTAI_OK) platform.status.last_error = XIAOTAI_OK;
            else failed(err);
        }
    }
    platform_unlock();
    return 0;
}

void platform_session_shutdown(void)
{
    platform_session_t *s = &platform.session;
    atomic_store(&s->receive_generation, 0);
    if (s->link_live || s->destroy_pending) s->shutdown_failed = s->link.destroy(s->link.ctx) != XIAOTAI_OK;
}
