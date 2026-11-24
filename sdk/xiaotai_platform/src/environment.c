// SPDX-License-Identifier: MIT
/* The environment base: the stored address the discovery URL is made from.
 *
 * Boot: the engine's init reads service_env/ep_base once (a synchronous
 * storage read, allowed during init) into static storage; nothing is
 * allocated, so a provisioning boot reads it too.
 *
 * Store: two jobs of the storage port's worker, the write and then the
 * read-back, each given PLATFORM_ENVIRONMENT_STEP_MS like the synchronous
 * calls of deployed firmware. The write is queued on the caller's request;
 * its completion (storage worker) hands over to the owner task, which queues
 * the read-back, whose completion hands the result back to the owner task,
 * where done runs. An owner-task timer bounds each step: when it expires
 * before the step's completion arrived the store ends with
 * XIAOTAI_ERR_TIMEOUT (the job may still complete later and is then ignored),
 * and when a completion arrived but could not be handed over (owner queue
 * full) the timer carries it on. No platform worker is involved: a
 * provisioning boot has none and still repairs its environment.
 *
 * State shared between the owner task, the storage worker and [any] readers
 * is guarded by one mutex, held only for copies. */
#include <stdatomic.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"

#define TAG "platform"
#define BASE_BYTES (XIAOTAI_PLATFORM_BASE_MAX + 1U)

typedef enum {
    JOB_IDLE = 0,
    JOB_SUBMITTING, /* accepted, waiting for the owner task to queue the write */
    JOB_WRITING,    /* write queued */
    JOB_WRITTEN,    /* write completed; read-back not queued yet */
    JOB_READING,    /* read-back queued */
    JOB_READ,       /* read-back completed; result not delivered yet */
} job_state_t;

typedef struct {
    job_state_t state;
    uint32_t token;
    bool erase;
    char base[BASE_BYTES];
    xiaotai_platform_done_fn_t done;
    void *ctx;
    xiaotai_err_t result;
    uint32_t timer;
} job_t;

static xiaotai_mutex_t s_lock;
static bool s_lock_ready;
static atomic_bool s_booted;
static uint32_t s_sequence;
static uint32_t s_step_ms;

/* Guarded by s_lock once booted. */
static const char *s_builtin;     /* admissible product URL or NULL; static storage */
static xiaotai_err_t s_boot_error;
static char s_boot_base[BASE_BYTES]; /* base read at boot: the active environment */
static char s_saved[BASE_BYTES];      /* last confirmed stored base */
static bool s_saved_known;            /* false after a boot error until a confirmed store */
static bool s_unconfirmed;
static job_t s_job;

static void lock(void) { (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&s_lock); }

static uint32_t step_ms(void) { return s_step_ms ? s_step_ms : PLATFORM_ENVIRONMENT_STEP_MS; }

static void *token_ctx(uint32_t token) { return (void *)(uintptr_t)token; }
static uint32_t ctx_token(void *ctx) { return (uint32_t)(uintptr_t)ctx; }

/* base + "/services", or the built-in URL (written as http://) when base is "". */
static xiaotai_err_t discovery_of(const char *base, const char *builtin, char *out, size_t capacity)
{
    if (base[0] != '\0') {
        return platform_url_join(base, PLATFORM_ENVIRONMENT_SUFFIX, out, capacity);
    }
    if (!builtin) {
        out[0] = '\0';
        return XIAOTAI_ERR_NOT_FOUND;
    }
    return platform_url_http(builtin, false, out, capacity);
}

