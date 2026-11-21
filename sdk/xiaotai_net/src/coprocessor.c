// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/framework.h"

typedef struct {
    const uint8_t *image;
    size_t size;
    xiaotai_net_done_fn_t done;
    void *ctx;
} update_job_t;
_Static_assert(sizeof(update_job_t) <= NET_JOB_DATA_MAX, "update fits the worker queue");

static _Atomic(const xiaotai_network_t *) s_part;
static atomic_bool s_bound;
/* Cache and the reply/restart handshake are shared with callers. No task
 * except the network worker executes a part hook or a restart. */
static xiaotai_coprocessor_info_t *s_cache;
static bool s_valid, s_wrote_firmware, s_wait_reply, s_release_restart;
static int64_t s_restart_due_us;

static bool complete(const xiaotai_coprocessor_info_t *info)
{
    return info->project[0] && memchr(info->project, 0, sizeof(info->project)) &&
           info->version[0] && memchr(info->version, 0, sizeof(info->version)) &&
           info->platform[0] && memchr(info->platform, 0, sizeof(info->platform)) &&
           info->elf_sha256[0] && memchr(info->elf_sha256, 0, sizeof(info->elf_sha256)) &&
           memchr(info->protocol, 0, sizeof(info->protocol));
}

void net_coprocessor_publish_prepare_identity(const xiaotai_coprocessor_info_t *info, void *ctx)
{
    (void)ctx;
    const bool valid = info && complete(info);
    xiaotai_critical_enter(&net_gate_lock);
    s_valid = valid && s_cache && !s_wrote_firmware;
    if (s_valid) *s_cache = *info;
    xiaotai_critical_exit(&net_gate_lock);
}

void net_coprocessor_cache_prepare(void)
{
    const xiaotai_network_t *part = net_state->part;
    if (!s_cache || !part || !part->coprocessor_info) return;
    xiaotai_coprocessor_info_t info = {0};
    const bool valid = part->coprocessor_info(part->self, false, &info) == XIAOTAI_OK;
    net_coprocessor_publish_prepare_identity(valid ? &info : NULL, NULL);
}

void net_coprocessor_invalidate(void)
{
    xiaotai_critical_enter(&net_gate_lock);
    s_wrote_firmware = true;
    s_valid = false;
    xiaotai_critical_exit(&net_gate_lock);
}

static xiaotai_err_t live_info(net_call_t *call)
{
    const xiaotai_network_t *part = net_state->part;
    xiaotai_coprocessor_info_t info = {0};
    xiaotai_err_t err = part->coprocessor_info(part->self, true, &info);
    if (err == XIAOTAI_OK && !complete(&info)) err = XIAOTAI_ERR_NOT_FOUND;
    call->size = 0;
    if (err == XIAOTAI_OK) {
        call->size = sizeof(info);
        memcpy(call->data, &info, call->size);
    }
    return err;
}

xiaotai_err_t xiaotai_net_coprocessor_info(bool live, xiaotai_coprocessor_info_t *info)
{
    if (!info) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_bound)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_network_t *part = atomic_load(&s_part);
    if (!part || !part->coprocessor_info) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_critical_enter(&net_gate_lock);
    const bool available = live ? !s_wrote_firmware : s_valid;
    if (available && !live) *info = *s_cache;
    xiaotai_critical_exit(&net_gate_lock);
    if (!available) return XIAOTAI_ERR_NOT_FOUND;
    if (!live) return XIAOTAI_OK;
    if (xiaotai_on_owner() || net_on_worker()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t result = net_gate_coprocessor_begin();
    if (result == XIAOTAI_OK) {
        /* Checked again after admission: a failed transfer may have ended
         * its lease after invalidating the identity in the meantime. */
        xiaotai_critical_enter(&net_gate_lock);
        const bool invalidated = s_wrote_firmware;
        xiaotai_critical_exit(&net_gate_lock);
        result = invalidated ? XIAOTAI_ERR_NOT_FOUND :
            net_call(live_info, 0, NULL, 0, info, sizeof(*info), NULL, XIAOTAI_WAIT_FOREVER);
        net_gate_coprocessor_end();
    }
    return result;
}

