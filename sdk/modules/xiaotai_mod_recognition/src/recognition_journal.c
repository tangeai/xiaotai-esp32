// SPDX-License-Identifier: MIT
#include "recognition_journal.h"
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"

#include <string.h>

#define JOURNAL_SPACE "xt_recognition"
#define JOURNAL_SCOPE "scope"
#define JOURNAL_PENDING "pending"
#define JOURNAL_BYTES 2048u
#define JOURNAL_ITEMS 8u
#define JOURNAL_TOKENS (1u + 7u * 2u + JOURNAL_ITEMS * 5u)
#define JOURNAL_KV_WAIT_MS 5000u
#define JOURNAL_QUEUE_DEPTH 4u

typedef xiaotai_recognition_journal_item_t journal_item_t;
_Static_assert(JOURNAL_ITEMS == XIAOTAI_RECOGNITION_JOURNAL_ITEMS_MAX, "journal mirror item limit");

typedef struct {
    xiaotai_recognition_journal_record_t record;
    unsigned version;
    size_t item_count;
    journal_item_t items[JOURNAL_ITEMS];
} journal_disk_t;

typedef struct {
    xiaotai_recognition_journal_mirror_t update;
    xiaotai_err_t result;
    xiaotai_recognition_journal_item_state_t item;
    xiaotai_recognition_journal_done_t done;
    void *ctx;
    uint32_t committed_scope; /* nonzero only after ADVANCE_SCOPE commits */
    bool load;
} journal_completion_t;

typedef struct {
    xiaotai_recognition_journal_mirror_t mirror; /* only the owner sees it */
    journal_completion_t completion; /* worker until posted, owner until acknowledged */
} journal_storage_t;

static struct {
    xiaotai_queue_t queue;
    xiaotai_signal_t delivered;
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
    xiaotai_signal_t exited;
#endif
    xiaotai_task_t worker;
    journal_storage_t *storage;
    size_t pending; /* owner only */
    size_t loads;   /* owner only; the mirror reads as READING while any LOAD is queued */
} journal;

_Static_assert(JOURNAL_BYTES <= XIAOTAI_KV_VALUE_MAX, "legacy record must fit the KV port");