xiaotai_err_t xiaotai_platform_environment_normalize(const char *base, char *out, size_t capacity)
{
    char normalized[BASE_BYTES];
    /* Read into a separate buffer first: deployed callers may normalize in
     * place, including the rewrite from https:// to http://. */
    if (!base || strlen(base) > XIAOTAI_PLATFORM_BASE_MAX ||
        platform_url_http(base, true, normalized, sizeof(normalized)) != XIAOTAI_OK) {
        if (out && capacity > 0) out[0] = '\0';
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!out) {
        return XIAOTAI_OK;
    }
    size_t length = strlen(normalized);
    if (length >= capacity) {
        if (capacity > 0) out[0] = '\0';
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    memcpy(out, normalized, length + 1);
    return XIAOTAI_OK;
}

xiaotai_err_t platform_environment_boot(const char *builtin_url)
{
    if (!s_lock_ready) {
        if (xiaotai_mutex_init(&s_lock) != XIAOTAI_OK) {
            return XIAOTAI_ERR_NO_MEM;
        }
        s_lock_ready = true;
    }
    char check[XIAOTAI_PLATFORM_URL_MAX];
    const char *builtin = builtin_url;
    if (builtin && platform_url_http(builtin, false, check, sizeof(check)) != XIAOTAI_OK) {
        XIAOTAI_LOGW(TAG, "built-in discovery URL is not admissible; none is used");
        builtin = NULL;
    }

    char stored[BASE_BYTES] = {0};
    char base[BASE_BYTES] = {0};
    xiaotai_err_t boot_error = XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_kv_get_str_strict(PLATFORM_ENVIRONMENT_NAMESPACE, PLATFORM_ENVIRONMENT_KEY, stored,
                                                  sizeof(stored), PLATFORM_ENVIRONMENT_STEP_MS);
    if (err == XIAOTAI_OK) {
        /* A stored value is re-checked: anything that is not an admissible
         * base stops the platform instead of silently using the built-in URL. */
        if (stored[0] == '\0' || xiaotai_platform_environment_normalize(stored, base, sizeof(base)) != XIAOTAI_OK) {
            boot_error = XIAOTAI_FAIL;
            base[0] = '\0';
        }
    } else if (err != XIAOTAI_ERR_NOT_FOUND) {
        boot_error = err;
    }
    if (boot_error != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "stored environment unreadable (%d): platform stopped; store a new one to repair",
                     boot_error);
    }

    lock();
    s_builtin = builtin;
    s_boot_error = boot_error;
    memcpy(s_boot_base, base, sizeof(s_boot_base));
    memcpy(s_saved, base, sizeof(s_saved));
    s_saved_known = boot_error == XIAOTAI_OK;
    s_unconfirmed = false;
    memset(&s_job, 0, sizeof(s_job));
    unlock();
    atomic_store(&s_booted, true);
    return boot_error;
}

xiaotai_err_t platform_environment_discovery_url(char *out, size_t capacity)
{
    if (!out || capacity == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!atomic_load(&s_booted)) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    lock();
    xiaotai_err_t boot_error = s_boot_error;
    char base[BASE_BYTES];
    memcpy(base, s_boot_base, sizeof(base));
    const char *builtin = s_builtin;
    unlock();
    if (boot_error != XIAOTAI_OK) {
        return boot_error;
    }
    return discovery_of(base, builtin, out, capacity);
}

void xiaotai_platform_environment(xiaotai_platform_environment_t *environment)
{
    if (!environment) {
        return;
    }
    memset(environment, 0, sizeof(*environment));
    if (!atomic_load(&s_booted)) {
        return;
    }
    char boot_base[BASE_BYTES];
    lock();
    const char *builtin = s_builtin;
    environment->boot_error = s_boot_error;
    environment->unconfirmed = s_unconfirmed;
    bool known = s_saved_known;
    memcpy(boot_base, s_boot_base, sizeof(boot_base));
    memcpy(environment->saved, s_saved, sizeof(s_saved));
    unlock();
    if (environment->boot_error == XIAOTAI_OK) {
        (void)discovery_of(boot_base, builtin, environment->active, sizeof(environment->active));
    }
    if (known) {
        (void)discovery_of(environment->saved, builtin, environment->next, sizeof(environment->next));
    }
}

/* ------------------------------------------------------------------ store */

static void finish(uint32_t token, xiaotai_err_t result, bool handed);
static void after_write(void *ctx);
static void after_read(void *ctx);

/* [owner] Ends the job: updates the snapshot and runs done. handed: the write
 * reached storage, so a failure leaves storage unknown. */
