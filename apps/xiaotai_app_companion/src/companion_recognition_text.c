// SPDX-License-Identifier: MIT
#include "companion_recognition_text.h"

#include <string.h>
#include "xiaotai/json_view.h"
#include "xiaotai/port/mem.h"

#define VALUE_BYTES 256U
#define DEPTH_MAX 4U

typedef struct {
    char *out;
    size_t used, capacity;
    char value[VALUE_BYTES];
} writer_t;

static void put(writer_t *w, const char *text, size_t length)
{
    size_t room = w->capacity - 1U - w->used;
    if (length > room) {
        length = room;
        while (length && ((unsigned char)text[length] & 0xc0U) == 0x80U) --length;
    }
    memcpy(w->out + w->used, text, length);
    w->used += length;
    w->out[w->used] = '\0';
}

static void puts_text(writer_t *w, const char *text) { put(w, text, strlen(text)); }

static void scalar(writer_t *w, const xiaotai_json_view_t *view)
{
    if (view->type != XIAOTAI_JSON_STRING) {
        bool truth;
        if (xiaotai_json_view_bool(view, &truth) == XIAOTAI_OK) puts_text(w, truth ? "是" : "否");
        else put(w, view->text, view->length);
        return;
    }
    if (xiaotai_json_view_string(view, w->value, sizeof(w->value), NULL) == XIAOTAI_OK) puts_text(w, w->value);
    else put(w, view->text + 1, view->length > 2U ? view->length - 2U : 0);     /* longer than a line: as it was written */
}

static bool plain_list(const xiaotai_json_view_t *list)
{
    xiaotai_json_view_iter_t it;
    xiaotai_json_view_t value;
    xiaotai_json_view_iter(list, &it);
    while (xiaotai_json_view_next(&it, NULL, &value))
        if (value.type == XIAOTAI_JSON_OBJECT || value.type == XIAOTAI_JSON_ARRAY) return false;
    return true;
}

static void members(writer_t *w, const xiaotai_json_view_t *container, unsigned depth);

static void value_of(writer_t *w, const xiaotai_json_view_t *value, unsigned depth)
{
    if (value->type == XIAOTAI_JSON_ARRAY && plain_list(value)) {
        xiaotai_json_view_iter_t it;
        xiaotai_json_view_t entry;
        bool first = true;
        xiaotai_json_view_iter(value, &it);
        while (xiaotai_json_view_next(&it, NULL, &entry)) {
            if (!first) puts_text(w, "、");
            first = false;
            scalar(w, &entry);
        }
        puts_text(w, "\n");
    } else if (value->type == XIAOTAI_JSON_OBJECT || value->type == XIAOTAI_JSON_ARRAY) {
        puts_text(w, "\n");
        if (depth < DEPTH_MAX) members(w, value, depth + 1U);
    } else {
        scalar(w, value);
        puts_text(w, "\n");
    }
}

static void members(writer_t *w, const xiaotai_json_view_t *container, unsigned depth)
{
    xiaotai_json_view_iter_t it;
    xiaotai_json_view_t key, value;
    xiaotai_json_view_iter(container, &it);
    while (xiaotai_json_view_next(&it, &key, &value)) {
        if (value.type == XIAOTAI_JSON_NULL) continue;
        for (unsigned i = 0; i < depth; ++i) puts_text(w, "  ");
        if (container->type == XIAOTAI_JSON_OBJECT) {
            scalar(w, &key);
            puts_text(w, "：");
        } else {
            puts_text(w, "· ");
        }
        value_of(w, &value, depth);
    }
}

void companion_recognition_format(const char *json, size_t length, char *out, size_t capacity)
{
    writer_t w = {.out = out, .capacity = capacity};
    xiaotai_json_view_t root, part;
    out[0] = '\0';
    if (xiaotai_json_view_parse(json, length, NULL, &root) != XIAOTAI_OK || root.type != XIAOTAI_JSON_OBJECT) {
        put(&w, json, length);
        return;
    }
    if (xiaotai_json_view_get(&root, "summary", &part) && part.type == XIAOTAI_JSON_STRING && part.length > 2U) {
        scalar(&w, &part);
        puts_text(&w, "\n");
    }
    if (xiaotai_json_view_get(&root, "labels", &part) && part.type == XIAOTAI_JSON_ARRAY && part.length > 2U) {
        puts_text(&w, "标签：");
        value_of(&w, &part, 0);
    }
    if (xiaotai_json_view_get(&root, "details", &part)) {
        if (part.type == XIAOTAI_JSON_OBJECT || part.type == XIAOTAI_JSON_ARRAY) members(&w, &part, 0);
        else if (part.type == XIAOTAI_JSON_STRING) scalar(&w, &part);
    }
    while (w.used && out[w.used - 1U] == '\n') out[--w.used] = '\0';
    if (!w.used) puts_text(&w, "没有更多内容。");
}

xiaotai_err_t companion_recognition_details(const xiaotai_recognition_result_t *result, size_t item, char *out, size_t capacity)
{
    size_t size = 0;
    (void)xiaotai_recognition_result_detail(result, item, NULL, 0, &size);
    if (!size) return XIAOTAI_ERR_NOT_FOUND;
    char *json = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size + 1U);
    if (!json) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t error = xiaotai_recognition_result_detail(result, item, json, size + 1U, &size);
    if (error == XIAOTAI_OK) companion_recognition_format(json, size, out, capacity);
    xiaotai_mem_free(json);
    return error;
}