typedef struct {
    xiaotai_network_stage_t stage;
    uint8_t percent;
} update_progress_t;

static void update_progress(xiaotai_network_stage_t stage, uint8_t percent, void *ctx)
{
    update_progress_t *progress = ctx;
    *progress = (update_progress_t){stage, percent};
    if (stage == XIAOTAI_NETWORK_WRITING) net_coprocessor_invalidate();
    if (stage <= XIAOTAI_NETWORK_READY) net_report_prepare(stage, percent, XIAOTAI_OK);
}

static void promise_restart(bool wait_reply)
{
    xiaotai_critical_enter(&net_gate_lock);
    net_gate.restart_pending = true;
    net_gate.coprocessor_op = false;
    net_gate.coprocessor_write = false;
    s_wait_reply = wait_reply;
    s_release_restart = false;
    s_restart_due_us = 0;
    xiaotai_critical_exit(&net_gate_lock);
}

static void update_job(const net_message_t *message)
{
    update_job_t job;
    memcpy(&job, message->u.job.data, sizeof(job));
    const xiaotai_network_t *part = net_state->part;
    if (!net_state->driver_running || !net_state->link.radio_ready) {
        net_gate_coprocessor_end();
        job.done(XIAOTAI_ERR_INVALID_STATE, job.ctx);
        return;
    }
    update_progress_t progress = {.stage = XIAOTAI_NETWORK_CHECKING};
    net_report_prepare(progress.stage, 0, XIAOTAI_OK);
    const xiaotai_err_t result = part->coprocessor_update(part->self, job.image, job.size, update_progress, &progress);
    if (result == XIAOTAI_OK) {
        promise_restart(false);
        job.done(result, job.ctx);
        net_restart_now();
    } else {
        /* A begin failure may require a power cycle before WRITING was reported. */
        const bool power_cycle = progress.stage == XIAOTAI_NETWORK_POWER_CYCLE;
        if (power_cycle) net_coprocessor_invalidate();
        net_report_prepare(progress.stage, progress.percent, result);
        if (power_cycle) net_link_coprocessor_power_cycle();
        net_gate_coprocessor_end();
        job.done(result, job.ctx);
    }
}

xiaotai_err_t xiaotai_net_coprocessor_update(const uint8_t *image, size_t size,
                                            xiaotai_net_done_fn_t done, void *ctx)
{
    if (!image || !size || !done) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_bound)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_network_t *part = atomic_load(&s_part);
    if (!part || !part->coprocessor_update) return XIAOTAI_ERR_NOT_SUPPORTED;
    const xiaotai_err_t result = net_gate_coprocessor_write_begin();
    if (result != XIAOTAI_OK) return result;
    return net_coprocessor_update_reserved(image, size, done, ctx);
}

xiaotai_err_t net_coprocessor_update_reserved(const uint8_t *image, size_t size,
                                             xiaotai_net_done_fn_t done, void *ctx)
{
    const update_job_t job = {image, size, done, ctx};
    const xiaotai_err_t result = net_post(update_job, 0, &job, sizeof(job));
    if (result != XIAOTAI_OK) net_gate_coprocessor_end();
    return result;
}

