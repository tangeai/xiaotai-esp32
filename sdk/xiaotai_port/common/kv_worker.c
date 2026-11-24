// SPDX-License-Identifier: MIT
/* The KV worker shared by every port: fixed job slots, one worker task, copy-in
 * and copy-out, timeouts that never hand a slot back while the backend may still
 * use it, and coalesced asynchronous writes. Only the backend touches storage.
 *
 * A job is one of: a read of one entry (synchronous get), writes to one
 * namespace with one commit (a single set or erase, the erase of a whole
 * namespace, or a caller's batch: xiaotai_kv_commit and _commit_async), or a
 * read of up to XIAOTAI_KV_READS_MAX entries with a completion
 * (xiaotai_kv_read_async). Single entries and reads describe their entries in
 * the slot; a batch serializes its operations (descriptor, then values) into
 * the job's data, so the permanent slot stays small.
 *
 * Values up to XIAOTAI_KV_SLOT_BYTES live in the slot; a larger job gets a BULK
 * buffer for its life, so the permanent cost stays KV_SLOTS slots whatever
 * XIAOTAI_KV_VALUE_MAX is. A read with a completion never takes one: its
 * capacities must fit the slot. A free slot is all zero bytes: names, values
 * and the completion of a job are wiped when it ends (values may be
 * credentials, names tell what they are).
 *
 * Ordering: one worker takes jobs from one FIFO queue, so "every job queued
 * before" is a count. Each queued job takes the next queue sequence number,
 * the worker advances the done sequence for every job it takes (run or
 * skipped) after its completion returned, and xiaotai_kv_flush waits until the
 * done sequence reaches the queue sequence it saw. */
#include "xiaotai/port/kv.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "kv_backend.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"

#define KV_SLOTS ((unsigned)XIAOTAI_KV_SLOTS)
#define KV_STACK_BYTES ((uint32_t)XIAOTAI_KV_WORKER_STACK_BYTES)
#define KV_PRIORITY 1U
#define KV_SUBMIT_WAIT_MS 5U
/* xiaotai_kv_flush polls: it is rare (before a restart or an environment
 * switch), and polling needs no wake-up object per waiter. */
#define KV_FLUSH_POLL_MS 5U
/* An integer's share of a job's data (kv.h: "an integer counts 8 bytes"). */
#define KV_INT_BYTES 8U

_Static_assert(XIAOTAI_KV_SLOTS >= 2 && XIAOTAI_KV_SLOTS <= 8, "XIAOTAI_KV_SLOTS out of range (2..8)");
_Static_assert(XIAOTAI_KV_WORKER_STACK_BYTES >= 2560 && XIAOTAI_KV_WORKER_STACK_BYTES <= 8192,
               "XIAOTAI_KV_WORKER_STACK_BYTES out of range (2560..8192)");
_Static_assert(XIAOTAI_KV_VALUE_MAX <= UINT16_MAX, "item sizes and offsets are 16 bits");

typedef enum { SLOT_FREE = 0, SLOT_QUEUED, SLOT_RUNNING, SLOT_DONE } slot_state_t;
typedef enum { JOB_GET = 0, JOB_WRITE, JOB_COMMIT, JOB_READ } job_kind_t;
typedef enum { ITEM_BLOB = 0, ITEM_STR, ITEM_INT, ITEM_ERASE, ITEM_ERASE_ALL, ITEM_BLOB_STRICT,
               ITEM_STR_STRICT } item_kind_t;

/* One entry of a job. Written and read with memcpy where it is serialized
 * into job data. */
typedef struct {
    char key[XIAOTAI_KV_NAME_MAX + 1];
    uint8_t kind;       /* item_kind_t */
    uint8_t int_type;   /* xiaotai_kv_int_t, ITEM_INT */
    uint16_t offset;    /* the value within the job's data */
    uint16_t size;      /* value bytes; for a read the capacity */
} kv_item_t;

/* Narrow fields: the slots are permanent BULK memory, internal RAM on boards
 * without external RAM. */
typedef struct {
    uint8_t state;      /* slot_state_t */
    uint8_t kind;       /* job_kind_t */
    bool asynchronous, abandoned;
    uint8_t count;      /* entries: in items[] (GET, WRITE, READ) or serialized at data[0] (COMMIT) */
    uint8_t read_stage; /* xiaotai_kv_read_stage_t; uses alignment before items[] */
    char space[XIAOTAI_KV_NAME_MAX + 1];
    kv_item_t items[XIAOTAI_KV_READS_MAX];
    uint16_t large_size;
    xiaotai_err_t result;
    size_t result_size; /* GET: native stored length can exceed the port buffer limit */
    union {
        xiaotai_kv_done_fn_t write; /* COMMIT with a completion */
        xiaotai_kv_read_fn_t read;  /* READ */
    } done;
    void *ctx;
    uint8_t *large;     /* the job's BULK buffer of large_size bytes; NULL: inline_data */
    _Alignas(8) uint8_t inline_data[XIAOTAI_KV_SLOT_BYTES];
} kv_job_t;

/* Descriptors and values of the largest batch fit the 16-bit sizes. */
_Static_assert(XIAOTAI_KV_OPS_MAX * sizeof(kv_item_t) + XIAOTAI_KV_VALUE_MAX <= UINT16_MAX, "job data size");

