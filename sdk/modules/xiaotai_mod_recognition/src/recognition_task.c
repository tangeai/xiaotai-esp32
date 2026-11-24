// SPDX-License-Identifier: MIT
#include "recognition_json.h"
#include "xiaotai/port/mem.h"

#include <stdio.h>
#include <string.h>

static int read_enum(const rc_json_t *json, int index, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (rc_equals(json, index, names[i])) return (int)i;
    return -1;
}

int rc_task_state(const rc_json_t *json, int index)
{
    static const char *const states[] = {
        "queued", "processing", "succeeded", "partial_failed", "failed", "unknown", "cancelled",
    };
    return read_enum(json, index, states, sizeof(states) / sizeof(states[0]));
}

static bool read_failure(rc_json_t *json, int index, int32_t *out)
{
    *out = 0;
    if (rc_json_is(json, index, XIAOTAI_JSON_NULL)) return true;
    uint64_t code;
    if (!rc_json_unique(json, index) ||
        !rc_json_uint(json, rc_get(json, index, "code"), INT32_MAX, &code) ||
        !rc_json_is(json, rc_get(json, index, "msg"), XIAOTAI_JSON_STRING))
        return false;
    *out = (int32_t)code;
    return true;
}

static rc_span_t span(const rc_json_t *json, int index, uint32_t base)
{
    const xiaotai_json_token_t *token = &json->tokens[index];
    return (rc_span_t){.start = token->start - base, .size = token->end - token->start};
}

static bool read_text(rc_json_t *json, int result, uint32_t base, rc_item_t *out)
{
    if (!rc_json_unique(json, result)) return false;
    const int summary = rc_get(json, result, "summary");
    const int labels = rc_get(json, result, "labels");
    size_t summary_size;
    const char *text = rc_json_string(json, summary, XIAOTAI_RECOGNITION_SUMMARY_MAX - 1, &summary_size);
    if (!text || !summary_size || !rc_utf8(text, summary_size, 500) ||
        xiaotai_json_bool(&json->doc, rc_get(json, result, "alarm"), &out->alarm) != XIAOTAI_OK ||
        !rc_json_is(json, labels, XIAOTAI_JSON_ARRAY) || json->tokens[labels].children > 32 ||
        !rc_json_is(json, rc_get(json, result, "details"), XIAOTAI_JSON_OBJECT))
        return false;
    for (size_t i = 0; i < json->tokens[labels].children; ++i)
        if (!rc_json_is(json, xiaotai_json_at(&json->doc, labels, i), XIAOTAI_JSON_STRING)) return false;
    out->result = span(json, result, base);
    out->summary = span(json, summary, base);
    return true;
}

static bool ready_speech(rc_json_t *json, int speech, int id, const char *request)
{
    const char *item_id = rc_json_string(json, id, XIAOTAI_RECOGNITION_ID_MAX - 1, NULL);
    if (!item_id) return false;
    char expected[180];
    int n = snprintf(expected, sizeof(expected), "/v1/recognition/requests/%s/audio/%s", request, item_id);
    return n >= 0 && (size_t)n < sizeof(expected) &&
        rc_equals(json, rc_get(json, speech, "url"), expected) &&
        rc_equals(json, rc_get(json, speech, "format"), "wav");
}