static xiaotai_err_t run_command(net_call_t *call)
{
    char command[NET_CALL_DATA_MAX];
    memcpy(command, call->data, call->size);
    memset(call->data, 0, sizeof(call->data));
    if (call->arg && (!net_state->driver_running || !net_state->link.radio_ready)) {
        call->size = 0;
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const xiaotai_network_t *part = net_state->part;
    const xiaotai_err_t result = part->coprocessor_command(part->self, command, (char *)call->data, sizeof(call->data));
    call->data[sizeof(call->data) - 1] = 0;
    call->size = strlen((const char *)call->data) + 1;
    if (result == XIAOTAI_OK && call->arg) promise_restart(true);
    return result;
}

static void wake(const net_message_t *message) { (void)message; }
static void restart_due(void) { net_coprocessor_work(); }

xiaotai_err_t xiaotai_net_coprocessor_command(const char *command, bool restart, char *reply, size_t capacity)
{
    if (!command || !*command || (capacity && !reply)) return XIAOTAI_ERR_INVALID_ARG;
    const size_t length = strnlen(command, NET_CALL_DATA_MAX);
    if (length == NET_CALL_DATA_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (reply && capacity) *reply = '\0';
    if (!atomic_load(&s_bound) || xiaotai_on_owner() || net_on_worker()) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_network_t *part = atomic_load(&s_part);
    if (!part || !part->coprocessor_command) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_err_t result;
    if (restart) result = net_gate_coprocessor_write_begin();
    else if (strcmp(command, "INFO") == 0) result = net_gate_coprocessor_info_begin();
    else result = net_gate_coprocessor_begin();
    if (result != XIAOTAI_OK) return result;
    result = net_call(run_command, restart, command, length + 1, reply, capacity, NULL, XIAOTAI_WAIT_FOREVER);
    if (capacity) reply[capacity - 1] = '\0';
    if (result != XIAOTAI_OK || !restart) {
        net_gate_coprocessor_end();
    } else {
        /* Count the grace period from handback to the caller, not from the
         * hardware's completion (which may precede waking this task). */
        xiaotai_critical_enter(&net_gate_lock);
        const int64_t now = xiaotai_time_us();
        const int64_t wait = (int64_t)XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS * 1000;
        s_restart_due_us = now > INT64_MAX - wait ? INT64_MAX : now + wait;
        xiaotai_critical_exit(&net_gate_lock);
        (void)net_post(wake, 0, NULL, 0);
    }
    return result;
}

void xiaotai_net_coprocessor_restart_now(void)
{
    if (!atomic_load(&s_bound)) return;
    xiaotai_critical_enter(&net_gate_lock);
    const bool released = s_wait_reply && s_restart_due_us;
    if (released) s_release_restart = true;
    xiaotai_critical_exit(&net_gate_lock);
    if (released) (void)net_post(wake, 0, NULL, 0);
}

void net_coprocessor_work(void)
{
    xiaotai_critical_enter(&net_gate_lock);
    const int64_t due = s_wait_reply ? s_restart_due_us : 0;
    const bool restart = due && (s_release_restart || xiaotai_time_us() >= due);
    if (restart) s_wait_reply = false;
    xiaotai_critical_exit(&net_gate_lock);
    if (restart) {
        net_deadline_clear(NET_DEADLINE_RESTART);
        net_restart_now();
    } else if (due) {
        net_deadline_set(NET_DEADLINE_RESTART, due, restart_due);
    }
}

static xiaotai_err_t init(void)
{
    const xiaotai_network_t *part = net_part_in_use(NULL);
    if (part && part->coprocessor_info) {
        s_cache = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s_cache));
        if (!s_cache) return XIAOTAI_ERR_NO_MEM;
    }
    return net_companion_at_register(part);
}

static void bind(void)
{
    atomic_store(&s_part, net_state->part);
    atomic_store(&s_bound, true);
}

static void reset(void)
{
    atomic_store(&s_bound, false);
    net_companion_at_reset();
    xiaotai_critical_enter(&net_gate_lock);
    xiaotai_coprocessor_info_t *cache = s_cache;
    s_cache = NULL;
    s_valid = s_wrote_firmware = s_wait_reply = s_release_restart = false;
    s_restart_due_us = 0;
    xiaotai_critical_exit(&net_gate_lock);
    xiaotai_mem_free(cache);
    atomic_store(&s_part, NULL);
}

const net_unit_t net_coprocessor_unit = {.name = "coprocessor", .init = init, .bind = bind, .reset = reset};
