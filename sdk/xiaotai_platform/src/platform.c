// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"
#include "guest.h"
#include "signal_pool.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/framework.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/log.h"

platform_t platform;
static _Atomic uint32_t s_exchanges;
static const char *const defaults[] = {"ntp.aliyun.com", "pool.ntp.org"};

void platform_lock(void) { (void)xiaotai_mutex_lock(&platform.lock, XIAOTAI_WAIT_FOREVER); }
void platform_unlock(void) { xiaotai_mutex_unlock(&platform.lock); }
void platform_wake(void) { if (atomic_load(&platform.running)) xiaotai_signal_give(&platform.wake); }

static bool model_valid(const char *name)
{
    if (!name || !*name || strlen(name) > 63U) return false;
    for (; *name; ++name) {
        if ((unsigned char)*name < 0x20 || (unsigned char)*name > 0x7e || *name == '"' || *name == '\\') return false;
    }
    return true;
}

xiaotai_err_t xiaotai_platform_configure(const xiaotai_platform_config_t *config)
{
    if (atomic_load(&platform.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    if (!config) {
        platform.config = (xiaotai_platform_config_t){0};
        return XIAOTAI_OK;
    }
    char url[XIAOTAI_PLATFORM_URL_MAX];
    if ((config->discovery_url && platform_url_http(config->discovery_url, false, url, sizeof(url)) != XIAOTAI_OK) ||
        ((config->chip_model || config->board_model) &&
         (!model_valid(config->chip_model) || !model_valid(config->board_model))) ||
        config->time_server_count > XIAOTAI_CLOCK_SERVERS_MAX ||
        (config->time_server_count && !config->time_servers) ||
        (config->reserve_unbind_signal_slot && config->signal_slots == 1U) ||
        (unsigned)config->auth_failure > XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN) return XIAOTAI_ERR_INVALID_ARG;
    platform.config = *config;
    return XIAOTAI_OK;
}

static void defaults_apply(void)
{
    xiaotai_platform_config_t *c = &platform.config;
    if (!c->retry_ms) c->retry_ms = 5000;
    if (!c->profile_attempts) c->profile_attempts = 3;
    if (!c->keepalive_s) c->keepalive_s = 60;
    if (!c->reconnect_ms) c->reconnect_ms = 3000;
    if (!c->subscribe_timeout_s) c->subscribe_timeout_s = 12;
    if (!c->heartbeat_s) c->heartbeat_s = 30;
    if (!c->request_slots) c->request_slots = platform.external ? 4 : 2;
    if (!c->signal_slots) c->signal_slots = platform.external ? 8 : 3;
    if (!c->request_body_bytes) c->request_body_bytes = platform.external ? 2048 : 768;
    if (!c->response_bytes) c->response_bytes = 8192;
    if (!c->signal_bytes) c->signal_bytes = 8192;
    if (!c->worker_stack_bytes) c->worker_stack_bytes = platform.external ? 13312 : 8192;
}

static bool registry_open(void) { return atomic_load(&platform.initialized) && !atomic_load(&platform.started); }

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *need)
{
    if (!need) return XIAOTAI_ERR_INVALID_ARG;
    if (!registry_open()) return XIAOTAI_ERR_INVALID_STATE;
    if (need->request_body_bytes > platform.config.request_body_bytes ||
        need->response_bytes > platform.config.response_bytes || need->signal_bytes > platform.config.signal_bytes) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
#define TAKE_MAX(field) if (need->field > platform.need.field) platform.need.field = need->field
    TAKE_MAX(request_body_bytes);
    TAKE_MAX(response_bytes);
    TAKE_MAX(signal_bytes);
    TAKE_MAX(concurrent_requests);
#undef TAKE_MAX
    return XIAOTAI_OK;
}

bool platform_service_name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < XIAOTAI_PLATFORM_SERVICE_NAME_MAX; ++i) {
        const char c = name[i];
        if (!c) return true;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return false;
}

xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    if (!platform_service_name_valid(service)) return XIAOTAI_ERR_INVALID_ARG;
    if (!registry_open()) return XIAOTAI_ERR_INVALID_STATE;
    if (platform.provisioning || xiaotai_storage_boot_error() != XIAOTAI_OK) return XIAOTAI_OK;
    for (size_t i = 0; i < platform.required_count; ++i) {
        if (!strcmp(service, platform.required[i])) return XIAOTAI_OK;
    }
    if (platform.required_count == sizeof(platform.required) / sizeof(platform.required[0])) return XIAOTAI_ERR_NO_MEM;
    strcpy(platform.required[platform.required_count++], service);
    return XIAOTAI_OK;
}

xiaotai_err_t platform_service_find(const char *name, const char **base)
{
    if (!platform.status.discovered) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    for (size_t i = 0; i < platform.service_count; ++i) {
        if (strcmp(name, platform.services[i].name)) continue;
        *base = platform.services[i].base;
        return *base ? XIAOTAI_OK : XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t services_replace(const platform_service_t *services, size_t count, const uint32_t *revision)
{
    if (!platform.objects || !atomic_load(&platform.started)) return XIAOTAI_ERR_INVALID_STATE;
    if ((!services && count) || count > 64U) return XIAOTAI_ERR_INVALID_ARG;
    size_t size = count * sizeof(*services);
    for (size_t i = 0; i < count; ++i) {
        if (!platform_service_name_valid(services[i].name)) return XIAOTAI_ERR_INVALID_ARG;
        size += strlen(services[i].name) + 1U;
        if (services[i].base) {
            const size_t length = strnlen(services[i].base, XIAOTAI_PLATFORM_URL_MAX);
            if (!length || length == XIAOTAI_PLATFORM_URL_MAX) return XIAOTAI_ERR_INVALID_ARG;
            size += length + 1U;
        }
    }
    platform_service_t *table = count ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size) : NULL;
    if (count && !table) return XIAOTAI_ERR_NO_MEM;
    if (table) {
        char *at = (char *)&table[count];
        for (size_t i = 0; i < count; ++i) {
            table[i].name = at;
            strcpy(at, services[i].name);
            at += strlen(at) + 1U;
            table[i].base = NULL;
            if (services[i].base) {
                table[i].base = at;
                strcpy(at, services[i].base);  /* P-3 admits and normalizes before publishing */
                at += strlen(at) + 1U;
            }
        }
    }
    platform_lock();
    if (revision && !platform_session_current(*revision)) {
        platform_unlock();
        xiaotai_mem_free(table);
        return XIAOTAI_ERR_PLATFORM_STALE;
    }
    platform_service_t *old = platform.services;
    platform.services = table;
    platform.service_count = count;
    platform.status.discovered = count != 0;
    if (revision && count) platform_event_publish(PLATFORM_EVENT_DISCOVERED, 0);
    platform_unlock();
    xiaotai_mem_free(old);
    return XIAOTAI_OK;
}

xiaotai_err_t platform_services_replace(const platform_service_t *services, size_t count)
{
    return services_replace(services, count, NULL);
}

xiaotai_err_t platform_services_discovered(const platform_service_t *services, size_t count, uint32_t revision)
{
    return services_replace(services, count, &revision);
}

xiaotai_err_t xiaotai_platform_endpoint(const char *service, char *out, size_t capacity)
{
    if (!out || !capacity || !platform_service_name_valid(service)) return XIAOTAI_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!platform.objects) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    platform_lock();
    const char *base = NULL;
    xiaotai_err_t err = platform_service_find(service, &base);
    if (err == XIAOTAI_OK) {
        const size_t length = strlen(base) + 1U;
        if (length <= capacity) memcpy(out, base, length);
        else err = XIAOTAI_ERR_INVALID_SIZE;
    }
    platform_unlock();
    return err;
}

bool platform_slot_stale(const platform_slot_t *slot)
{
    if (slot->profile_id && !platform_profile_current(slot->profile_id)) return true;
    if (slot->generation != xiaotai_identity_generation() || slot->network_revision != platform.network_revision) return true;
    if (slot->auth != XIAOTAI_PLATFORM_AUTH_SESSION) return false;
    return slot->session_revision != platform.session_revision || !platform.status.authorized ||
           (platform.config.requests_need_signaling &&
            (!platform.status.signaling || (platform.status.held && !slot->profile_id)));
}