static bool identifier(const char *id)
{
    for (size_t i = 0; i < XIAOTAI_RECOGNITION_JOURNAL_ID_CAP; ++i) {
        const char ch = id[i];
        if (!ch) return i != 0;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    return false;
}

static bool record_valid(const xiaotai_recognition_journal_record_t *r)
{
    return identifier(r->device_id) && identifier(r->request_id) &&
        r->config_revision <= XIAOTAI_RECOGNITION_JOURNAL_REVISION_MAX &&
        r->seconds >= 1 && r->seconds <= 60;
}

static bool same_record(const xiaotai_recognition_journal_record_t *a,
                        const xiaotai_recognition_journal_record_t *b)
{
    return strcmp(a->device_id, b->device_id) == 0 && strcmp(a->request_id, b->request_id) == 0 &&
        a->config_revision == b->config_revision && a->seconds == b->seconds && a->scope == b->scope;
}

bool rc_record_matches(const xiaotai_recognition_journal_mirror_t *mirror,
                       const xiaotai_recognition_journal_record_t *record)
{
    return mirror->state == XIAOTAI_RECOGNITION_JOURNAL_PRESENT && same_record(&mirror->record, record);
}

xiaotai_recognition_journal_item_state_t rc_item_consumption(
    const xiaotai_recognition_journal_mirror_t *mirror, const char *item_id)
{
    for (size_t i = 0; i < mirror->item_count; ++i)
        if (!strcmp(mirror->items[i].id, item_id)) return mirror->items[i].state;
    return XIAOTAI_RECOGNITION_JOURNAL_UNTOUCHED;
}

static bool item_edge(xiaotai_recognition_journal_item_state_t from,
                      xiaotai_recognition_journal_item_state_t to)
{
    return (from == XIAOTAI_RECOGNITION_JOURNAL_UNTOUCHED &&
            (to == XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED || to == XIAOTAI_RECOGNITION_JOURNAL_DISMISSED)) ||
        (from == XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED &&
            (to == XIAOTAI_RECOGNITION_JOURNAL_PLAYED || to == XIAOTAI_RECOGNITION_JOURNAL_DISMISSED));
}

static bool read_int(const xiaotai_json_doc_t *doc, int index, const char *key,
                     int64_t min, int64_t max, int64_t *out)
{
    return xiaotai_json_int(doc, xiaotai_json_get(doc, index, key), out) == XIAOTAI_OK &&
        *out >= min && *out <= max;
}

static bool read_id(const xiaotai_json_doc_t *doc, int index, const char *key, char *out)
{
    return xiaotai_json_string(doc, xiaotai_json_get(doc, index, key), out,
                              XIAOTAI_RECOGNITION_JOURNAL_ID_CAP, NULL) == XIAOTAI_OK && identifier(out);
}

static size_t find_item(const journal_disk_t *disk, const char *id)
{
    size_t i = 0;
    while (i < disk->item_count && strcmp(disk->items[i].id, id) != 0) ++i;
    return i;
}

static bool read_items(const xiaotai_json_doc_t *doc, journal_disk_t *disk)
{
    const int array = xiaotai_json_get(doc, 0, "items");
    if (array < 0 || doc->tokens[array].type != XIAOTAI_JSON_ARRAY ||
        doc->tokens[array].children > JOURNAL_ITEMS) return false;

    const size_t count = doc->tokens[array].children;
    for (size_t i = 0; i < count; ++i) {
        const int index = xiaotai_json_at(doc, array, i);
        journal_item_t *item = &disk->items[i];
        int64_t state;
        if (index < 0 || doc->tokens[index].type != XIAOTAI_JSON_OBJECT ||
            doc->tokens[index].children != 2 ||
            !read_id(doc, index, "item_id", item->id) ||
            !read_int(doc, index, "state", XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED,
                      XIAOTAI_RECOGNITION_JOURNAL_DISMISSED, &state) ||
            find_item(disk, item->id) != i) return false;
        item->state = (xiaotai_recognition_journal_item_state_t)state;
        ++disk->item_count;
    }
    return true;
}

static bool read_object(const xiaotai_json_doc_t *doc, journal_disk_t *disk)
{
    int64_t version, revision, seconds, scope = 0;
    /* Every required key is checked and the count must match: no unknown
     * or repeated key fits. The only nested containers are v3's items. */
    if (!doc->count || doc->tokens[0].type != XIAOTAI_JSON_OBJECT ||
        !read_int(doc, 0, "version", 1, 4, &version) ||
        doc->tokens[0].children != (version == 1 ? 5 : version == 3 ? 7 : 6) ||
        !read_id(doc, 0, "device_id", disk->record.device_id) ||
        !read_id(doc, 0, "request_id", disk->record.request_id) ||
        !read_int(doc, 0, "revision", 0, XIAOTAI_RECOGNITION_JOURNAL_REVISION_MAX, &revision) ||
        !read_int(doc, 0, "seconds", 1, 60, &seconds) ||
        (version != 1 && !read_int(doc, 0, "scope", 0, UINT32_MAX, &scope)) ||
        (version == 3 && !read_items(doc, disk))) return false;

    disk->version = (unsigned)version;
    disk->record.config_revision = (uint64_t)revision;
    disk->record.seconds = (uint32_t)seconds;
    disk->record.scope = (uint32_t)scope;
    return true;
}

static xiaotai_err_t decode_record(const char *text, journal_disk_t *disk)
{
    const char *end = memchr(text, '\0', JOURNAL_BYTES);
    if (!end) return XIAOTAI_FAIL;
    const size_t len = (size_t)(end - text);
    size_t count = 0;
    if (xiaotai_json_parse(text, len, NULL, 0, &count, NULL) != XIAOTAI_OK ||
        count == 0 || count > JOURNAL_TOKENS) return XIAOTAI_RECOGNITION_JOURNAL_INVALID_RESPONSE;

    xiaotai_json_token_t *tokens = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, count * sizeof(*tokens));
    if (!tokens) return XIAOTAI_ERR_NO_MEM;
    xiaotai_json_doc_t doc;
    const bool ok = xiaotai_json_parse(text, len, tokens, count, &count, &doc) == XIAOTAI_OK &&
        read_object(&doc, disk);
    xiaotai_mem_free(tokens);
    return ok ? XIAOTAI_OK : XIAOTAI_RECOGNITION_JOURNAL_INVALID_RESPONSE;
}