/* What a caller asks for, before it becomes a job. */
typedef struct {
    job_kind_t kind;
    bool asynchronous;
    const char *space;
    kv_item_t item;             /* GET, WRITE: the entry, key included */
    const void *value;          /* WRITE: item.size bytes copied into the job */
    const xiaotai_kv_op_t *ops; /* COMMIT */
    const xiaotai_kv_read_t *reads; /* READ */
    size_t count;               /* COMMIT, READ */
    size_t need;                /* data bytes the job takes */
    xiaotai_kv_done_fn_t done;
    xiaotai_kv_read_fn_t read_done;
    void *ctx;
} request_t;

static const char *TAG = "storage";
static kv_job_t *s_jobs;           /* KV_SLOTS slots, BULK memory */
static xiaotai_signal_t s_done[KV_SLOTS]; /* kernel objects stay in internal RAM */
static xiaotai_queue_t s_queue;    /* slot indices */
static xiaotai_mutex_t s_mutex;
static xiaotai_task_t s_task;
static atomic_int s_init_result;
static bool s_ready;
static xiaotai_kv_stats_t s_stats;
/* Mutex held for both; compared by difference, so wrapping is harmless. */
static uint32_t s_queued_seq;      /* jobs put on the queue */
static uint32_t s_done_seq;        /* jobs the worker took off it and finished */

static uint8_t *data_of(kv_job_t *job) { return job->large ? job->large : job->inline_data; }

static size_t capacity_of(const kv_job_t *job) { return job->large ? job->large_size : sizeof(job->inline_data); }

/* Not optimized away although the memory is freed right after. */
static void wipe(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}

/* Returns the slot to all zero bytes and detaches a large buffer (wiped), which
 * the caller frees after releasing the mutex. Mutex held. */
static void *release(kv_job_t *job)
{
    uint8_t *large = job->large;
    if (large) wipe(large, job->large_size);
    memset(job, 0, sizeof(*job));
    return large;
}

static bool valid_name(const char *name)
{
    return name && name[0] && strlen(name) <= XIAOTAI_KV_NAME_MAX;
}

static bool valid_int_type(xiaotai_kv_int_t type) { return (unsigned)type <= (unsigned)XIAOTAI_KV_I64; }

static bool int_in_range(xiaotai_kv_int_t type, int64_t value)
{
    switch (type) {
    case XIAOTAI_KV_U8: return value >= 0 && value <= UINT8_MAX;
    case XIAOTAI_KV_I8: return value >= INT8_MIN && value <= INT8_MAX;
    case XIAOTAI_KV_U16: return value >= 0 && value <= UINT16_MAX;
    case XIAOTAI_KV_I16: return value >= INT16_MIN && value <= INT16_MAX;
    case XIAOTAI_KV_U32: return value >= 0 && value <= (int64_t)UINT32_MAX;
    case XIAOTAI_KV_I32: return value >= INT32_MIN && value <= INT32_MAX;
    case XIAOTAI_KV_U64: /* any 64 bits (kv.h) */
    case XIAOTAI_KV_I64: return true;
    default: return false;
    }
}

/* ------------------------------------------------------------------ worker */

/* A read or written result that is not a storage error: absent, another
 * type, or a value larger than the caller's capacity. */
static bool storage_error(xiaotai_err_t result)
{
    return result != XIAOTAI_OK && result != XIAOTAI_ERR_NOT_FOUND && result != XIAOTAI_ERR_INVALID_SIZE;
}

/* Reads one entry into data + item->offset. */
static void read_item(xiaotai_kv_handle_t handle, const kv_item_t *item, uint8_t *data, xiaotai_kv_value_t *value)
{
    uint8_t *destination = data + item->offset;
    memset(value, 0, sizeof(*value));
    if (item->kind == ITEM_INT) {
        int64_t number = 0;
        value->result = xiaotai_kv_backend_get_int(handle, item->key, (xiaotai_kv_int_t)item->int_type, &number);
        if (value->result == XIAOTAI_OK) {
            value->number = number;
            memcpy(destination, &number, sizeof(number));
        }
        return;
    }
    const bool strict_text = item->kind == ITEM_STR_STRICT;
    const bool text = item->kind == ITEM_STR || strict_text;
    size_t size = item->size;
    if (strict_text) value->result = xiaotai_kv_backend_get_str_strict(handle, item->key, destination, &size);
    else if (item->kind == ITEM_BLOB_STRICT)
        value->result = xiaotai_kv_backend_get_blob_strict(handle, item->key, destination, &size);
    else value->result = xiaotai_kv_backend_get(handle, item->key, text, destination, &size);
    if (value->result == XIAOTAI_OK || value->result == XIAOTAI_ERR_INVALID_SIZE) value->size = size;
    if (value->result != XIAOTAI_OK) return;
    if (text) {
        const bool terminated = size != 0U && (strict_text
            ? size <= item->size && memchr(destination, '\0', size) == destination + size - 1U
            : destination[size - 1U] == '\0');
        if (!terminated) {
            value->result = XIAOTAI_FAIL;
            return;
        }
    }
    value->data = destination;
}

