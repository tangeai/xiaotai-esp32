// SPDX-License-Identifier: MIT
#pragma once
/* A validated value borrowed from the caller's JSON text. Views are small,
 * have no owning state, and need no token table or allocation. The text must
 * stay alive and unchanged until its last view or iterator is used. Do not
 * construct or modify view fields except to initialize an absent view to zero.
 * The slice includes the value's quotes or brackets, but not surrounding
 * whitespace. Type 0 denotes absence, not a JSON null.
 *
 * Parsing validates the full document. Navigating a view scans the relevant
 * text, so use an iterator for one linear walk over a container. */
#include "xiaotai/json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *text;
    size_t length;
    xiaotai_json_type_t type;
} xiaotai_json_view_t;

typedef struct {
    xiaotai_json_view_t container;
    size_t next;
} xiaotai_json_view_iter_t;

/* Same validation and token count (including a partial count on error) as
 * xiaotai_json_parse() without a token table. count and root are optional.
 * An unsuccessful parse clears root. Callers may enforce their own count
 * limit after validation without preallocating storage for that many tokens. */
xiaotai_err_t xiaotai_json_view_parse(const char *text, size_t length, size_t *count,
                                      xiaotai_json_view_t *root);

/* First matching member after decoding its key; clears out if absent. */
bool xiaotai_json_view_get(const xiaotai_json_view_t *object, const char *key, xiaotai_json_view_t *out);

/* Iterate direct children in source order, including duplicate object keys.
 * Noncontainers are empty. key is optional and absent for array elements;
 * provided outputs are cleared at the end. key and value must not alias. */
void xiaotai_json_view_iter(const xiaotai_json_view_t *container, xiaotai_json_view_iter_t *iter);
bool xiaotai_json_view_next(xiaotai_json_view_iter_t *iter, xiaotai_json_view_t *key, xiaotai_json_view_t *value);

/* The same strict conversions and capacity rules as the token API. */
xiaotai_err_t xiaotai_json_view_int(const xiaotai_json_view_t *view, int64_t *value);
xiaotai_err_t xiaotai_json_view_bool(const xiaotai_json_view_t *view, bool *value);
xiaotai_err_t xiaotai_json_view_string(const xiaotai_json_view_t *view, char *out, size_t capacity, size_t *length);
bool xiaotai_json_view_equals(const xiaotai_json_view_t *view, const char *text);

#ifdef __cplusplus
}
#endif
