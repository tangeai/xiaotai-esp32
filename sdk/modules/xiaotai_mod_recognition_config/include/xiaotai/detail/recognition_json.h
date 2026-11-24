// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared by CONFIG and its full recognition consumer; not a public contract. */
#define RC_BULK_RESERVE (256U * 1024U)

typedef struct {
    xiaotai_json_doc_t doc;
    xiaotai_json_token_t *tokens;
    char *scratch;
    size_t scratch_size, reserve;
    xiaotai_err_t error;
    char small[96];
} rc_json_t;

xiaotai_err_t rc_json_open(rc_json_t *json, const char *body, size_t length, size_t limit);
xiaotai_err_t rc_json_open_reserved(rc_json_t *json, const char *body, size_t length, size_t limit, size_t reserve);
void rc_json_close(rc_json_t *json);
bool rc_json_is(const rc_json_t *json, int index, xiaotai_json_type_t type);
bool rc_json_uint(const rc_json_t *json, int index, uint64_t max, uint64_t *out);
bool rc_json_unique(rc_json_t *json, int object);
bool rc_json_code(rc_json_t *json, int object, bool unique_code, int *code);
bool rc_json_same(rc_json_t *json, int a, int b);
const char *rc_json_string(rc_json_t *json, int index, size_t max, size_t *size);
bool rc_utf8(const char *text, size_t size, size_t limit);
bool rc_bulk_fits(size_t size);

static inline int rc_get(const rc_json_t *json, int object, const char *name)
{
    return xiaotai_json_get(&json->doc, object, name);
}

static inline bool rc_equals(const rc_json_t *json, int index, const char *text)
{
    return xiaotai_json_equals(&json->doc, index, text);
}

#ifdef __cplusplus
}
#endif