/* Every entry of a read job: absent namespace means absent entries. True when
 * one of them met a storage error. */
static bool run_reads(kv_job_t *job, xiaotai_kv_value_t *values)
{
    xiaotai_kv_handle_t handle = 0;
    const xiaotai_err_t opened = xiaotai_kv_backend_open(job->space, false, &handle);
    job->read_stage = opened == XIAOTAI_OK ? XIAOTAI_KV_READ_STAGE_ENTRY : XIAOTAI_KV_READ_STAGE_OPEN;
    bool failed = false;
    for (unsigned i = 0; i < job->count; ++i) {
        if (opened == XIAOTAI_OK) {
            read_item(handle, &job->items[i], data_of(job), &values[i]);
        } else {
            memset(&values[i], 0, sizeof(values[i]));
            values[i].result = opened;
        }
        failed |= storage_error(values[i].result);
    }
    if (opened == XIAOTAI_OK) xiaotai_kv_backend_close(handle);
    return failed;
}

static xiaotai_err_t write_item(xiaotai_kv_handle_t handle, const kv_item_t *item, const uint8_t *data)
{
    const uint8_t *value = data + item->offset;
    switch (item->kind) {
    case ITEM_BLOB: return xiaotai_kv_backend_set(handle, item->key, false, value, item->size);
    case ITEM_STR: return xiaotai_kv_backend_set(handle, item->key, true, value, item->size);
    case ITEM_INT: {
        int64_t number;
        memcpy(&number, value, sizeof(number));
        return xiaotai_kv_backend_set_int(handle, item->key, (xiaotai_kv_int_t)item->int_type, number);
    }
    case ITEM_ERASE: return xiaotai_kv_backend_erase(handle, item->key);
    case ITEM_ERASE_ALL: return xiaotai_kv_backend_erase_all(handle);
    default: return XIAOTAI_ERR_INVALID_ARG;
    }
}

/* The entries in order, stopping at the first failure; one commit when all
 * succeeded. items may be unaligned (serialized into job data). */
static xiaotai_err_t run_writes(const char *space, const uint8_t *items, size_t count, const uint8_t *data)
{
    xiaotai_kv_handle_t handle = 0;
    xiaotai_err_t result = xiaotai_kv_backend_open(space, true, &handle);
    if (result != XIAOTAI_OK) return result;
    for (size_t i = 0; i < count && result == XIAOTAI_OK; ++i) {
        kv_item_t item;
        memcpy(&item, items + i * sizeof(item), sizeof(item));
        result = write_item(handle, &item, data);
    }
    if (result == XIAOTAI_OK) result = xiaotai_kv_backend_commit(handle);
    xiaotai_kv_backend_close(handle);
    return result;
}

/* Runs a job the worker owns (state RUNNING, mutex free); a completion is
 * called here, before the slot is wiped. *failed: counts as a failure. */
static xiaotai_err_t run(kv_job_t *job, bool *failed)
{
    xiaotai_err_t result = XIAOTAI_OK;
    switch (job->kind) {
    case JOB_GET: {
        xiaotai_kv_value_t value;
        *failed = run_reads(job, &value);
        job->result_size = value.size;
        return value.result;
    }
    case JOB_READ: {
        xiaotai_kv_value_t values[XIAOTAI_KV_READS_MAX];
        *failed = run_reads(job, values);
        job->done.read(values, job->count, job->ctx);
        return XIAOTAI_OK;
    }
    case JOB_WRITE:
        result = run_writes(job->space, (const uint8_t *)job->items, job->count, data_of(job));
        break;
    case JOB_COMMIT:
    default:
        result = run_writes(job->space, data_of(job), job->count, data_of(job));
        break;
    }
    *failed = result != XIAOTAI_OK;
    if (job->done.write) job->done.write(result, job->ctx);
    return result;
}

static void worker(void *arg)
{
    (void)arg;
    /* First NVS access can map the partition table with caches frozen. Run
     * initialization here too, before a caller can submit any jobs. */
    const xiaotai_err_t initialized = xiaotai_kv_backend_init();
    atomic_store(&s_init_result, initialized);
    xiaotai_signal_give(&s_done[0]);
    if (initialized != XIAOTAI_OK) xiaotai_task_exit();
    for (;;) {
        uint8_t index;
        if (!xiaotai_queue_receive(&s_queue, &index, XIAOTAI_WAIT_FOREVER)) continue;
        kv_job_t *job = &s_jobs[index];
        xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
        if (job->abandoned) {
            /* Timed out while queued: skipped, and only now its slot is free. */
            void *large = release(job);
            ++s_done_seq;
            xiaotai_mutex_unlock(&s_mutex);
            xiaotai_mem_free(large);
            continue;
        }
        job->state = SLOT_RUNNING;
        xiaotai_mutex_unlock(&s_mutex);

        /* The running job owns its slot; the mutex is free during storage I/O
         * and during a completion. */
        bool failed = false;
        const xiaotai_err_t result = run(job, &failed);

        void *large = NULL;
        xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
        ++s_stats.completed;
        ++s_done_seq;
        if (failed) ++s_stats.failures;
        const bool report = failed && job->asynchronous && job->kind != JOB_READ;
        if (job->asynchronous || job->abandoned) {
            large = release(job);
        } else {
            job->result = result;
            job->state = SLOT_DONE;
            xiaotai_signal_give(&s_done[index]);
        }
        xiaotai_mutex_unlock(&s_mutex);
        xiaotai_mem_free(large);
        /* Never log keys' values: they may hold credentials. */
        if (report) XIAOTAI_LOGE(TAG, "async write failed: %d", result);
    }
}