static void finish(uint32_t token, xiaotai_err_t result, bool handed)
{
    lock();
    if (s_job.token != token || s_job.state == JOB_IDLE) {
        unlock();
        return;
    }
    xiaotai_platform_done_fn_t done = s_job.done;
    void *ctx = s_job.ctx;
    uint32_t timer = s_job.timer;
    if (result == XIAOTAI_OK) {
        memcpy(s_saved, s_job.erase ? "" : s_job.base, s_job.erase ? 1U : strlen(s_job.base) + 1U);
        s_saved_known = true;
        s_unconfirmed = false;
    } else if (handed) {
        s_unconfirmed = true;
    }
    memset(&s_job, 0, sizeof(s_job));
    unlock();
    if (timer) {
        xiaotai_timer_cancel(timer);
    }
    if (result != XIAOTAI_OK) {
        XIAOTAI_LOGW(TAG, "environment store failed (%d)%s", result, handed ? "; storage unconfirmed" : "");
    }
    if (done) {
        done(result, ctx);
    }
}

static void on_deadline(void *ctx);

/* [owner] A new deadline for the step about to start. */
static xiaotai_err_t arm(uint32_t token)
{
    uint32_t id = 0;
    xiaotai_err_t err = xiaotai_timer_start(step_ms(), on_deadline, token_ctx(token), &id);
    if (err == XIAOTAI_OK) {
        lock();
        s_job.timer = id;
        unlock();
    }
    return err;
}

static void disarm(void)
{
    lock();
    uint32_t timer = s_job.timer;
    s_job.timer = 0;
    unlock();
    if (timer) {
        xiaotai_timer_cancel(timer);
    }
}

/* [storage worker] */
static void on_written(xiaotai_err_t result, void *ctx)
{
    uint32_t token = ctx_token(ctx);
    lock();
    bool current = s_job.token == token && s_job.state == JOB_WRITING;
    if (current) {
        s_job.result = result;
        s_job.state = JOB_WRITTEN;
    }
    unlock();
    /* A full owner queue leaves the job WRITTEN: the step's deadline carries it on. */
    if (current) {
        (void)xiaotai_run(after_write, ctx);
    }
}

/* [storage worker] */
static void on_read(const xiaotai_kv_value_t *values, size_t count, void *ctx)
{
    uint32_t token = ctx_token(ctx);
    lock();
    bool current = s_job.token == token && s_job.state == JOB_READING;
    if (current) {
        xiaotai_err_t read = count == 1 ? values[0].result : XIAOTAI_FAIL;
        xiaotai_err_t result;
        if (s_job.erase) {
            /* Removed means absent; a value still there is a mismatch. */
            result = read == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_OK : read == XIAOTAI_OK ? XIAOTAI_FAIL : read;
        } else if (read != XIAOTAI_OK) {
            result = read;
        } else {
            const char *data = values[0].data;
            result = data && strcmp(data, s_job.base) == 0 ? XIAOTAI_OK : XIAOTAI_FAIL;
        }
        s_job.result = result;
        s_job.state = JOB_READ;
    }
    unlock();
    if (current) {
        (void)xiaotai_run(after_read, ctx);
    }
}

/* [owner] Queue the read-back after a successful write. */
static void start_read(uint32_t token)
{
    xiaotai_err_t err = arm(token);
    if (err != XIAOTAI_OK) {
        finish(token, err, true);
        return;
    }
    lock();
    s_job.state = JOB_READING;
    unlock();
    const xiaotai_kv_read_t read = {
        .kind = XIAOTAI_KV_READ_STR_STRICT, .key = PLATFORM_ENVIRONMENT_KEY, .capacity = BASE_BYTES};
    err = xiaotai_kv_read_async(PLATFORM_ENVIRONMENT_NAMESPACE, &read, 1, on_read, token_ctx(token));
    if (err != XIAOTAI_OK) {
        finish(token, err, true);
    }
}

/* [owner] The write's completion. */
static void written(uint32_t token)
{
    lock();
    bool current = s_job.token == token && s_job.state == JOB_WRITTEN;
    xiaotai_err_t result = s_job.result;
    unlock();
    if (!current) {
        return;
    }
    disarm();
    if (result != XIAOTAI_OK) {
        finish(token, result, true);
        return;
    }
    start_read(token);
}

static void after_write(void *ctx) { written(ctx_token(ctx)); }

static void after_read(void *ctx)
{
    uint32_t token = ctx_token(ctx);
    lock();
    bool current = s_job.token == token && s_job.state == JOB_READ;
    xiaotai_err_t result = s_job.result;
    unlock();
    if (current) {
        finish(token, result, true);
    }
}

