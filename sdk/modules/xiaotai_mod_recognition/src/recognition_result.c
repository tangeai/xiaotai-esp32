// SPDX-License-Identifier: MIT
#include "recognition_result.h"

#include "xiaotai/identity.h"
#include "xiaotai/port/mem.h"

#include <stdatomic.h>
#include <string.h>

typedef struct {
    atomic_uint refs;
    rc_task_t *task;
    xiaotai_recognition_journal_record_t record;
} result_document_t;

struct xiaotai_recognition_result {
    atomic_uint refs;
    result_document_t *document;
    uint32_t generation;
    uint8_t consumption[XIAOTAI_RECOGNITION_ITEMS_MAX];
};

static xiaotai_recognition_result_t *owner_view, *published;
static atomic_flag view_lock = ATOMIC_FLAG_INIT;
static atomic_bool dirty;
static bool view_confirmed;

static void read_consumption(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                             uint8_t out[XIAOTAI_RECOGNITION_ITEMS_MAX])
{
    memset(out, 0, XIAOTAI_RECOGNITION_ITEMS_MAX);
    for (size_t i = 0; i < task->info.items; ++i) {
        char id[XIAOTAI_RECOGNITION_ID_MAX];
        if (rc_task_string(task, task->items[i].id, id, sizeof(id), NULL) == XIAOTAI_OK)
            out[i] = (uint8_t)rc_item_consumption(mirror, id);
    }
}

static void retain(xiaotai_recognition_result_t *view)
{
    if (view) atomic_fetch_add_explicit(&view->refs, 1U, memory_order_relaxed);
}

void xiaotai_recognition_result_release(xiaotai_recognition_result_t *view)
{
    if (!view || atomic_fetch_sub_explicit(&view->refs, 1U, memory_order_acq_rel) != 1U) return;
    result_document_t *const doc = view->document;
    xiaotai_mem_free(view);
    if (atomic_fetch_sub_explicit(&doc->refs, 1U, memory_order_acq_rel) == 1U) {
        rc_task_free(doc->task);
        xiaotai_mem_free(doc);
    }
}

void rc_result_flush(void)
{
    if (!atomic_load_explicit(&dirty, memory_order_acquire) || !view_confirmed ||
        atomic_flag_test_and_set_explicit(&view_lock, memory_order_acquire)) return;
    xiaotai_recognition_result_t *old = published;
    published = owner_view;
    retain(published);
    atomic_store_explicit(&dirty, false, memory_order_release);
    atomic_flag_clear_explicit(&view_lock, memory_order_release);
    xiaotai_recognition_result_release(old);
}

static void set_view(xiaotai_recognition_result_t *view, bool confirmed)
{
    xiaotai_recognition_result_t *old = owner_view;
    atomic_store_explicit(&dirty, true, memory_order_release);
    owner_view = view;
    view_confirmed = confirmed;
    rc_result_flush();
    xiaotai_recognition_result_release(old);
}

xiaotai_err_t rc_result_adopt(rc_task_t *task, uint32_t generation,
                             const xiaotai_recognition_journal_record_t *record,
                             const xiaotai_recognition_journal_mirror_t *mirror)
{
    result_document_t *const doc = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*doc));
    if (!doc) return XIAOTAI_ERR_NO_MEM;
    xiaotai_recognition_result_t *const view = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*view));
    if (!view) {
        xiaotai_mem_free(doc);
        return XIAOTAI_ERR_NO_MEM;
    }
    doc->task = task;
    doc->record = *record;
    atomic_init(&doc->refs, 1U);
    view->document = doc;
    view->generation = generation;
    atomic_init(&view->refs, 1U);
    const bool matches = rc_record_matches(mirror, record);
    if (matches) read_consumption(task, mirror, view->consumption);
    else memset(view->consumption, 0, sizeof(view->consumption));
    set_view(view, matches);
    return XIAOTAI_OK;
}

void rc_result_update(const xiaotai_recognition_journal_mirror_t *mirror)
{
    if (!owner_view) return;
    if (!rc_record_matches(mirror, &owner_view->document->record)) {
        atomic_store_explicit(&dirty, true, memory_order_release);
        view_confirmed = false;
        return;
    }
    uint8_t consumption[XIAOTAI_RECOGNITION_ITEMS_MAX];
    read_consumption(owner_view->document->task, mirror, consumption);
    if (!memcmp(consumption, owner_view->consumption, sizeof(consumption))) {
        view_confirmed = true;
        rc_result_flush();
        return;
    }
    xiaotai_recognition_result_t *const view = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*view));
    if (!view) {
        atomic_store_explicit(&dirty, true, memory_order_release);
        view_confirmed = false;
        return;
    }
    view->document = owner_view->document;
    atomic_fetch_add_explicit(&view->document->refs, 1U, memory_order_relaxed);
    view->generation = owner_view->generation;
    atomic_init(&view->refs, 1U);
    memcpy(view->consumption, consumption, sizeof(consumption));
    set_view(view, true);
}

void rc_result_clear(void) { set_view(NULL, true); }

