// SPDX-License-Identifier: MIT
#include "identity_internal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define IDENTITY_JOBS 4U

typedef struct {
    xiaotai_identity_apply_t kind;
    xiaotai_identity_done_fn_t done;
    void *ctx;
    xiaotai_identity_t *save;
    bool active, silent, defer_erase;
} identity_job_t;

/* Serialize handovers to preserve KV queue order; neither the KV worker
 * nor the reload task takes this lock. The records lock covers only memory. */
static xiaotai_mutex_t s_handover, s_records;
static bool s_created;
static bool s_restart_erase;
static identity_job_t s_jobs[IDENTITY_JOBS];
static xiaotai_identity_settings_t s_reload_settings;

static void records_lock(void) { (void)xiaotai_mutex_lock(&s_records, XIAOTAI_WAIT_FOREVER); }
static void records_unlock(void) { xiaotai_mutex_unlock(&s_records); }

xiaotai_err_t identity_jobs_init(void)
{
    if (s_created) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_mutex_init(&s_handover);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_mutex_init(&s_records);
    if (err != XIAOTAI_OK) xiaotai_mutex_deinit(&s_handover);
    s_created = err == XIAOTAI_OK;
    return err;
}

void identity_jobs_silence(void)
{
    if (!s_created) return;
    records_lock();
    s_restart_erase = false;
    for (size_t i = 0; i < IDENTITY_JOBS; ++i) {
        if (s_jobs[i].active) s_jobs[i].silent = true;
    }
    records_unlock();
}

static identity_job_t *reserve(xiaotai_identity_apply_t kind, bool defer_erase,
                               xiaotai_identity_done_fn_t done, void *ctx)
{
    identity_job_t *free_slot = NULL;
    records_lock();
    for (size_t i = 0; !s_restart_erase && i < IDENTITY_JOBS; ++i) {
        identity_job_t *job = &s_jobs[i];
        if (!job->active) {
            if (!free_slot) free_slot = job;
        } else if (defer_erase || job->defer_erase || job->kind == XIAOTAI_IDENTITY_APPLY_RELOAD ||
                   (kind == XIAOTAI_IDENTITY_APPLY_SAVE && job->kind == XIAOTAI_IDENTITY_APPLY_SAVE)) {
            free_slot = NULL;
            break;
        }
    }
    if (free_slot) *free_slot = (identity_job_t){.kind = kind, .done = done, .ctx = ctx,
                                               .active = true, .defer_erase = defer_erase};
    records_unlock();
    return free_slot;
}

static void release(identity_job_t *job)
{
    records_lock();
    *job = (identity_job_t){0};
    records_unlock();
}

static void finish(identity_job_t *job, xiaotai_err_t result, xiaotai_identity_t *loaded,
                   xiaotai_identity_state_t state)
{
    records_lock();
    const xiaotai_identity_apply_t kind = job->kind;
    const xiaotai_identity_done_fn_t done = job->silent ? NULL : job->done;
    void *ctx = job->ctx;
    if (!job->silent) {
        if (result == XIAOTAI_OK) {
            if (job->defer_erase) s_restart_erase = true;
            else (void)xiaotai_identity_apply(kind, kind == XIAOTAI_IDENTITY_APPLY_SAVE ? job->save : loaded, state);
            identity_count_job(kind == XIAOTAI_IDENTITY_APPLY_SAVE ? IDENTITY_SAVED
                : (kind == XIAOTAI_IDENTITY_APPLY_RELOAD ? IDENTITY_LOADED : IDENTITY_ERASED));
        } else if (kind != XIAOTAI_IDENTITY_APPLY_ERASE) {
            identity_count_job(kind == XIAOTAI_IDENTITY_APPLY_SAVE ? IDENTITY_SAVE_FAILED : IDENTITY_LOAD_FAILED);
        }
    }
    records_unlock();
    if (loaded) xiaotai_identity_wipe(loaded, sizeof(*loaded));
    if (job->save) {
        xiaotai_identity_wipe(job->save, sizeof(*job->save));
        xiaotai_mem_free(job->save);
    }
    if (done) done(result, ctx);
    release(job);
}

static void committed(xiaotai_err_t result, void *ctx)
{
    finish(ctx, result, NULL, XIAOTAI_IDENTITY_ABSENT);
}

