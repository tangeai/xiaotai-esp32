// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include "xiaotai/modules/recognition.h"

/* The result an item came with ({"summary","labels","details"}) as lines of
 * words for a person: the summary, the labels, then one line for each detail.
 * Cut at a character boundary. */
xiaotai_err_t companion_recognition_details(const xiaotai_recognition_result_t *result, size_t item,
                                            char *out, size_t capacity);
void companion_recognition_format(const char *json, size_t length, char *out, size_t capacity);