xiaotai_err_t xiaotai_kv_init_boot(xiaotai_err_t *local_error)
{
    if (local_error) *local_error = XIAOTAI_OK;
    if (s_ready) return XIAOTAI_OK;
    xiaotai_err_t err;
    s_jobs = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, KV_SLOTS, sizeof(kv_job_t));
    if (!s_jobs) return XIAOTAI_ERR_NO_MEM;
    unsigned signals = 0;
    bool mutex = false, queue = false;
    for (; signals < KV_SLOTS; ++signals) {
        if ((err = xiaotai_signal_init(&s_done[signals])) != XIAOTAI_OK) goto fail;
    }
    if ((err = xiaotai_mutex_init(&s_mutex)) != XIAOTAI_OK) goto fail;
    mutex = true;
    if ((err = xiaotai_queue_init(&s_queue, KV_SLOTS, sizeof(uint8_t), XIAOTAI_MEM_INTERNAL)) != XIAOTAI_OK) goto fail;
    queue = true;
    /* Flash access may disable the cache: the worker's stack stays internal. */
    const xiaotai_task_config_t task = {
        .name = "xiaotai_kv", .entry = worker, .stack_bytes = KV_STACK_BYTES,
        .stack = XIAOTAI_MEM_INTERNAL, .priority = KV_PRIORITY, .core = XIAOTAI_CORE_ANY,
    };
    if ((err = xiaotai_task_create(&task, &s_task)) != XIAOTAI_OK) goto fail;
    /* Reuse slot zero's signal before the queue is available to callers.
     * On failure this is the worker's last access to KV resources. */
    (void)xiaotai_signal_take(&s_done[0], XIAOTAI_WAIT_FOREVER);
    err = atomic_load(&s_init_result);
    if (err != XIAOTAI_OK) {
        if (local_error && xiaotai_kv_backend_recovery_error(err)) *local_error = err;
        goto fail;
    }
    s_ready = true;
    return XIAOTAI_OK;

fail:
    /* Nothing stays half made: a later call starts again from scratch. */
    memset(&s_task, 0, sizeof(s_task));
    if (queue) xiaotai_queue_deinit(&s_queue);
    if (mutex) xiaotai_mutex_deinit(&s_mutex);
    while (signals) xiaotai_signal_deinit(&s_done[--signals]);
    xiaotai_mem_free(s_jobs);
    s_jobs = NULL;
    return err;
}

xiaotai_err_t xiaotai_kv_init(void) { return xiaotai_kv_init_boot(NULL); }

/* ------------------------------------------------------------- submission */

/* Copies a request into a free slot's job (mutex held, data area sized). */
static void fill(kv_job_t *job, const request_t *request)
{
    job->kind = (uint8_t)request->kind;
    job->asynchronous = request->asynchronous;
    job->abandoned = false;
    strcpy(job->space, request->space);
    if (request->kind == JOB_READ) {
        job->done.read = request->read_done;
    } else {
        job->done.write = request->done;
    }
    job->ctx = request->ctx;
    uint8_t *data = data_of(job);
    switch (request->kind) {
    case JOB_GET:
    case JOB_WRITE:
        job->count = 1;
        job->items[0] = request->item;
        if (request->kind == JOB_WRITE && request->item.size) memcpy(data, request->value, request->item.size);
        break;
    case JOB_READ: {
        uint16_t offset = 0;
        job->count = (uint8_t)request->count;
        for (size_t i = 0; i < request->count; ++i) {
            const xiaotai_kv_read_t *read = &request->reads[i];
            kv_item_t *item = &job->items[i];
            strcpy(item->key, read->key);
            item->offset = offset;
            if (read->kind == XIAOTAI_KV_READ_INT) {
                item->kind = ITEM_INT;
                item->int_type = (uint8_t)read->int_type;
                item->size = KV_INT_BYTES;
            } else {
                item->kind = read->kind == XIAOTAI_KV_READ_STR_STRICT ? ITEM_STR_STRICT :
                    read->kind == XIAOTAI_KV_READ_STR ? ITEM_STR : ITEM_BLOB;
                item->size = (uint16_t)read->capacity;
            }
            offset = (uint16_t)(offset + item->size);
        }
        break;
    }
    case JOB_COMMIT:
    default: {
        /* Descriptors first, then the values in order. */
        job->count = (uint8_t)request->count;
        size_t offset = request->count * sizeof(kv_item_t);
        for (size_t i = 0; i < request->count; ++i) {
            const xiaotai_kv_op_t *op = &request->ops[i];
            kv_item_t item;
            memset(&item, 0, sizeof(item));
            strcpy(item.key, op->key);
            item.offset = (uint16_t)offset;
            switch (op->kind) {
            case XIAOTAI_KV_OP_SET:
                item.kind = ITEM_BLOB;
                item.size = (uint16_t)op->size;
                if (op->size) memcpy(data + offset, op->data, op->size);
                break;
            case XIAOTAI_KV_OP_SET_STR:
                item.kind = ITEM_STR;
                item.size = (uint16_t)(strlen(op->data) + 1);
                memcpy(data + offset, op->data, item.size);
                break;
            case XIAOTAI_KV_OP_SET_U8:
            case XIAOTAI_KV_OP_SET_INT: {
                const int64_t number = op->kind == XIAOTAI_KV_OP_SET_U8 ? *(const uint8_t *)op->data : op->number;
                item.kind = ITEM_INT;
                item.int_type = (uint8_t)(op->kind == XIAOTAI_KV_OP_SET_U8 ? XIAOTAI_KV_U8 : op->int_type);
                item.size = KV_INT_BYTES;
                memcpy(data + offset, &number, sizeof(number));
                break;
            }
            case XIAOTAI_KV_OP_ERASE:
            default:
                item.kind = ITEM_ERASE;
                break;
            }
            memcpy(data + i * sizeof(item), &item, sizeof(item));
            offset += item.size;
        }
        break;
    }
    }
}