static xiaotai_err_t write_record(char *text, const journal_disk_t *disk)
{
    xiaotai_json_writer_t w;
    xiaotai_json_writer_init(&w, text, JOURNAL_BYTES);
    xiaotai_json_object_begin(&w, NULL);
    xiaotai_json_put_int(&w, "version", disk->version);
    xiaotai_json_put_string(&w, "device_id", disk->record.device_id);
    xiaotai_json_put_string(&w, "request_id", disk->record.request_id);
    xiaotai_json_put_int(&w, "revision", (int64_t)disk->record.config_revision);
    xiaotai_json_put_int(&w, "seconds", disk->record.seconds);
    xiaotai_json_put_int(&w, "scope", disk->record.scope);
    if (disk->version == 3) {
        xiaotai_json_array_begin(&w, "items");
        for (size_t i = 0; i < disk->item_count; ++i) {
            xiaotai_json_object_begin(&w, NULL);
            xiaotai_json_put_string(&w, "item_id", disk->items[i].id);
            xiaotai_json_put_int(&w, "state", disk->items[i].state);
            xiaotai_json_object_end(&w);
        }
        xiaotai_json_array_end(&w);
    }
    xiaotai_json_object_end(&w);
    xiaotai_err_t err = xiaotai_json_writer_end(&w, NULL);
    if (err != XIAOTAI_OK) return err;
    return xiaotai_kv_set_str(JOURNAL_SPACE, JOURNAL_PENDING, text, JOURNAL_KV_WAIT_MS);
}

static void mirror_from_disk(journal_completion_t *completion, xiaotai_err_t err,
                             const journal_disk_t *disk)
{
    xiaotai_recognition_journal_mirror_t *m = &completion->update;
    memset(m, 0, sizeof(*m));
    if (err == XIAOTAI_OK) {
        m->state = XIAOTAI_RECOGNITION_JOURNAL_PRESENT;
        m->record = disk->record;
        m->version = disk->version;
        m->item_count = (uint8_t)disk->item_count;
        memcpy(m->items, disk->items, disk->item_count * sizeof(m->items[0]));
    } else if (err == XIAOTAI_ERR_NOT_FOUND) {
        m->state = XIAOTAI_RECOGNITION_JOURNAL_ABSENT;
    } else {
        m->state = XIAOTAI_RECOGNITION_JOURNAL_ERROR;
        m->error = err;
    }
}

static void mirror_failure(journal_completion_t *completion, xiaotai_err_t err)
{
    completion->update = (xiaotai_recognition_journal_mirror_t){
        .state = XIAOTAI_RECOGNITION_JOURNAL_ERROR, .error = err,
    };
}

static xiaotai_err_t process_record_job(const xiaotai_recognition_journal_job_t *job,
    journal_completion_t *completion, char *text, uint32_t scope)
{
    journal_disk_t disk = {0};
    xiaotai_err_t err = xiaotai_kv_get_str(JOURNAL_SPACE, JOURNAL_PENDING, text, JOURNAL_BYTES,
                                          JOURNAL_KV_WAIT_MS);
    if (err == XIAOTAI_OK) {
        err = decode_record(text, &disk);
        if (err == XIAOTAI_OK && disk.record.scope > scope) err = XIAOTAI_RECOGNITION_JOURNAL_INVALID_RESPONSE;
        if (err == XIAOTAI_OK && disk.record.scope < scope) err = XIAOTAI_ERR_NOT_FOUND;
    }
    mirror_from_disk(completion, err, &disk);
    if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_LOAD) return err;
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_NOT_FOUND) return err;

    if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_PREPARE) {
        if (err == XIAOTAI_OK) return XIAOTAI_ERR_INVALID_STATE;
        disk = (journal_disk_t){.version = 2, .record = job->record};
        disk.record.scope = scope;
    } else {
        const bool clear = job->operation == XIAOTAI_RECOGNITION_JOURNAL_CLEAR ||
            job->operation == XIAOTAI_RECOGNITION_JOURNAL_CLEAR_CONFLICT;
        if (err == XIAOTAI_ERR_NOT_FOUND) {
            if (clear) return XIAOTAI_OK;
            return job->operation == XIAOTAI_RECOGNITION_JOURNAL_MARK_CONFLICT ? XIAOTAI_ERR_INVALID_STATE : err;
        }
        if (!same_record(&disk.record, &job->record)) return XIAOTAI_ERR_INVALID_STATE;

        if (clear) {
            if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_CLEAR_CONFLICT && disk.version != 4) {
                return XIAOTAI_ERR_INVALID_STATE;
            }
            err = xiaotai_kv_erase(JOURNAL_SPACE, JOURNAL_PENDING, JOURNAL_KV_WAIT_MS);
            if (err == XIAOTAI_OK) mirror_from_disk(completion, XIAOTAI_ERR_NOT_FOUND, &disk);
            else mirror_failure(completion, err);
            return err;
        }
        if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_MARK_CONFLICT) {
            if (disk.version == 4) return XIAOTAI_OK;
            if (disk.version != 2) return XIAOTAI_ERR_INVALID_STATE;
            disk.version = 4;
        } else {
            const size_t index = find_item(&disk, job->item_id);
            const xiaotai_recognition_journal_item_state_t state = index == disk.item_count ?
                XIAOTAI_RECOGNITION_JOURNAL_UNTOUCHED : disk.items[index].state;
            if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_ITEM_READ) {
                completion->item = state;
                return XIAOTAI_OK;
            }
            if (state != job->expected_item) return XIAOTAI_ERR_INVALID_STATE;
            if (index == disk.item_count) {
                if (disk.item_count == JOURNAL_ITEMS) return XIAOTAI_ERR_INVALID_SIZE;
                memcpy(disk.items[index].id, job->item_id, sizeof(disk.items[index].id));
                ++disk.item_count;
            }
            disk.items[index].state = job->next_item;
            disk.version = 3;
        }
    }

    err = write_record(text, &disk);
    if (err == XIAOTAI_OK) mirror_from_disk(completion, err, &disk);
    else mirror_failure(completion, err);
    return err;
}

