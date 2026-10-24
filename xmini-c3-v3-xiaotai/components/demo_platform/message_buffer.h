#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Single-owner task buffer. Observers must copy data before returning. */
typedef struct {
    char *data;
    size_t capacity;
    size_t used;
} message_buffer_t;

bool message_buffer_begin(message_buffer_t *buffer, size_t capacity);
bool message_buffer_append(message_buffer_t *buffer, size_t offset, const void *data, size_t size);
void message_buffer_release(message_buffer_t *buffer);