void platform_session_publish(const xiaotai_platform_status_t *status, const char *token)
{
    if (!status || !platform.objects) return;
    const size_t bytes = token ? strnlen(token, XIAOTAI_PLATFORM_BEARER_MAX) : 0;
    platform_lock();
    const uint32_t generation = xiaotai_identity_generation();
    const bool authorized = status->authorized && bytes && bytes < XIAOTAI_PLATFORM_BEARER_MAX;
    if (generation != platform.status.generation || (platform.status.authorized && !authorized) ||
        (platform.status.authorized && authorized && strcmp(token, platform.token)) ||
        (platform.config.requests_need_signaling && platform.status.signaling && !status->signaling)) {
        ++platform.session_revision;
        platform.connection_close = true;
    }
    const uint16_t exchanges = platform.status.exchanges;
    const bool discovered = platform.status.discovered;
    platform.status = *status;
    platform.status.exchanges = exchanges;
    platform.status.discovered = discovered;
    platform.status.generation = generation;
    platform.status.gated = platform.gate != 0;
    platform.status.authorized = authorized;
    xiaotai_identity_wipe(platform.token, XIAOTAI_PLATFORM_BEARER_MAX);
    if (authorized) memcpy(platform.token, token, bytes);
    platform_unlock();
    platform_wake();
}

void platform_reply_hooks(const platform_reply_hooks_t *hooks)
{
    if (registry_open()) platform.reply_hooks = hooks ? *hooks : (platform_reply_hooks_t){0};
}

void xiaotai_platform_set_network(bool up, const uint8_t *mac)
{
    platform_session_network(up, mac, false, 0);
}

static void on_network(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const bool up = !strcmp(event->name, XIAOTAI_EVENT_NET_UP);
    const xiaotai_net_status_t *net = event->size == sizeof(*net) ? event->data : NULL;
    platform_session_network(up, up && net && net->mac_valid ? net->mac : NULL, true,
        up && net ? net->generation : 0);
}

void xiaotai_platform_status(xiaotai_platform_status_t *status)
{
    if (!status) return;
    if (!platform.objects) {
        *status = (xiaotai_platform_status_t){.gated = atomic_load(&platform.dormant_gate) != 0,
                                             .last_error = xiaotai_storage_boot_error()};
        return;
    }
    platform_lock();
    *status = platform.status;
    status->gated |= atomic_load(&platform.dormant_gate) != 0;
    platform_unlock();
}

void xiaotai_platform_stats(xiaotai_platform_stats_t *stats)
{
    if (!stats) return;
    if (!platform.objects) { *stats = (xiaotai_platform_stats_t){0}; return; }
    platform_lock();
    *stats = platform.stats;
    if (!atomic_load(&platform.running)) stats->worker_stack_free = 0;
    platform_unlock();
}

bool xiaotai_platform_exchanges_busy(void) { return atomic_load(&s_exchanges) != 0; }

void platform_count_publish(void)
{
    const uint16_t old = platform.status.exchanges;
    const uint16_t next = (uint16_t)(platform.active + (platform.profile_pending && !platform.profile.in_slot ? 1U : 0U));
    platform.status.exchanges = next;
    atomic_store(&s_exchanges, next);
    if (old && !next) platform.idle_pending = true;
    if (next) platform.idle_pending = false;
}

void platform_profile_exchange(bool pending)
{
    if (!platform.objects) return;
    platform_lock();
    platform.profile_pending = pending;
    platform_count_publish();
    platform_unlock();
    platform_wake();
}

void platform_idle_flush(void)
{
    platform_lock();
    if (platform.idle_pending && !platform.status.exchanges) {
        platform.idle_pending = false;
        platform_event_publish(PLATFORM_EVENT_IDLE, 0);
    }
    platform_unlock();
}

bool xiaotai_platform_background_deferred(void)
{
    return platform.config.defer_background_while_active && platform_active_now();
}

bool platform_active_now(void)
{
    if (xiaotai_on_owner()) return xiaotai_activity_current(NULL) != 0;
    return xiaotai_activity_active();
}

static void on_activity(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    platform_wake();
}

xiaotai_err_t xiaotai_platform_exchange_preflight_register(xiaotai_platform_exchange_preflight_fn_t fn, void *ctx)
{
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!registry_open() || platform.preflight) return XIAOTAI_ERR_INVALID_STATE;
    if (!platform.provisioning) {
        platform.preflight = fn;
        platform.preflight_ctx = ctx;
    }
    return XIAOTAI_OK;
}