/* Claims a slot (coalescing an asynchronous single write) and queues it. */
static xiaotai_err_t submit(const request_t *request, uint8_t *claimed)
{
    /* The worker itself would wait for its own job: refused like interrupts. */
    if (!s_ready || xiaotai_in_isr() || xiaotai_task_is_current(&s_task)) return XIAOTAI_ERR_INVALID_STATE;
    /* Taken before the lock; given back below when no job keeps it. */
    uint8_t *large = NULL;
    if (request->need > XIAOTAI_KV_SLOT_BYTES) {
        large = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, request->need);
        if (!large) return XIAOTAI_ERR_NO_MEM;
    }
    if (!xiaotai_mutex_lock(&s_mutex, KV_SUBMIT_WAIT_MS)) {
        xiaotai_mem_free(large);
        return XIAOTAI_ERR_TIMEOUT;
    }
    xiaotai_err_t result = XIAOTAI_ERR_NO_MEM;
    void *spare = large; /* never holds a value: wiped before it gets here */
    if (request->asynchronous && request->kind == JOB_WRITE) {
        const kv_item_t *wanted = &request->item;
        for (unsigned i = 0; i < KV_SLOTS; ++i) {
            kv_job_t *job = &s_jobs[i];
            const kv_item_t *queued = &job->items[0];
            /* Same type only: a queued write of another type keeps its own job.
             * Batches and reads never merge. */
            if (job->state == SLOT_QUEUED && job->asynchronous && job->kind == JOB_WRITE &&
                queued->kind == wanted->kind && (wanted->kind != ITEM_INT || queued->int_type == wanted->int_type) &&
                strcmp(job->space, request->space) == 0 && strcmp(queued->key, wanted->key) == 0) {
                memset(data_of(job), 0, capacity_of(job));
                if (request->need > capacity_of(job)) {
                    /* The queued job's buffer is too small: it takes the new one. */
                    spare = job->large;
                    job->large = large;
                    job->large_size = (uint16_t)request->need;
                }
                if (wanted->size) memcpy(data_of(job), request->value, wanted->size);
                job->items[0].size = wanted->size;
                ++s_stats.coalesced;
                result = XIAOTAI_OK;
                goto out;
            }
        }
    }
    for (uint8_t i = 0; i < KV_SLOTS; ++i) {
        kv_job_t *job = &s_jobs[i];
        if (job->state != SLOT_FREE) continue;
        if (large) {
            job->large = large;
            job->large_size = (uint16_t)request->need;
            spare = NULL;
        }
        fill(job, request);
        job->state = SLOT_QUEUED;
        (void)xiaotai_signal_take(&s_done[i], 0); /* drop a stale completion */
        if (!xiaotai_queue_send(&s_queue, &i, 0)) {
            spare = release(job);
            ++s_stats.full;
            goto out;
        }
        ++s_stats.submitted;
        ++s_queued_seq;
        if (claimed) *claimed = i;
        result = XIAOTAI_OK;
        goto out;
    }
    ++s_stats.full;
out:
    xiaotai_mutex_unlock(&s_mutex);
    xiaotai_mem_free(spare);
    return result;
}

typedef struct {
    void *data;       /* blob or string */
    size_t *size;     /* capacity in, length out */
    int64_t *number;  /* integer */
    xiaotai_kv_read_stage_t *stage;
} get_out_t;

/* Waits for a synchronous job; copies a read value out only if it completed. */
static xiaotai_err_t wait(uint8_t index, uint32_t timeout_ms, const get_out_t *out)
{
    (void)xiaotai_signal_take(&s_done[index], timeout_ms);
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    kv_job_t *job = &s_jobs[index];
    xiaotai_err_t result;
    void *large = NULL;
    if (job->state == SLOT_DONE) {
        result = job->result;
        if (out && job->kind == JOB_GET) {
            if (out->stage) *out->stage = (xiaotai_kv_read_stage_t)job->read_stage;
            if (job->items[0].kind == ITEM_INT) {
                if (result == XIAOTAI_OK) memcpy(out->number, data_of(job), sizeof(*out->number));
            } else {
                /* On success the backend's result fits the requested capacity. */
                if (result == XIAOTAI_OK) memcpy(out->data, data_of(job), job->result_size);
                if (result == XIAOTAI_OK || result == XIAOTAI_ERR_INVALID_SIZE) *out->size = job->result_size;
            }
        }
        large = release(job);
    } else {
        /* Queued or running: the worker still owns the slot and releases it. */
        job->abandoned = true;
        ++s_stats.timeouts;
        result = XIAOTAI_ERR_TIMEOUT;
    }
    xiaotai_mutex_unlock(&s_mutex);
    xiaotai_mem_free(large);
    return result;
}