static xiaotai_err_t process_job(const xiaotai_recognition_journal_job_t *job,
                                 journal_completion_t *completion)
{
    int64_t scope = 0;
    xiaotai_err_t err = xiaotai_kv_get_int(JOURNAL_SPACE, JOURNAL_SCOPE, XIAOTAI_KV_U32,
                                          &scope, JOURNAL_KV_WAIT_MS);
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_NOT_FOUND) {
        mirror_failure(completion, err);
        return err;
    }
    if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_ADVANCE_SCOPE) {
        if (scope == UINT32_MAX) {
            completion->update.state = XIAOTAI_RECOGNITION_JOURNAL_UNREAD;
            return XIAOTAI_ERR_INVALID_STATE;
        }
        err = xiaotai_kv_set_int(JOURNAL_SPACE, JOURNAL_SCOPE, XIAOTAI_KV_U32, scope + 1,
                                JOURNAL_KV_WAIT_MS);
        if (err == XIAOTAI_OK) {
            completion->committed_scope = (uint32_t)(scope + 1);
            completion->update.state = XIAOTAI_RECOGNITION_JOURNAL_UNREAD;
        } else mirror_failure(completion, err);
        return err;
    }

    char *text = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, JOURNAL_BYTES);
    if (!text) {
        mirror_failure(completion, XIAOTAI_ERR_NO_MEM);
        return XIAOTAI_ERR_NO_MEM;
    }
    err = process_record_job(job, completion, text, (uint32_t)scope);
    xiaotai_mem_free(text);
    return err;
}

static void deliver(void *arg)
{
    journal_completion_t *completion = arg;
    const xiaotai_recognition_journal_mirror_t *previous = &journal.storage->mirror;
    /* The worker waits for this callback before taking another job. Only a
     * confirmed advance can retire the pending record last read by this owner. */
    if (completion->committed_scope != 0 && journal.pending == 1 &&
        previous->state == XIAOTAI_RECOGNITION_JOURNAL_PRESENT &&
        previous->record.scope == completion->committed_scope - 1U) {
        completion->update.state = XIAOTAI_RECOGNITION_JOURNAL_ABSENT;
    }
    journal.storage->mirror = completion->update;
    --journal.pending;
    if (completion->load) --journal.loads;
    completion->done(completion->result, completion->item, &completion->update, completion->ctx);
    xiaotai_signal_give(&journal.delivered);
}

static void worker(void *arg)
{
    journal_storage_t *storage = arg;
    xiaotai_recognition_journal_job_t job;
    for (;;) {
        if (!xiaotai_queue_receive(&journal.queue, &job, XIAOTAI_WAIT_FOREVER)) continue;
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
        if (!job.done) break;
#endif
        journal_completion_t *completion = &storage->completion;
        memset(completion, 0, sizeof(*completion));
        completion->done = job.done;
        completion->ctx = job.ctx;
        completion->load = job.operation == XIAOTAI_RECOGNITION_JOURNAL_LOAD;
        completion->result = process_job(&job, completion);
        /* Keep one completion until the owner takes it. A full owner queue
         * must not drop a storage result or reorder two mirror updates. */
        while (xiaotai_run(deliver, completion) != XIAOTAI_OK) xiaotai_delay_ms(1);
        (void)xiaotai_signal_take(&journal.delivered, XIAOTAI_WAIT_FOREVER);
    }
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
    xiaotai_signal_give(&journal.exited);
    xiaotai_task_exit();
#endif
}