static bool read_item(rc_json_t *json, int item, const char *request, size_t order, uint32_t base, rc_item_t *out)
{
    static const char *const outputs[] = {"text", "audio", "text_audio"};
    static const char *const states[] = {"queued", "processing", "succeeded", "failed", "unknown", "cancelled"};
    static const char *const speeches[] = {"pending", "synthesizing", "ready", "failed", "unknown", "skipped"};
    if (!rc_json_unique(json, item)) return false;
    const int id = rc_get(json, item, "item_id");
    const int speech = rc_get(json, item, "speech");
    const int result = rc_get(json, item, "result");
    const int scheme = rc_get(json, item, "scheme_id");
    const int output = read_enum(json, rc_get(json, item, "output_mode"), outputs, 3);
    const int state = read_enum(json, rc_get(json, item, "status"), states, 6);
    const int voice = read_enum(json, rc_get(json, speech, "status"), speeches, 6);
    uint64_t n, version;
    if (!rc_id_valid(rc_json_string(json, id, XIAOTAI_RECOGNITION_ID_MAX - 1, NULL)) ||
        !rc_json_is(json, scheme, XIAOTAI_JSON_STRING) ||
        json->tokens[scheme].start == json->tokens[scheme].end ||
        !rc_json_uint(json, rc_get(json, item, "order"), XIAOTAI_RECOGNITION_ITEMS_MAX, &n) || n != order ||
        !rc_json_uint(json, rc_get(json, item, "scheme_version"), XIAOTAI_RECOGNITION_REVISION_MAX, &version) ||
        !version || output < 0 || state < 0 || voice < 0 || !rc_json_unique(json, speech) ||
        !read_failure(json, rc_get(json, item, "error"), &out->error_code) ||
        !read_failure(json, rc_get(json, speech, "error"), &out->speech_error_code))
        return false;
    if (output == XIAOTAI_RECOGNITION_OUTPUT_AUDIO || state != XIAOTAI_RECOGNITION_ITEM_SUCCEEDED) {
        if (!rc_json_is(json, result, XIAOTAI_JSON_NULL)) return false;
    } else if (!read_text(json, result, base, out)) {
        return false;
    }
    if (output == XIAOTAI_RECOGNITION_OUTPUT_TEXT && voice != XIAOTAI_RECOGNITION_SPEECH_SKIPPED) return false;
    if (voice == XIAOTAI_RECOGNITION_SPEECH_READY &&
        (state != XIAOTAI_RECOGNITION_ITEM_SUCCEEDED || !ready_speech(json, speech, id, request)))
        return false;
    out->id = span(json, id, base);
    out->state = (uint8_t)state;
    out->output = (uint8_t)output;
    out->speech = (uint8_t)voice;
    return true;
}

static xiaotai_err_t read_task(rc_json_t *json, int envelope, int http, const char *device,
                               const char *request, int *code, rc_task_t **out)
{
    if (!rc_json_unique(json, envelope) || !rc_json_code(json, envelope, false, code))
        return json->error != XIAOTAI_OK ? json->error : RC_INVALID_RESPONSE;
    const int data = rc_get(json, envelope, "data");
    const int items = rc_get(json, data, "results");
    const int state = rc_task_state(json, rc_get(json, data, "status"));
    uint64_t revision;
    int32_t ignored;
    if (http != 200 || *code != 200 || !rc_json_unique(json, data) ||
        strlen(device) >= XIAOTAI_RECOGNITION_ID_MAX || !rc_equals(json, rc_get(json, data, "device_id"), device) ||
        !rc_equals(json, rc_get(json, data, "request_id"), request) || state < 0 ||
        !rc_json_uint(json, rc_get(json, data, "status_revision"), XIAOTAI_RECOGNITION_REVISION_MAX, &revision) ||
        !revision || !read_failure(json, rc_get(json, data, "error"), &ignored) ||
        !rc_json_is(json, items, XIAOTAI_JSON_ARRAY) || !json->tokens[items].children ||
        json->tokens[items].children > XIAOTAI_RECOGNITION_ITEMS_MAX)
        return json->error != XIAOTAI_OK ? json->error : RC_INVALID_RESPONSE;

    rc_task_t *task = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*task));
    if (!task) return XIAOTAI_ERR_NO_MEM;
    task->info.revision = revision;
    task->info.state = (xiaotai_recognition_task_state_t)state;
    task->info.items = (uint8_t)json->tokens[items].children;
    strcpy(task->info.request_id, request);
    uint32_t base = json->tokens[envelope].start;
    int ids[XIAOTAI_RECOGNITION_ITEMS_MAX];
    for (size_t i = 0; i < task->info.items; ++i) {
        const int item = xiaotai_json_at(&json->doc, items, i);
        if (!read_item(json, item, request, i + 1, base, &task->items[i])) goto invalid;
        ids[i] = rc_get(json, item, "item_id");
        for (size_t j = 0; j < i; ++j)
            if (rc_json_same(json, ids[i], ids[j]) || json->error != XIAOTAI_OK) goto invalid;
    }
    task->length = json->tokens[envelope].end - base;
    const char *payload = json->doc.text + base;
    rc_json_close(json);  /* Retained text does not compete with the token table. */
    if (rc_bulk_fits(task->length + 1)) task->body = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, task->length + 1);
    if (!task->body) {
        rc_task_free(task);
        return XIAOTAI_ERR_NO_MEM;
    }
    memcpy(task->body, payload, task->length);
    task->body[task->length] = '\0';
    *out = task;
    return XIAOTAI_OK;