static xiaotai_err_t call(const request_t *request, uint32_t timeout_ms, const get_out_t *out)
{
    if (timeout_ms == 0 || timeout_ms > XIAOTAI_KV_WAIT_MAX_MS) return XIAOTAI_ERR_INVALID_ARG;
    uint8_t index;
    const xiaotai_err_t err = submit(request, &index);
    if (err != XIAOTAI_OK) return err;
    return wait(index, timeout_ms, out);
}

/* A request for one entry; the key's length is checked by the caller. */
static request_t single(job_kind_t kind, bool asynchronous, const char *space, const char *key, item_kind_t item_kind)
{
    request_t request;
    memset(&request, 0, sizeof(request));
    request.kind = kind;
    request.asynchronous = asynchronous;
    request.space = space;
    request.item.kind = (uint8_t)item_kind;
    if (key) strcpy(request.item.key, key);
    return request;
}

/* --------------------------------------------------------- synchronous calls */

static xiaotai_err_t get_bytes(const char *space, const char *key, item_kind_t kind, void *data, size_t *size,
                               xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms)
{
    if (!valid_name(space) || !valid_name(key)) return XIAOTAI_ERR_INVALID_ARG;
    request_t request = single(JOB_GET, false, space, key, kind);
    const size_t capacity = *size > XIAOTAI_KV_VALUE_MAX ? XIAOTAI_KV_VALUE_MAX : *size;
    request.item.size = (uint16_t)capacity;
    request.need = capacity;
    const get_out_t out = {.data = data, .size = size, .stage = stage};
    return call(&request, timeout_ms, &out);
}

xiaotai_err_t xiaotai_kv_get(const char *space, const char *key, void *data, size_t *size, uint32_t timeout_ms)
{
    if (!data || !size) return XIAOTAI_ERR_INVALID_ARG;
    return get_bytes(space, key, ITEM_BLOB, data, size, NULL, timeout_ms);
}

xiaotai_err_t xiaotai_kv_get_blob_strict(const char *space, const char *key, void *data, size_t *size,
                                         uint32_t timeout_ms)
{
    if (!data || !size) return XIAOTAI_ERR_INVALID_ARG;
    return get_bytes(space, key, ITEM_BLOB_STRICT, data, size, NULL, timeout_ms);
}

static xiaotai_err_t get_string(const char *space, const char *key, item_kind_t kind, char *out, size_t capacity,
                                  xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms)
{
    if (stage) *stage = XIAOTAI_KV_READ_STAGE_JOB;
    if (!out || capacity == 0) return XIAOTAI_ERR_INVALID_ARG;
    size_t size = capacity;
    const xiaotai_err_t err = get_bytes(space, key, kind, out, &size, stage, timeout_ms);
    if (err != XIAOTAI_OK) out[0] = '\0';
    return err;
}

xiaotai_err_t xiaotai_kv_get_str(const char *space, const char *key, char *out, size_t capacity, uint32_t timeout_ms)
{
    return get_string(space, key, ITEM_STR, out, capacity, NULL, timeout_ms);
}

xiaotai_err_t xiaotai_kv_get_str_with_stage(const char *space, const char *key, char *out, size_t capacity,
                                           xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms)
{
    return get_string(space, key, ITEM_STR, out, capacity, stage, timeout_ms);
}

xiaotai_err_t xiaotai_kv_get_str_strict(const char *space, const char *key, char *out, size_t capacity,
                                        uint32_t timeout_ms)
{
    return get_string(space, key, ITEM_STR_STRICT, out, capacity, NULL, timeout_ms);
}

xiaotai_err_t xiaotai_kv_get_int(const char *space, const char *key, xiaotai_kv_int_t type, int64_t *value,
                                 uint32_t timeout_ms)
{
    return xiaotai_kv_get_int_with_stage(space, key, type, value, NULL, timeout_ms);
}

xiaotai_err_t xiaotai_kv_get_int_with_stage(const char *space, const char *key, xiaotai_kv_int_t type, int64_t *value,
                                           xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms)
{
    if (stage) *stage = XIAOTAI_KV_READ_STAGE_JOB;
    if (!value || !valid_int_type(type) || !valid_name(space) || !valid_name(key)) return XIAOTAI_ERR_INVALID_ARG;
    request_t request = single(JOB_GET, false, space, key, ITEM_INT);
    request.item.int_type = (uint8_t)type;
    request.item.size = KV_INT_BYTES;
    request.need = KV_INT_BYTES;
    const get_out_t out = {.number = value, .stage = stage};
    return call(&request, timeout_ms, &out);
}