void xiaotai_platform_exchange_preflight_resume(void)
{
    if (!platform.objects || !xiaotai_on_owner()) return;
    platform_lock();
    ++platform.resume_revision;
    platform_unlock();
    platform_wake();
}

xiaotai_err_t xiaotai_engine_platform_init(void)
{
    if (atomic_load(&platform.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    platform.external = xiaotai_mem_has_external();
    platform.provisioning = xiaotai_net_provisioning_boot();
    defaults_apply();
    const xiaotai_err_t storage = xiaotai_storage_boot_error();
    const xiaotai_err_t boot = storage == XIAOTAI_OK ? platform_environment_boot(platform.config.discovery_url) : storage;
    char discovery[XIAOTAI_PLATFORM_URL_MAX];
    platform.session.boot_failed = boot != XIAOTAI_OK;
    platform.session.enabled = !platform.provisioning && boot == XIAOTAI_OK &&
        platform_environment_discovery_url(discovery, sizeof(discovery)) == XIAOTAI_OK;
    platform.status.generation = xiaotai_identity_generation();
    if (platform.session.boot_failed) platform.status.last_error = boot;
    platform.reply_hooks.auth_failed = platform_session_auth_failed;
    platform.reply_hooks.binding_lost = platform_binding_request_lost;
    const xiaotai_platform_config_t *c = &platform.config;
    const xiaotai_clock_config_t clock = {
        .servers = c->time_server_count ? c->time_servers : defaults,
        .server_count = c->time_server_count ? c->time_server_count : sizeof(defaults) / sizeof(defaults[0]),
        .timezone = c->timezone ? c->timezone : "CST-8", .keep_running = !c->clock_release_after_sync,
        .probe = c->clock_probe, .trust_kept_time = c->clock_trust_kept_time,
    };
    xiaotai_err_t err = xiaotai_clock_init(&clock);
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_INVALID_STATE) return err;
    if (!platform.provisioning && storage == XIAOTAI_OK) {
        err = xiaotai_mutex_init(&platform.lock);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_signal_init(&platform.wake);
        if (err != XIAOTAI_OK) { xiaotai_mutex_deinit(&platform.lock); return err; }
        err = xiaotai_signal_init(&platform.exited);
        if (err != XIAOTAI_OK) {
            xiaotai_signal_deinit(&platform.wake);
            xiaotai_mutex_deinit(&platform.lock);
            return err;
        }
        platform.objects = true;
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_UP, on_network, NULL);
        if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_DOWN, on_network, NULL);
        if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, on_activity, NULL);
#if XIAOTAI_PLATFORM_RECONNECT_COMMAND
        if (err == XIAOTAI_OK) err = xiaotai_platform_at_dev_register();
#endif
        if (err != XIAOTAI_OK) return err;
    }
    err = platform_diagnostics_register();
    if (err != XIAOTAI_OK) return err;
    atomic_store(&platform.initialized, true);
    return platform.session.enabled ? platform_signals_binding_register(platform_binding_signal, NULL) : XIAOTAI_OK;
}

bool xiaotai_platform_environment_boot_failed(void)
{
    return atomic_load(&platform.initialized) && platform.session.boot_failed;
}

bool xiaotai_platform_start_failed(void)
{
    return atomic_load(&platform.start_failed);
}

static size_t slot_pool_bytes(void)
{
    const size_t slots = platform.slot_count * sizeof(*platform.slots);
    if (!platform.external) return slots;
    return slots + (platform.slot_count - 1U) * platform.request_limit + platform.profile_limit;
}

static void pools_free(void)
{
    if (platform.slots) {
        if (!platform.external) {
            for (size_t i = 0; i < platform.slot_count; ++i)
                platform_slot_body_free(&platform.slots[i]);
        }
        xiaotai_identity_wipe(platform.slots, slot_pool_bytes());
        xiaotai_mem_free(platform.slots);
        platform.slots = NULL;
    }
    xiaotai_mem_free(platform.pending);
    platform.pending = NULL;
    if (platform.token) {
        xiaotai_identity_wipe(platform.token, XIAOTAI_PLATFORM_BEARER_MAX);
        xiaotai_mem_free(platform.token);
        platform.token = NULL;
    }
    if (platform.bearer) {
        xiaotai_identity_wipe(platform.bearer, XIAOTAI_PLATFORM_BEARER_MAX);
        xiaotai_mem_free(platform.bearer);
        platform.bearer = NULL;
    }
    if (platform.response) {
        xiaotai_identity_wipe(platform.response, platform.response_limit + 1U);
        xiaotai_mem_free(platform.response);
        platform.response = NULL;
    }
}