xiaotai_err_t xiaotai_recognition_journal_start(void)
{
    if (journal.storage) return XIAOTAI_ERR_INVALID_STATE;
    journal_storage_t *storage = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*storage));
    if (!storage) return XIAOTAI_ERR_NO_MEM;
    memset(storage, 0, sizeof(*storage));
    xiaotai_err_t err = xiaotai_queue_init(&journal.queue, JOURNAL_QUEUE_DEPTH,
                                          sizeof(xiaotai_recognition_journal_job_t), XIAOTAI_MEM_BULK);
    if (err != XIAOTAI_OK) goto free_storage;
    err = xiaotai_signal_init(&journal.delivered);
    if (err != XIAOTAI_OK) goto deinit_queue;
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
    err = xiaotai_signal_init(&journal.exited);
    if (err != XIAOTAI_OK) goto deinit_delivery;
#endif
    const xiaotai_task_config_t config = {
        .name = "xt_rec_journal", .entry = worker, .arg = storage, .stack_bytes = 4096,
        .stack = XIAOTAI_MEM_BULK, .priority = 1, .core = XIAOTAI_CORE_ANY,
    };
    err = xiaotai_task_create(&config, &journal.worker);
    if (err == XIAOTAI_OK) {
        journal.storage = storage;
        return XIAOTAI_OK;
    }
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
    xiaotai_signal_deinit(&journal.exited);
deinit_delivery:
#endif
    xiaotai_signal_deinit(&journal.delivered);
deinit_queue:
    xiaotai_queue_deinit(&journal.queue);
free_storage:
    xiaotai_mem_free(storage);
    return err;
}

xiaotai_err_t xiaotai_recognition_journal_submit(const xiaotai_recognition_journal_job_t *job)
{
    if (!xiaotai_on_owner() || !journal.storage) return XIAOTAI_ERR_INVALID_STATE;
    if (!job || !job->done) return XIAOTAI_ERR_INVALID_ARG;
    switch (job->operation) {
    case XIAOTAI_RECOGNITION_JOURNAL_LOAD:
    case XIAOTAI_RECOGNITION_JOURNAL_ADVANCE_SCOPE:
        break;
    case XIAOTAI_RECOGNITION_JOURNAL_ITEM_TRANSITION:
        if (!item_edge(job->expected_item, job->next_item)) return XIAOTAI_ERR_INVALID_ARG;
        /* fall through */
    case XIAOTAI_RECOGNITION_JOURNAL_ITEM_READ:
        if (!identifier(job->item_id)) return XIAOTAI_ERR_INVALID_ARG;
        /* fall through */
    case XIAOTAI_RECOGNITION_JOURNAL_PREPARE:
    case XIAOTAI_RECOGNITION_JOURNAL_MARK_CONFLICT:
    case XIAOTAI_RECOGNITION_JOURNAL_CLEAR:
    case XIAOTAI_RECOGNITION_JOURNAL_CLEAR_CONFLICT:
        if (!record_valid(&job->record)) return XIAOTAI_ERR_INVALID_ARG;
        break;
    default:
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!xiaotai_queue_send(&journal.queue, job, 0)) return XIAOTAI_ERR_NO_MEM;
    ++journal.pending;
    if (job->operation == XIAOTAI_RECOGNITION_JOURNAL_LOAD) ++journal.loads;
    return XIAOTAI_OK;
}

const xiaotai_recognition_journal_mirror_t *xiaotai_recognition_journal_mirror(void)
{
    static const xiaotai_recognition_journal_mirror_t unread = {.state = XIAOTAI_RECOGNITION_JOURNAL_UNREAD};
    static const xiaotai_recognition_journal_mirror_t reading = {.state = XIAOTAI_RECOGNITION_JOURNAL_READING};
    if (!journal.storage) return &unread;
    if (journal.loads) return &reading;
    return &journal.storage->mirror;
}

bool xiaotai_recognition_journal_busy(void)
{
    return journal.pending != 0;
}

#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
void xiaotai_recognition_journal_host_stop(void)
{
    if (!journal.storage) return;
    const xiaotai_recognition_journal_job_t stop = {0};
    (void)xiaotai_queue_send(&journal.queue, &stop, XIAOTAI_WAIT_FOREVER);
    (void)xiaotai_signal_take(&journal.exited, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&journal.exited);
    xiaotai_signal_deinit(&journal.delivered);
    xiaotai_queue_deinit(&journal.queue);
    xiaotai_mem_free(journal.storage);
    journal.storage = NULL;
}
#endif