static void reload_task(void *ctx)
{
    xiaotai_identity_t loaded;
    xiaotai_identity_state_t state;
    const xiaotai_err_t err = xiaotai_identity_layout_read(&s_reload_settings, &loaded, &state);
    finish(ctx, err, &loaded, state);
    xiaotai_task_exit();
}

static xiaotai_err_t handover(xiaotai_identity_apply_t kind, const xiaotai_identity_t *identity,
                             bool defer_erase, xiaotai_identity_done_fn_t done, void *ctx)
{
    if (!xiaotai_identity_ready() || xiaotai_in_isr()) return XIAOTAI_ERR_INVALID_STATE;
    /* Bounded arbitration between callers; flash processing never holds it. */
    if (!xiaotai_mutex_lock(&s_handover, 10U)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = XIAOTAI_ERR_NO_MEM;
    identity_job_t *job = reserve(kind, defer_erase, done, ctx);
    if (job) {
        const xiaotai_identity_settings_t *settings = xiaotai_identity_settings();
        if (kind == XIAOTAI_IDENTITY_APPLY_RELOAD) {
            s_reload_settings = *settings;
            const xiaotai_task_config_t task = {
                .name = "identity_reload", .entry = reload_task, .arg = job,
                .stack_bytes = XIAOTAI_IDENTITY_RELOAD_STACK_BYTES,
                .stack = XIAOTAI_MEM_BULK, .priority = 1, .core = XIAOTAI_CORE_ANY,
            };
            xiaotai_task_t handle;
            err = xiaotai_task_create(&task, &handle) == XIAOTAI_OK ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
        } else {
            xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX];
            uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES];
            size_t count = 0;
            if (kind == XIAOTAI_IDENTITY_APPLY_SAVE) {
                if (xiaotai_identity_state() == XIAOTAI_IDENTITY_UNREADABLE) {
                    err = XIAOTAI_ERR_INVALID_STATE;
                } else {
                    job->save = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*job->save));
                    if (job->save) {
                        *job->save = *identity;
                        count = xiaotai_identity_layout_save_ops(settings, job->save, record, ops);
                    }
                }
            } else {
                count = xiaotai_identity_layout_erase_ops(settings, ops);
            }
            if (count) err = xiaotai_kv_commit_async(settings->space, ops, count, committed, job);
            xiaotai_identity_wipe(record, sizeof(record));
        }
        if (err != XIAOTAI_OK) {
            if (job->save) {
                xiaotai_identity_wipe(job->save, sizeof(*job->save));
                xiaotai_mem_free(job->save);
            }
            release(job);
        }
    }
    if (err == XIAOTAI_ERR_NO_MEM) identity_count_job(IDENTITY_REFUSED);
    xiaotai_mutex_unlock(&s_handover);
    return err;
}

xiaotai_err_t xiaotai_identity_save(const xiaotai_identity_t *identity, xiaotai_identity_done_fn_t done, void *ctx)
{
    if (!xiaotai_identity_valid(identity)) return XIAOTAI_ERR_INVALID_ARG;
    return handover(XIAOTAI_IDENTITY_APPLY_SAVE, identity, false, done, ctx);
}

xiaotai_err_t xiaotai_identity_erase(xiaotai_identity_done_fn_t done, void *ctx)
{
    return handover(XIAOTAI_IDENTITY_APPLY_ERASE, NULL, false, done, ctx);
}

xiaotai_err_t xiaotai_identity_erase_for_restart(xiaotai_identity_done_fn_t done, void *ctx)
{
    if (!done) return XIAOTAI_ERR_INVALID_ARG;
    return handover(XIAOTAI_IDENTITY_APPLY_ERASE, NULL, true, done, ctx);
}

void xiaotai_identity_publish_restart_erase(void)
{
    if (!s_created) return;
    records_lock();
    if (s_restart_erase) {
        (void)xiaotai_identity_apply(XIAOTAI_IDENTITY_APPLY_ERASE, NULL, XIAOTAI_IDENTITY_ABSENT);
        s_restart_erase = false;
    }
    records_unlock();
}

xiaotai_err_t xiaotai_identity_reload(xiaotai_identity_done_fn_t done, void *ctx)
{
    return handover(XIAOTAI_IDENTITY_APPLY_RELOAD, NULL, false, done, ctx);
}