invalid:
    rc_task_free(task);
    return json->error != XIAOTAI_OK ? json->error : RC_INVALID_RESPONSE;
}

xiaotai_err_t rc_task_decode(const char *body, size_t length, int http,
                             const char *device_id, const char *request_id, int *code, rc_task_t **task)
{
    if (!code || !task || !device_id || !device_id[0] || !rc_id_valid(request_id)) return XIAOTAI_ERR_INVALID_ARG;
    *code = -1;
    *task = NULL;
    rc_json_t json;
    xiaotai_err_t error = rc_json_open(&json, body, length, RC_TASK_BYTES);
    if (error != XIAOTAI_OK) return error;
    error = read_task(&json, 0, http, device_id, request_id, code, task);
    rc_json_close(&json);
    return error;
}

xiaotai_err_t rc_notification_decode(const char *body, size_t length, const char *device_id,
                                     const char *request_id, uint64_t *revision, rc_task_t **task)
{
    if (!body || !length || !device_id || !device_id[0] || !revision || !task) return XIAOTAI_ERR_INVALID_ARG;
    *revision = 0;
    *task = NULL;
    if (length > RC_TASK_BYTES) return RC_INVALID_RESPONSE;
    rc_json_t json;
    xiaotai_err_t error = rc_json_open(&json, body, length, RC_TASK_BYTES);
    if (error != XIAOTAI_OK) return error;
    const int type = rc_get(&json, 0, "type");
    const int event = rc_get(&json, 0, "event_id");
    const int payload = rc_get(&json, 0, "payload");
    const int data = rc_get(&json, payload, "data");
    const int device = rc_get(&json, data, "device_id");
    int code = -1;
    error = RC_INVALID_RESPONSE;
    if (!rc_json_unique(&json, 0) || !rc_json_is(&json, type, XIAOTAI_JSON_STRING) ||
        !rc_equals(&json, rc_get(&json, 0, "channel"), "recognition") ||
        !rc_json_is(&json, event, XIAOTAI_JSON_STRING) || json.tokens[event].start == json.tokens[event].end ||
        !rc_json_unique(&json, payload) || !rc_json_unique(&json, data) ||
        !rc_json_code(&json, payload, false, &code) || code != 200 ||
        !rc_json_is(&json, device, XIAOTAI_JSON_STRING))
        goto done;
    if (!rc_equals(&json, device, device_id)) {
        error = XIAOTAI_ERR_NOT_FOUND;
    } else if (rc_equals(&json, type, "recognition_config_changed")) {
        if (rc_json_uint(&json, rc_get(&json, data, "revision"), XIAOTAI_RECOGNITION_REVISION_MAX, revision) &&
            *revision) error = XIAOTAI_OK;
    } else if (rc_equals(&json, type, "recognition_result")) {
        const int id = rc_get(&json, data, "request_id");
        if (!rc_json_is(&json, id, XIAOTAI_JSON_STRING)) goto done;
        if (!request_id || !*request_id || !rc_equals(&json, id, request_id)) error = XIAOTAI_ERR_NOT_FOUND;
        else if (!rc_id_valid(request_id)) error = XIAOTAI_ERR_INVALID_ARG;
        else error = read_task(&json, payload, 200, device_id, request_id, &code, task);
    }

done:
    if (json.error != XIAOTAI_OK) error = json.error;
    rc_json_close(&json);
    return error;
}

void rc_task_free(rc_task_t *task)
{
    if (!task) return;
    xiaotai_mem_free(task->body);
    xiaotai_mem_free(task);
}

xiaotai_err_t rc_task_string(const rc_task_t *task, rc_span_t range, char *out, size_t capacity, size_t *size)
{
    if (!task || !task->body || range.start > task->length || range.size > task->length - range.start)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!range.size) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_json_token_t string = {
        .type = XIAOTAI_JSON_STRING, .start = range.start, .end = range.start + range.size,
    };
    const xiaotai_json_doc_t view = {.text = task->body, .length = task->length, .tokens = &string, .count = 1};
    return xiaotai_json_string(&view, 0, out, capacity, size);
}