xiaotai_err_t xiaotai_recognition_result_acquire(xiaotai_recognition_result_t **result)
{
    if (!result) return XIAOTAI_ERR_INVALID_ARG;
    *result = NULL;
    if (atomic_flag_test_and_set_explicit(&view_lock, memory_order_acquire)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_recognition_result_t *view = published;
    const bool updating = atomic_load_explicit(&dirty, memory_order_acquire);
    if (!updating) retain(view);
    atomic_flag_clear_explicit(&view_lock, memory_order_release);
    if (updating) return XIAOTAI_ERR_TIMEOUT;
    if (!view) return XIAOTAI_ERR_NOT_FOUND;
    if (view->generation != xiaotai_identity_generation()) {
        xiaotai_recognition_result_release(view);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    *result = view;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_info(const xiaotai_recognition_result_t *view,
                                              xiaotai_recognition_result_info_t *info)
{
    if (!view || !info) return XIAOTAI_ERR_INVALID_ARG;
    if (view->generation != xiaotai_identity_generation()) return XIAOTAI_ERR_INVALID_STATE;
    *info = view->document->task->info;
    return XIAOTAI_OK;
}

static xiaotai_err_t check_item(const xiaotai_recognition_result_t *view, size_t index)
{
    if (!view || index >= view->document->task->info.items) return XIAOTAI_ERR_INVALID_ARG;
    return view->generation == xiaotai_identity_generation() ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_recognition_result_item(const xiaotai_recognition_result_t *view, size_t index,
                                              xiaotai_recognition_item_t *item)
{
    if (!item) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t error = check_item(view, index);
    if (error != XIAOTAI_OK) return error;
    const rc_task_t *task = view->document->task;
    const rc_item_t *entry = &task->items[index];
    *item = (xiaotai_recognition_item_t){.state = entry->state, .output = entry->output, .speech = entry->speech,
        .error_code = entry->error_code, .speech_error_code = entry->speech_error_code,
        .has_text = entry->result.size != 0, .alarm = entry->alarm, .consumption = view->consumption[index]};
    return rc_task_string(task, entry->id, item->item_id, sizeof(item->item_id), NULL);
}

xiaotai_err_t xiaotai_recognition_result_summary(const xiaotai_recognition_result_t *view, size_t index,
                                                 char *out, size_t capacity, size_t *size)
{
    if (size) *size = 0;
    const xiaotai_err_t error = check_item(view, index);
    if (error != XIAOTAI_OK) return error;
    const rc_task_t *task = view->document->task;
    return rc_task_string(task, task->items[index].summary, out, capacity, size);
}

xiaotai_err_t xiaotai_recognition_result_detail(const xiaotai_recognition_result_t *view, size_t index,
                                                char *out, size_t capacity, size_t *size)
{
    if (size) *size = 0;
    const xiaotai_err_t error = check_item(view, index);
    if (error != XIAOTAI_OK) return error;
    const rc_task_t *task = view->document->task;
    const rc_span_t span = task->items[index].result;
    if (!span.size) return XIAOTAI_ERR_NOT_FOUND;
    if (size) *size = span.size;
    if (!out && capacity) return XIAOTAI_ERR_INVALID_ARG;
    if (capacity <= span.size) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, task->body + span.start, span.size);
    out[span.size] = '\0';
    return XIAOTAI_OK;
}

static bool continues_utf8(char byte)
{
    return ((unsigned char)byte & 0xc0U) == 0x80U;
}

static size_t detail_page_end(const char *text, size_t size, size_t offset, size_t capacity)
{
    if (size - offset < capacity) return size;
    size_t end = offset + capacity - 1U;
    while (end > offset && continues_utf8(text[end])) --end;
    return end;
}

xiaotai_err_t xiaotai_recognition_result_detail_page(const xiaotai_recognition_result_t *view,
                                                     size_t index, size_t offset, char *out,
                                                     size_t capacity, size_t *next, size_t *previous,
                                                     size_t *total)
{
    if (!out || capacity < 5U || !next || !total) return XIAOTAI_ERR_INVALID_ARG;
    *next = *total = 0;
    if (previous) *previous = 0;
    out[0] = '\0';
    const xiaotai_err_t error = check_item(view, index);
    if (error != XIAOTAI_OK) return error;
    const rc_task_t *task = view->document->task;
    const rc_span_t span = task->items[index].result;
    if (!span.size) return XIAOTAI_ERR_NOT_FOUND;
    const char *text = task->body + span.start;
    if (offset >= span.size || continues_utf8(text[offset])) return XIAOTAI_ERR_INVALID_ARG;
    const size_t end = detail_page_end(text, span.size, offset, capacity);
    if (end <= offset) return XIAOTAI_ERR_INVALID_SIZE;
    if (previous) {
        // Visit boundaries without copying any preceding page.
        for (size_t start = 0; start < offset;) {
            const size_t limit = detail_page_end(text, span.size, start, capacity);
            if (limit <= start || limit > offset) return XIAOTAI_ERR_INVALID_ARG;
            *previous = start;
            start = limit;
        }
    }
    memcpy(out, text + offset, end - offset);
    out[end - offset] = '\0';
    *next = end;
    *total = span.size;
    return XIAOTAI_OK;
}