/* Called with the platform lock, before a worker exists. */
static void fail_start(xiaotai_err_t error)
{
    platform.status.last_error = error;
    pools_free();
    atomic_store(&platform.start_failed, true);
    platform_unlock();
    platform_signals_reset();
}

void xiaotai_engine_platform_start(void)
{
    if (!atomic_load(&platform.initialized) || atomic_exchange(&platform.started, true) || platform.provisioning ||
        xiaotai_storage_boot_error() != XIAOTAI_OK) return;
    platform_lock();
    const xiaotai_err_t signaling = platform_signaling_default_link();
    if (signaling != XIAOTAI_OK) {
        fail_start(signaling);
        return;
    }
    platform.slot_count = platform.need.concurrent_requests ? platform.need.concurrent_requests : 1U;
    if (platform.slot_count > platform.config.request_slots) {
        platform.slot_count = platform.config.request_slots;
        ++platform.stats.slot_clamps;
    }
    platform.request_limit = platform.need.request_body_bytes ? platform.need.request_body_bytes : platform.config.request_body_bytes;
    platform.profile_limit = platform_profile_body_limit();
    platform.response_limit = platform.need.response_bytes ? platform.need.response_bytes : platform.config.response_bytes;
    platform.slots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, slot_pool_bytes());
    platform.pending = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, platform.slot_count);
    platform.token = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, XIAOTAI_PLATFORM_BEARER_MAX);
    if (platform.external) platform.response = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, platform.response_limit + 1U);
    if (!platform.slots || !platform.pending || !platform.token || (platform.external && !platform.response)) goto no_mem;
    if (platform.external) {
        char *body = (char *)&platform.slots[platform.slot_count];
        for (size_t i = 0; i < platform.slot_count; ++i) platform.slots[i].body = body + platform.request_limit * i;
    }
    unsigned priority = platform.config.worker_priority;
    const unsigned owner = XIAOTAI_FRAMEWORK_OWNER_PRIORITY;
    if (!priority) priority = owner > 1 ? owner - 1 : 1;
    if (priority > owner) {
        XIAOTAI_LOGW("platform", "worker priority %u lowered to owner priority %u", priority, owner);
        priority = owner;
    }
    const xiaotai_task_config_t task = {
        .name = "xiaotai_platform", .entry = platform_worker, .stack_bytes = platform.config.worker_stack_bytes,
        .stack = XIAOTAI_MEM_BULK, .priority = priority, .core = XIAOTAI_CORE_ANY,
    };
    const xiaotai_err_t err = xiaotai_task_create(&task, &platform.task);
    if (err == XIAOTAI_OK) {
        atomic_store(&platform.running, true);
        platform_unlock();
        return;
    }
    fail_start(err);
    return;
no_mem:
    fail_start(XIAOTAI_ERR_NO_MEM);
}

void xiaotai_platform_reset(void)
{
    if (platform.objects) {
        platform_lock();
        atomic_store(&platform.stopped, true);
        if (platform.client && platform.io) xiaotai_http_cancel(platform.client);
        if (platform.session.http) xiaotai_http_cancel(platform.session.http);
        platform_unlock();
        if (atomic_load(&platform.running)) {
            platform_wake();
            if (!xiaotai_signal_take(&platform.exited, 2000)) return;
            atomic_store(&platform.running, false);
        }
        if (platform.session.shutdown_failed) return;
        platform_profile_reset();
        platform_guest_reset();
        platform_signals_reset();
        pools_free();
        xiaotai_mem_free(platform.services);
        xiaotai_signal_deinit(&platform.exited);
        xiaotai_signal_deinit(&platform.wake);
        xiaotai_mutex_deinit(&platform.lock);
    }
    else {
        platform_profile_reset();
        platform_signals_reset();
    }
    memset(&platform, 0, sizeof(platform));
    atomic_store(&s_exchanges, 0);
    platform_environment_reset();
}