/* [owner] A step's deadline. */
static void on_deadline(void *ctx)
{
    uint32_t token = ctx_token(ctx);
    lock();
    bool current = s_job.token == token;
    job_state_t state = s_job.state;
    xiaotai_err_t result = s_job.result;
    if (current) {
        s_job.timer = 0; /* this timer has run */
    }
    unlock();
    if (!current) {
        return;
    }
    switch (state) {
    case JOB_WRITTEN: /* the completion could not be handed over */
        written(token);
        break;
    case JOB_READ:
        finish(token, result, true);
        break;
    case JOB_WRITING:
    case JOB_READING:
        finish(token, XIAOTAI_ERR_TIMEOUT, true);
        break;
    default:
        break;
    }
}

/* [owner] Arms the write's deadline and queues the write. On failure nothing
 * reached storage and the job is still SUBMITTING. */
static xiaotai_err_t submit(uint32_t token)
{
    xiaotai_err_t err = arm(token);
    if (err != XIAOTAI_OK) {
        return err;
    }
    xiaotai_kv_op_t op = {.key = PLATFORM_ENVIRONMENT_KEY};
    lock();
    s_job.state = JOB_WRITING;
    if (s_job.erase) {
        op.kind = XIAOTAI_KV_OP_ERASE;
    } else {
        op.kind = XIAOTAI_KV_OP_SET_STR;
        op.data = s_job.base;
    }
    unlock();
    /* Not under s_lock: the completion takes it on the storage worker. The
     * port copies the value before it returns, and only this task (the
     * owner) ends a job, so the job's buffer stays put meanwhile. */
    err = xiaotai_kv_commit_async(PLATFORM_ENVIRONMENT_NAMESPACE, &op, 1, on_written, token_ctx(token));
    if (err != XIAOTAI_OK) {
        lock();
        s_job.state = JOB_SUBMITTING;
        unlock();
        disarm();
    }
    return err;
}

/* [owner] A store accepted on another task. */
static void submit_job(void *ctx)
{
    uint32_t token = ctx_token(ctx);
    lock();
    bool current = s_job.token == token && s_job.state == JOB_SUBMITTING;
    unlock();
    if (!current) {
        return;
    }
    xiaotai_err_t err = submit(token);
    if (err != XIAOTAI_OK) {
        finish(token, err, false);
    }
}

xiaotai_err_t xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done, void *ctx)
{
    char normalized[BASE_BYTES] = {0};
    if (base && xiaotai_platform_environment_normalize(base, normalized, sizeof(normalized)) != XIAOTAI_OK) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_booted)) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    lock();
    if (s_job.state != JOB_IDLE) {
        unlock();
        return XIAOTAI_ERR_BUSY;
    }
    uint32_t token = ++s_sequence;
    if (token == 0) {
        token = ++s_sequence;
    }
    s_job.state = JOB_SUBMITTING;
    s_job.token = token;
    s_job.erase = base == NULL;
    memcpy(s_job.base, normalized, sizeof(s_job.base));
    s_job.done = done;
    s_job.ctx = ctx;
    s_job.result = XIAOTAI_OK;
    s_job.timer = 0;
    unlock();

    xiaotai_err_t err = xiaotai_on_owner() ? submit(token) : xiaotai_run(submit_job, token_ctx(token));
    if (err != XIAOTAI_OK) {
        /* Nothing reached storage: no done, the snapshot stays as it was. */
        lock();
        if (s_job.token == token) {
            memset(&s_job, 0, sizeof(s_job));
        }
        unlock();
    }
    return err;
}

void platform_environment_reset(void)
{
    if (!s_lock_ready) {
        return;
    }
    atomic_store(&s_booted, false);
    lock();
    s_builtin = NULL;
    s_boot_error = XIAOTAI_OK;
    memset(s_boot_base, 0, sizeof(s_boot_base));
    memset(s_saved, 0, sizeof(s_saved));
    s_saved_known = false;
    s_unconfirmed = false;
    /* A pending timer or storage completion finds no job with its token. */
    memset(&s_job, 0, sizeof(s_job));
    unlock();
    s_step_ms = 0;
}

void platform_environment_set_step_ms(uint32_t ms) { s_step_ms = ms; }