xiaotai_err_t xiaotai_kv_get_u8(const char *space, const char *key, uint8_t *value, uint32_t timeout_ms)
{
    if (!value) return XIAOTAI_ERR_INVALID_ARG;
    int64_t number = 0;
    const xiaotai_err_t err = xiaotai_kv_get_int(space, key, XIAOTAI_KV_U8, &number, timeout_ms);
    if (err == XIAOTAI_OK) *value = (uint8_t)number;
    return err;
}

/* A single write of a blob or string: checks and builds the request. */
static xiaotai_err_t bytes_request(const char *space, const char *key, bool text, const void *data, size_t size,
                                   bool asynchronous, request_t *request)
{
    if (!valid_name(space) || !valid_name(key) || (size && !data)) return XIAOTAI_ERR_INVALID_ARG;
    if (size > XIAOTAI_KV_VALUE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    *request = single(JOB_WRITE, asynchronous, space, key, text ? ITEM_STR : ITEM_BLOB);
    request->item.size = (uint16_t)size;
    request->value = data;
    request->need = size;
    return XIAOTAI_OK;
}

/* String length with its terminator, or 0 when longer than any value. */
static size_t text_size(const char *value)
{
    const size_t length = strnlen(value, XIAOTAI_KV_VALUE_MAX);
    return length < XIAOTAI_KV_VALUE_MAX ? length + 1 : 0;
}

static xiaotai_err_t int_request(const char *space, const char *key, xiaotai_kv_int_t type, const int64_t *value,
                                 bool asynchronous, request_t *request)
{
    if (!valid_name(space) || !valid_name(key) || !valid_int_type(type) || !int_in_range(type, *value)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    *request = single(JOB_WRITE, asynchronous, space, key, ITEM_INT);
    request->item.int_type = (uint8_t)type;
    request->item.size = KV_INT_BYTES;
    request->value = value;
    request->need = KV_INT_BYTES;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_set(const char *space, const char *key, const void *data, size_t size, uint32_t timeout_ms)
{
    request_t request;
    const xiaotai_err_t err = bytes_request(space, key, false, data, size, false, &request);
    return err != XIAOTAI_OK ? err : call(&request, timeout_ms, NULL);
}

xiaotai_err_t xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout_ms)
{
    if (!value) return XIAOTAI_ERR_INVALID_ARG;
    const size_t size = text_size(value);
    if (!size) return XIAOTAI_ERR_INVALID_SIZE;
    request_t request;
    const xiaotai_err_t err = bytes_request(space, key, true, value, size, false, &request);
    return err != XIAOTAI_OK ? err : call(&request, timeout_ms, NULL);
}

xiaotai_err_t xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t type, int64_t value,
                                 uint32_t timeout_ms)
{
    request_t request;
    const xiaotai_err_t err = int_request(space, key, type, &value, false, &request);
    return err != XIAOTAI_OK ? err : call(&request, timeout_ms, NULL);
}

xiaotai_err_t xiaotai_kv_set_u8(const char *space, const char *key, uint8_t value, uint32_t timeout_ms)
{
    return xiaotai_kv_set_int(space, key, XIAOTAI_KV_U8, value, timeout_ms);
}

xiaotai_err_t xiaotai_kv_erase(const char *space, const char *key, uint32_t timeout_ms)
{
    if (!valid_name(space) || !valid_name(key)) return XIAOTAI_ERR_INVALID_ARG;
    const request_t request = single(JOB_WRITE, false, space, key, ITEM_ERASE);
    return call(&request, timeout_ms, NULL);
}

xiaotai_err_t xiaotai_kv_erase_space(const char *space, uint32_t timeout_ms)
{
    if (!valid_name(space)) return XIAOTAI_ERR_INVALID_ARG;
    const request_t request = single(JOB_WRITE, false, space, NULL, ITEM_ERASE_ALL);
    return call(&request, timeout_ms, NULL);
}

/* ----------------------------------------------------------------- batches */

/* Checks a batch against the rules of xiaotai_kv_commit; *need: the job's data
 * (descriptors and values). */
static xiaotai_err_t commit_request(const char *space, const xiaotai_kv_op_t *ops, size_t count, request_t *request)
{
    if (!valid_name(space) || !ops) return XIAOTAI_ERR_INVALID_ARG;
    if (count == 0 || count > XIAOTAI_KV_OPS_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_kv_op_t *op = &ops[i];
        if (!valid_name(op->key)) return XIAOTAI_ERR_INVALID_ARG;
        size_t bytes = 0;
        switch (op->kind) {
        case XIAOTAI_KV_OP_SET:
            if (op->size && !op->data) return XIAOTAI_ERR_INVALID_ARG;
            if (op->size > XIAOTAI_KV_VALUE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
            bytes = op->size;
            break;
        case XIAOTAI_KV_OP_SET_STR:
            if (!op->data) return XIAOTAI_ERR_INVALID_ARG;
            bytes = text_size(op->data);
            if (!bytes) return XIAOTAI_ERR_INVALID_SIZE;
            break;
        case XIAOTAI_KV_OP_SET_U8:
            if (!op->data) return XIAOTAI_ERR_INVALID_ARG;
            bytes = KV_INT_BYTES;
            break;
        case XIAOTAI_KV_OP_SET_INT:
            if (!valid_int_type(op->int_type) || !int_in_range(op->int_type, op->number)) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            bytes = KV_INT_BYTES;
            break;
        case XIAOTAI_KV_OP_ERASE:
            break;
        default:
            return XIAOTAI_ERR_INVALID_ARG;
        }
        if (bytes > XIAOTAI_KV_VALUE_MAX - total) return XIAOTAI_ERR_INVALID_SIZE;
        total += bytes;
    }
    memset(request, 0, sizeof(*request));
    request->kind = JOB_COMMIT;
    request->space = space;
    request->ops = ops;
    request->count = count;
    request->need = count * sizeof(kv_item_t) + total;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_commit(const char *space, const xiaotai_kv_op_t *ops, size_t count, uint32_t timeout_ms)
{
    request_t request;
    const xiaotai_err_t err = commit_request(space, ops, count, &request);
    return err != XIAOTAI_OK ? err : call(&request, timeout_ms, NULL);
}

xiaotai_err_t xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                      xiaotai_kv_done_fn_t done, void *ctx)
{
    request_t request;
    const xiaotai_err_t err = commit_request(space, ops, count, &request);
    if (err != XIAOTAI_OK) return err;
    request.asynchronous = true;
    request.done = done;
    request.ctx = ctx;
    return submit(&request, NULL);
}

xiaotai_err_t xiaotai_kv_read_async(const char *space, const xiaotai_kv_read_t *reads, size_t count,
                                    xiaotai_kv_read_fn_t done, void *ctx)
{
    if (!valid_name(space) || !reads || !done || count == 0 || count > XIAOTAI_KV_READS_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_kv_read_t *read = &reads[i];
        if (!valid_name(read->key)) return XIAOTAI_ERR_INVALID_ARG;
        if (read->kind == XIAOTAI_KV_READ_INT) {
            if (!valid_int_type(read->int_type)) return XIAOTAI_ERR_INVALID_ARG;
        } else if (read->kind == XIAOTAI_KV_READ_BLOB || read->kind == XIAOTAI_KV_READ_STR ||
                   read->kind == XIAOTAI_KV_READ_STR_STRICT) {
            if (read->capacity == 0) return XIAOTAI_ERR_INVALID_ARG;
        } else {
            return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    /* The values travel in one permanent slot: no buffer is ever taken. */
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t bytes = reads[i].kind == XIAOTAI_KV_READ_INT ? KV_INT_BYTES : reads[i].capacity;
        if (bytes > XIAOTAI_KV_SLOT_BYTES - total) return XIAOTAI_ERR_INVALID_SIZE;
        total += bytes;
    }
    request_t request;
    memset(&request, 0, sizeof(request));
    request.kind = JOB_READ;
    request.asynchronous = true;
    request.space = space;
    request.reads = reads;
    request.count = count;
    request.need = total;
    request.read_done = done;
    request.ctx = ctx;
    return submit(&request, NULL);
}

/* ------------------------------------------------------- asynchronous writes */

xiaotai_err_t xiaotai_kv_set_async(const char *space, const char *key, const void *data, size_t size)
{
    request_t request;
    const xiaotai_err_t err = bytes_request(space, key, false, data, size, true, &request);
    return err != XIAOTAI_OK ? err : submit(&request, NULL);
}

xiaotai_err_t xiaotai_kv_set_str_async(const char *space, const char *key, const char *value)
{
    if (!value) return XIAOTAI_ERR_INVALID_ARG;
    const size_t size = text_size(value);
    if (!size) return XIAOTAI_ERR_INVALID_SIZE;
    request_t request;
    const xiaotai_err_t err = bytes_request(space, key, true, value, size, true, &request);
    return err != XIAOTAI_OK ? err : submit(&request, NULL);
}

xiaotai_err_t xiaotai_kv_set_int_async(const char *space, const char *key, xiaotai_kv_int_t type, int64_t value)
{
    request_t request;
    const xiaotai_err_t err = int_request(space, key, type, &value, true, &request);
    return err != XIAOTAI_OK ? err : submit(&request, NULL);
}

/* ------------------------------------------------------------------ others */

xiaotai_err_t xiaotai_kv_flush(uint32_t timeout_ms)
{
    if (!s_ready) return XIAOTAI_OK;
    if (xiaotai_in_isr() || xiaotai_task_is_current(&s_task)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    const uint32_t target = s_queued_seq;
    xiaotai_mutex_unlock(&s_mutex);
    const int64_t deadline = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
        const uint32_t done = s_done_seq;
        xiaotai_mutex_unlock(&s_mutex);
        if ((int32_t)(done - target) >= 0) return XIAOTAI_OK;
        const int64_t left_us = deadline - xiaotai_time_us();
        if (left_us <= 0) return XIAOTAI_ERR_TIMEOUT;
        xiaotai_delay_ms(left_us < (int64_t)KV_FLUSH_POLL_MS * 1000 ? (uint32_t)((left_us + 999) / 1000)
                                                                     : KV_FLUSH_POLL_MS);
    }
}

void xiaotai_kv_stats(xiaotai_kv_stats_t *stats)
{
    if (!stats) return;
    if (!s_ready) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    *stats = s_stats;
    xiaotai_mutex_unlock(&s_mutex);
}
