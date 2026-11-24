// SPDX-License-Identifier: MIT
#include "xiaotai/detail/recognition_json.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/port/mem.h"

#include <limits.h>
#include <string.h>

static bool memory_fits(size_t size, size_t reserve)
{
    xiaotai_mem_stats_t stats;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &stats);
    /* Some ports only report allocator failures, not heap watermarks. */
    return !stats.total || (stats.free >= reserve && size <= stats.free - reserve && size <= stats.largest_block);
}

bool rc_bulk_fits(size_t size)
{
    return memory_fits(size, RC_BULK_RESERVE);
}

bool rc_utf8(const char *text, size_t size, size_t limit)
{
    const uint8_t *next = (const uint8_t *)text;
    const uint8_t *end = next + size;
    size_t count = 0;
    while (next != end) {
        uint32_t point = *next++;
        unsigned tail = 0;
        uint32_t minimum = 0;
        if (point >= 0xc2 && point <= 0xdf) { point &= 0x1f; tail = 1; minimum = 0x80; }
        else if (point >= 0xe0 && point <= 0xef) { point &= 0x0f; tail = 2; minimum = 0x800; }
        else if (point >= 0xf0 && point <= 0xf4) { point &= 7; tail = 3; minimum = 0x10000; }
        else if (!point || point >= 0x80) return false;
        if ((size_t)(end - next) < tail || ++count > limit) return false;
        for (unsigned j = 0; j < tail; ++j) {
            if ((*next & 0xc0) != 0x80) return false;
            point = (point << 6) | (*next++ & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
}

void rc_json_close(rc_json_t *json)
{
    xiaotai_mem_free(json->tokens);
    xiaotai_mem_free(json->scratch);
    *json = (rc_json_t){0};
}

xiaotai_err_t rc_json_open(rc_json_t *json, const char *body, size_t length, size_t limit)
{
    return rc_json_open_reserved(json, body, length, limit, RC_BULK_RESERVE);
}

xiaotai_err_t rc_json_open_reserved(rc_json_t *json, const char *body, size_t length, size_t limit, size_t reserve)
{
    *json = (rc_json_t){.reserve = reserve};
    if (!body) return XIAOTAI_FAIL;
    if (length > limit) return XIAOTAI_ERR_INVALID_SIZE;
    size_t count = 0;
    if (memchr(body, 0, length) ||
        xiaotai_json_parse(body, length, NULL, 0, &count, NULL) != XIAOTAI_OK || !count)
        return XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE;
    size_t bytes = count * sizeof(*json->tokens);
    if (!memory_fits(bytes, reserve)) return XIAOTAI_ERR_NO_MEM;
    json->tokens = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (!json->tokens) return XIAOTAI_ERR_NO_MEM;
    if (xiaotai_json_parse(body, length, json->tokens, count, &count, &json->doc) != XIAOTAI_OK) {
        rc_json_close(json);
        return XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!rc_json_is(json, (int)i, XIAOTAI_JSON_STRING)) continue;
        size_t raw_size;
        const char *raw = xiaotai_json_raw(&json->doc, (int)i, &raw_size);
        if (!rc_utf8(raw, raw_size, SIZE_MAX) ||
            xiaotai_json_string(&json->doc, (int)i, NULL, 0, NULL) != XIAOTAI_ERR_INVALID_SIZE) {
            rc_json_close(json);
            return XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE;
        }
    }
    return XIAOTAI_OK;
}

bool rc_json_is(const rc_json_t *json, int index, xiaotai_json_type_t type)
{
    return index >= 0 && (size_t)index < json->doc.count && json->tokens[index].type == type;
}

bool rc_json_uint(const rc_json_t *json, int index, uint64_t max, uint64_t *out)
{
    int64_t val;
    if (xiaotai_json_int(&json->doc, index, &val) != XIAOTAI_OK || val < 0 || (uint64_t)val > max) return false;
    *out = (uint64_t)val;
    return true;
}

const char *rc_json_string(rc_json_t *json, int index, size_t max, size_t *size)
{
    size_t length;
    if (xiaotai_json_string(&json->doc, index, NULL, 0, &length) != XIAOTAI_ERR_INVALID_SIZE ||
        length > max) return NULL;
    char *dest = json->small;
    if (length >= sizeof(json->small)) {
        if (json->scratch_size <= length) {
            xiaotai_mem_free(json->scratch);
            json->scratch = NULL;
            json->scratch_size = 0;
            if (memory_fits(length + 1, json->reserve))
                json->scratch = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, length + 1);
            if (!json->scratch) {
                json->error = XIAOTAI_ERR_NO_MEM;
                return NULL;
            }
            json->scratch_size = length + 1;
        }
        dest = json->scratch;
    }
    if (xiaotai_json_string(&json->doc, index, dest, length + 1, NULL) != XIAOTAI_OK) return NULL;
    if (size) *size = length;
    return dest;
}

bool rc_json_same(rc_json_t *json, int a, int b)
{
    if (!rc_json_is(json, a, XIAOTAI_JSON_STRING) || !rc_json_is(json, b, XIAOTAI_JSON_STRING)) return false;
    size_t al, bl;
    const char *ar = xiaotai_json_raw(&json->doc, a, &al);
    const char *br = xiaotai_json_raw(&json->doc, b, &bl);
    if (al == bl && !memcmp(ar, br, al)) return true;
    if (!memchr(ar, '\\', al) && !memchr(br, '\\', bl)) return false;
    const char *decoded = rc_json_string(json, al <= bl ? a : b, SIZE_MAX, NULL);
    return decoded && rc_equals(json, al <= bl ? b : a, decoded);
}

static size_t next_key(const rc_json_t *json, size_t key)
{
    const uint32_t end = json->tokens[key + 1].end;
    size_t i = key + 2;
    while (i < json->doc.count && json->tokens[i].start < end) ++i;
    return i;
}

bool rc_json_unique(rc_json_t *json, int object)
{
    if (!rc_json_is(json, object, XIAOTAI_JSON_OBJECT) || json->tokens[object].children > 32) return false;
    int keys[32];
    unsigned count = 0;
    for (size_t key = (size_t)object + 1; key < json->doc.count &&
         json->tokens[key].start < json->tokens[object].end; key = next_key(json, key)) {
        if (count == sizeof(keys) / sizeof(keys[0]) || key + 1 == json->doc.count) return false;
        for (unsigned i = 0; i < count; ++i)
            if (rc_json_same(json, keys[i], (int)key) || json->error != XIAOTAI_OK) return false;
        keys[count++] = (int)key;
    }
    return true;
}

bool rc_json_code(rc_json_t *json, int object, bool unique_code, int *code)
{
    uint64_t value;
    if (!rc_json_uint(json, rc_get(json, object, "code"), INT32_MAX, &value)) return false;
    if (unique_code) {
        unsigned count = 0;
        for (size_t key = (size_t)object + 1; key < json->doc.count &&
             json->tokens[key].start < json->tokens[object].end; key = next_key(json, key)) {
            if (rc_equals(json, (int)key, "code") && ++count > 1) return false;
        }
    }
    *code = (int)value;
    return true;
}
