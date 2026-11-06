#include "message_buffer.h"
#include <stdlib.h>
#include <string.h>

void message_buffer_release(message_buffer_t *buffer)
{
    if (buffer->data) {
        /* Control responses may contain credentials; do not optimize this away. */
        volatile unsigned char *p = (volatile unsigned char *)buffer->data;
        for (size_t i = 0; i < buffer->capacity; ++i)
            p[i] = 0;
        free(buffer->data);
    }
    *buffer = (message_buffer_t){0};
}

bool message_buffer_begin(message_buffer_t *buffer, size_t capacity)
{
    message_buffer_release(buffer);
    if (capacity == 0)
        return false;
    buffer->data = calloc(capacity, 1);
    if (!buffer->data)
        return false;
    buffer->capacity = capacity;
    return true;
}

bool message_buffer_append(message_buffer_t *buffer, size_t offset, const void *data, size_t size)
{
    if (!buffer->data || offset != buffer->used || buffer->used >= buffer->capacity ||
        size >= buffer->capacity - buffer->used || (size && !data))
        return false;
    if (size)
        memcpy(buffer->data + buffer->used, data, size);
    buffer->used += size;
    buffer->data[buffer->used] = 0;
    return true;
}
