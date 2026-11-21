// SPDX-License-Identifier: MIT
#pragma once
/* Helpers shared by the reader's translation units. Nothing here allocates,
 * locks or logs: every caller is [any, rt]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/json.h"

/* One step of a path ("data.list[2].id"): a member name or an array position. */
typedef struct {
    const char *key;     /* not terminated; key_length bytes, compared literally */
    size_t key_length;
    size_t position;
    bool is_position;
} xiaotai_json_segment_t;

/* Reads the next segment at *cursor and advances it: 1 a segment, 0 the end of
 * the path, -1 a malformed path (empty name, "[" without digits and "]", a
 * name directly after "]", a position that does not fit size_t). */
int xiaotai_json_path_next(const char **cursor, xiaotai_json_segment_t *segment);

/* Decodes raw string bytes (between the quotes, escapes as written) into out.
 * out may be NULL with capacity 0 to measure. XIAOTAI_ERR_INVALID_ARG for a
 * refused escape (malformed, a lone surrogate, \u0000), and then *length is
 * not set and out is untouched; XIAOTAI_ERR_INVALID_SIZE when the decoded
 * bytes and the terminator do not fit (out[0] is then '\0' when capacity > 0).
 * *length is the decoded length without the terminator. */
xiaotai_err_t xiaotai_json_decode(const char *raw, size_t raw_length, char *out, size_t capacity, size_t *length);

/* True when raw string bytes decode to exactly text[0, length). A refused
 * escape never compares equal. */
bool xiaotai_json_raw_equals(const char *raw, size_t raw_length, const char *text, size_t length);
