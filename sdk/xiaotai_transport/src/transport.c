// SPDX-License-Identifier: MIT
/* Transport engine: product configuration, module requirements, sizing at
 * start, the public state and status, and the lifecycle. The run rules are in
 * run_control.c. */
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/log.h"
#include "xiaotai/transport_config.h"
#if XIAOTAI_TRANSPORT_MEMTRACE
#include "memtrace.h"
#endif

xt_engine_t xt;

/* ----------------------------------------------------------- configuration */

static bool recovery_known(xiaotai_transport_recovery_t value)
{
    switch (value) {
    case XIAOTAI_TRANSPORT_RECOVER_RETRY:
    case XIAOTAI_TRANSPORT_RECOVER_AT_ONCE:
    case XIAOTAI_TRANSPORT_RECOVER_ON_EVENT:
    case XIAOTAI_TRANSPORT_RECOVER_FAULT:
        return true;
    }
    return false;
}

static bool switch_known(xiaotai_transport_switch_t value)
{
    return value == XIAOTAI_TRANSPORT_STACK_DEFAULT || value == XIAOTAI_TRANSPORT_OFF ||
           value == XIAOTAI_TRANSPORT_ON;
}

xiaotai_err_t xiaotai_transport_configure(const xiaotai_transport_config_t *config)
{
    if (atomic_load(&xt.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    if (!config) {
        memset(&xt.config, 0, sizeof(xt.config));
        return XIAOTAI_OK;
    }
    const xiaotai_transport_config_t *c = config;
    const bool invalid =
        c->links > XIAOTAI_TRANSPORT_LINKS_MAX || c->poll_timeout_ms > XT_POLL_TIMEOUT_MAX_MS ||
        c->stack_log_level > XT_STACK_LOG_LEVEL_MAX ||
        (c->worker_stack_bytes != 0U && c->worker_stack_bytes < XT_WORKER_STACK_MIN) ||
        (c->vendor_idle_stack_bytes != 0U &&
         (c->vendor_idle_stack_bytes < 4096U || c->vendor_idle_stack_bytes > 8192U ||
          c->vendor_idle_stack_bytes % 16U != 0U)) ||
        c->worker_priority > XT_WORKER_PRIORITY_MAX ||
        ((c->yield_connect_free_bytes == 0U) != (c->yield_connect_block_bytes == 0U)) ||
        (c->release_timeout_ms != 0U &&
         (c->release_timeout_ms < XT_RELEASE_TIMEOUT_MIN_MS || c->release_timeout_ms > XT_RELEASE_TIMEOUT_MAX_MS)) ||
        (uint64_t)c->command_pool_bytes > (uint64_t)XIAOTAI_LINK_COMMAND_LIMIT_BYTES * XIAOTAI_TRANSPORT_LINKS_MAX ||
        (c->command_pool_mode != XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT &&
         c->command_pool_mode != XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT &&
         c->command_pool_mode != XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND) ||
        !recovery_known(c->start_failure) || !recovery_known(c->unsolicited_stop) ||
        !switch_known(c->connect_cache) || !switch_known(c->media_transport) || !switch_known(c->media_uplink_worker) ||
        (c->media_transport_before_init && c->media_transport != XIAOTAI_TRANSPORT_ON);
    if (invalid) return XIAOTAI_ERR_INVALID_ARG;
    xt.config = *c;
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------ requirements */

static uint32_t send_buffer_ceiling(void)
{
    if (xt.config.send_buffer_max_bytes) return xt.config.send_buffer_max_bytes;
    return xiaotai_mem_has_external() ? XT_SEND_BUFFER_CEILING_EXTERNAL : XT_SEND_BUFFER_CEILING_INTERNAL;
}

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *requirement)
{
    if (!requirement || requirement->links > XIAOTAI_TRANSPORT_LINKS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&xt.sized)) return XIAOTAI_ERR_INVALID_STATE;
    if (requirement->send_buffer_bytes > send_buffer_ceiling() ||
        requirement->command_pool_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    /* Modules declare from their init on the owner task: no lock needed. */
    const uint32_t share =
        requirement->command_pool_bytes ? requirement->command_pool_bytes : 2U * XIAOTAI_LINK_COMMAND_DEFAULT_BYTES;
    if (share > xt.need.command_pool_bytes) xt.need.command_pool_bytes = share;
    if (requirement->send_buffer_bytes > xt.need.send_buffer_bytes) {
        xt.need.send_buffer_bytes = requirement->send_buffer_bytes;
    }
    if (requirement->links > xt.need.links) xt.need.links = requirement->links;
    return XIAOTAI_OK;
}

/* Sizes the engine once, from the configuration and the requirements. */
static void size_engine(void)
{
    const xiaotai_transport_config_t *c = &xt.config;
    xt.external = xiaotai_mem_has_external();
    uint8_t links = c->links ? c->links : 1U;
    if (xt.need.links > links) links = xt.need.links;
    xt.links_max = links;
    xt.event_slots = c->event_slots ? c->event_slots
                                    : (uint8_t)(xt.external ? XT_EVENT_SLOTS_EXTERNAL : XT_EVENT_SLOTS_INTERNAL);
    xt.send_buffer_ceiling = send_buffer_ceiling();
    xt.send_buffer_bytes = xt.need.send_buffer_bytes
                               ? xt.need.send_buffer_bytes
                               : (xt.external ? XT_SEND_BUFFER_DEFAULT_EXTERNAL : XT_SEND_BUFFER_DEFAULT_INTERNAL);
    if (xt.send_buffer_bytes > xt.send_buffer_ceiling) xt.send_buffer_bytes = xt.send_buffer_ceiling;
    /* No module declared a share: every link gets the default share. */
    const uint32_t share =
        xt.need.command_pool_bytes ? xt.need.command_pool_bytes : 2U * XIAOTAI_LINK_COMMAND_DEFAULT_BYTES;
    xt.command_pool_bytes = c->command_pool_bytes ? c->command_pool_bytes : share * links;
    const bool hybrid = c->command_pool_mode == XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT && !xt.external;
    xt.command_on_demand = c->command_pool_mode == XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND || hybrid;
    if (!xt.command_on_demand) xt.command_resident_bytes = xt.command_pool_bytes;
    else if (hybrid) {
        xt.command_resident_bytes = xt.command_pool_bytes < XT_COMMAND_RESIDENT_COMPACT_BYTES ?
            xt.command_pool_bytes : XT_COMMAND_RESIDENT_COMPACT_BYTES;
    }
    xt.worker_stack_bytes = c->worker_stack_bytes
                                ? c->worker_stack_bytes
                                : (xt.external ? XT_WORKER_STACK_EXTERNAL : XT_WORKER_STACK_INTERNAL);
    xt.worker_priority = c->worker_priority ? c->worker_priority : (unsigned)XIAOTAI_FRAMEWORK_OWNER_PRIORITY;
    xt.release_timeout_ms = c->release_timeout_ms ? c->release_timeout_ms : XT_RELEASE_TIMEOUT_DEFAULT_MS;
    xt.start_retry_ms = c->start_retry_ms ? c->start_retry_ms : XT_START_RETRY_DEFAULT_MS;
}

/* ------------------------------------------------------------------- state */

xiaotai_transport_state_t xiaotai_transport_state(void)
{
    return (xiaotai_transport_state_t)atomic_load(&xt.state);
}

const char *xiaotai_transport_version(void)
{
    const xiaotai_rtc_caps_t *caps = xiaotai_rtc_caps();
    return caps && caps->version ? caps->version : "";
}

void xiaotai_transport_status(xiaotai_transport_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    const xt_counters_t *n = &xt.counters;
    status->state = (xiaotai_transport_state_t)atomic_load(&xt.state);
    status->wait = (xiaotai_transport_wait_t)atomic_load(&xt.wait);
    status->holds = atomic_load(&xt.hold_count);
    status->links_max = xt.links_max;
    if (xt.slots) xt_links_count(&status->links_requested, &status->links_open, &status->links_releasing);
    if (xt.sync_ready) {
        xiaotai_critical_enter(&xt.lock);
        status->connect_ready = xt_link_admissible() == XIAOTAI_OK && xt_link_free_locked() != NULL;
        xiaotai_critical_exit(&xt.lock);
        status->connect_ready = status->connect_ready && xt_connect_memory_ready();
    }
    if (xt.sync_ready) {
        xiaotai_critical_enter(&xt.lock);
        status->start_reserve_held = xt.start_reserve != NULL;
        status->connect_reserve_held = xt.run.connect_reserve != NULL;
        status->signaling_suspended = xt.run.start_hold != 0U || xt.run.link_hold != 0U;
        xiaotai_critical_exit(&xt.lock);
    }
    status->send_buffer_bytes = xt.send_buffer_bytes;
    status->starts = atomic_load(&n->starts);
    status->start_failures = atomic_load(&n->start_failures);
    status->stops = atomic_load(&n->stops);
    status->faults = atomic_load(&n->faults);
    status->unsolicited_stops = atomic_load(&n->unsolicited_stops);
    status->release_stalls = atomic_load(&n->release_stalls);
    status->last_start_detail = atomic_load(&n->last_start_detail);
    status->last_fault_detail = atomic_load(&n->last_fault_detail);
    status->inbound_admitted = atomic_load(&n->inbound_admitted);
    status->inbound_refused = atomic_load(&n->inbound_refused);
    status->late_links_closed = atomic_load(&n->late_links_closed);
    status->early_dropped = atomic_load(&n->early_dropped);
    status->stale_calls = atomic_load(&n->stale_calls);
    status->send_call_max_us = atomic_load(&n->send_call_max_us);
    status->events_peak = atomic_load(&n->events_peak);
    status->events_lost = atomic_load(&n->events_lost);
    status->connect_memory_misses = atomic_load(&n->connect_memory_misses);
    status->signaling_yields = atomic_load(&n->signaling_yields);
    status->service_requests = atomic_load(&n->service_requests);
    status->service_failures = atomic_load(&n->service_failures);
    status->alerts = atomic_load(&n->alerts);
    xiaotai_rtc_diag_t diag;
    memset(&diag, 0, sizeof(diag));
    xiaotai_rtc_diag(&diag);
    status->stack_log_bytes = diag.log_bytes;
    status->worker_stack_free = atomic_load(&xt.worker_stack_free);
    status->command_pool_bytes = xt.command_pool_bytes;
    status->command_pool_peak = atomic_load(&n->command_pool_peak);
    status->command_largest = atomic_load(&n->command_largest);
    status->engine_bytes = atomic_load(&xt.engine_bytes);
}

/* ---------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_engine_transport_init(void)
{
    if (atomic_load(&xt.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    const bool local_only = xiaotai_storage_boot_error() != XIAOTAI_OK;
    const bool online_boot = !xiaotai_net_provisioning_boot() && !local_only;
    if (online_boot) {
        xiaotai_err_t err = xiaotai_platform_require_service(XIAOTAI_PLATFORM_SERVICE_TIRTC);
        if (err != XIAOTAI_OK) return err;
    }
    if (!xt.sync_ready) {
        xiaotai_critical_init(&xt.lock);
        if (xiaotai_mutex_init(&xt.control) != XIAOTAI_OK) return XIAOTAI_FAIL;
        if (xiaotai_signal_init(&xt.worker_exited) != XIAOTAI_OK) {
            xiaotai_mutex_deinit(&xt.control);
            return XIAOTAI_FAIL;
        }
        xt.sync_ready = true;
    }
    atomic_store(&xt.state, XIAOTAI_TRANSPORT_DOWN);
    atomic_store(&xt.wait, XIAOTAI_TRANSPORT_WAIT_IDENTITY);
    xiaotai_err_t err = xt_run_init();
    if (err == XIAOTAI_OK) err = xt_diag_init();
#if XIAOTAI_TRANSPORT_MEMTRACE
    if (err == XIAOTAI_OK) err = xiaotai_transport_memtrace_init();
#endif
    if (err != XIAOTAI_OK) return err;
    xt_audio_sink_init();
    const bool external = xiaotai_mem_has_external();
    const bool audio_on_worker = xt.config.media_uplink_worker == XIAOTAI_TRANSPORT_ON ||
        (xt.config.media_uplink_worker == XIAOTAI_TRANSPORT_STACK_DEFAULT && !external);
    const uint32_t worker_stack = xt.config.worker_stack_bytes ? xt.config.worker_stack_bytes :
        (external ? XT_WORKER_STACK_EXTERNAL : XT_WORKER_STACK_INTERNAL);
    if (!local_only && audio_on_worker && worker_stack >= (uint64_t)xt_audio_executor.sink->send_stack_bytes + 1024U) {
        err = xiaotai_media_uplink_attach(&xt_audio_executor);
        if (err != XIAOTAI_OK) return err;
        xt.audio_executor_attached = true;
    }
    if (online_boot) xt_receive_topics_init();
    atomic_store(&xt.initialized, true);
    return XIAOTAI_OK;
}

/* Frees what start took; used on a failed start and by reset. */
static void release_memory(void)
{
    xt_links_free();
    if (xt.command_slots) {
        xt_command_payloads_release();
        xiaotai_mem_free(xt.command_slots);
        xt_account(-(int64_t)((xt.event_slots + 1U) * sizeof(*xt.command_slots)));
        xt.command_slots = NULL;
    }
    if (xt.event_ring) {
        xiaotai_mem_free(xt.event_ring);
        xt_account(-(int64_t)xt.event_slots * XT_EVENT_ITEM_BYTES);
        xt.event_ring = NULL;
    }
    if (xt.command_pool) {
        const uint32_t bytes = xt_command_storage_bytes();
        volatile uint8_t *p = xt.command_pool;
        for (uint32_t i = 0; i < bytes; ++i) p[i] = 0;
        xiaotai_mem_free(xt.command_pool);
        xt_account(-(int64_t)bytes);
        xt.command_pool = NULL;
    }
}

void xiaotai_engine_transport_start(void)
{
    if (!atomic_load(&xt.initialized) || atomic_load(&xt.sized)) return;
    atomic_store(&xt.sized, true);
    if (xiaotai_storage_boot_error() != XIAOTAI_OK) return;
    if (xiaotai_net_provisioning_boot()) {
        /* The access point and the portal need this memory: nothing is
         * created, the engine stays down for the whole boot. */
        atomic_store(&xt.provisioning, true);
        atomic_store(&xt.state, XIAOTAI_TRANSPORT_DOWN);
        atomic_store(&xt.wait, XIAOTAI_TRANSPORT_WAIT_PROVISIONING);
        XIAOTAI_LOGI(XT_TAG, "provisioning boot: transport stays down");
        return;
    }
    size_engine();
    xiaotai_err_t err = xt_links_alloc();
    if (err == XIAOTAI_OK) {
        const size_t ring = (size_t)xt.event_slots * XT_EVENT_ITEM_BYTES;
        xt.event_ring = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1U, ring);
        if (xt.event_ring) {
            xt_account((int64_t)ring);
        } else {
            err = XIAOTAI_ERR_NO_MEM;
        }
    }
    if (err == XIAOTAI_OK && xt.command_pool_bytes) {
        bool ready;
        if (xt.command_on_demand) {
            const size_t bytes = (xt.event_slots + 1U) * sizeof(*xt.command_slots);
            xt.command_slots = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1U, bytes);
            ready = xt.command_slots != NULL;
            if (ready) xt_account((int64_t)bytes);
        } else ready = true;
        if (ready && xt.command_resident_bytes) {
            const size_t bytes = xt_command_storage_bytes();
            xt.command_pool = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
            if (xt.command_pool) xt_account((int64_t)bytes);
            else if (!xt.command_on_demand) ready = false;
            else {
                XIAOTAI_LOGW(XT_TAG, "command reservation unavailable; using bounded on-demand storage");
                xt.command_resident_bytes = 0;
            }
        }
        if (!ready) {
            /* Without command storage every nonempty payload is reported dropped. */
            XIAOTAI_LOGW(XT_TAG, "command pool of %u bytes not available", (unsigned)xt.command_pool_bytes);
            xt.command_pool_bytes = 0;
        }
    }
    if (err == XIAOTAI_OK) err = xt_run_start();
    if (err != XIAOTAI_OK) {
        release_memory();
        atomic_fetch_add(&xt.counters.faults, 1U);
        atomic_store(&xt.counters.last_fault_detail, err);
        xt.run.fault = true;
        atomic_store(&xt.state, XIAOTAI_TRANSPORT_FAULT);
        atomic_store(&xt.wait, XIAOTAI_TRANSPORT_WAIT_FAULT);
        XIAOTAI_LOGE(XT_TAG, "cannot start: %d", err);
    }
}

void xiaotai_transport_reset(void)
{
    if (xt_on_worker()) {
        XIAOTAI_LOGE(XT_TAG, "cannot reset from the transport worker");
        return;
    }
    if (xt.audio_executor_attached) xiaotai_media_uplink_detach(&xt_audio_executor);
    if (!xt_run_stop_worker()) return;
    xt_service_reset();
#if XIAOTAI_TRANSPORT_MEMTRACE
    xt_memtrace_stop();
#endif
    xt_diag_reset();
    release_memory();
    xt_listeners_free();
    const bool sync_ready = xt.sync_ready;
    const xiaotai_critical_t lock = xt.lock;
    const xiaotai_mutex_t control = xt.control;
    const xiaotai_signal_t exited = xt.worker_exited;
    memset(&xt, 0, sizeof(xt));
    /* The synchronization objects outlive a reset: holds may be taken before
     * the next init, and the objects have no portable re-initialization. */
    if (sync_ready) {
        xt.lock = lock;
        xt.control = control;
        xt.worker_exited = exited;
        xt.sync_ready = true;
    }
}
