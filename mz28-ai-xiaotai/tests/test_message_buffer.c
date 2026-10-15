#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_next;
static size_t live, allocation_size;
static void *allocation;

static void *test_calloc(size_t count, size_t size)
{
    assert(!live);
    if (fail_next) {
        fail_next = 0;
        return NULL;
    }
    allocation = calloc(count, size);
    assert(allocation);
    allocation_size = count * size;
    ++live;
    return allocation;
}

static void test_free(void *ptr)
{
    assert(live == 1 && ptr == allocation);
    for (size_t i = 0; i < allocation_size; ++i)
        assert(((unsigned char *)ptr)[i] == 0);
    free(ptr);
    allocation = NULL;
    --live;
}

#define calloc test_calloc
#define free test_free
#include "../components/demo_platform/message_buffer.c"
#undef calloc
#undef free

int main(void)
{
    message_buffer_t buffer = {0};
    message_buffer_release(&buffer);
    assert(!message_buffer_begin(&buffer, 0));
    assert(!message_buffer_append(&buffer, 0, "x", 1));
    fail_next = 1;
    assert(!message_buffer_begin(&buffer, 4096));
    assert(!buffer.data && !buffer.used && !buffer.capacity && !live);
    char payload[4095];
    memset(payload, 'A', sizeof(payload));
    assert(message_buffer_begin(&buffer, 4096));
    assert(message_buffer_append(&buffer, 0, payload, 1024));
    assert(!message_buffer_append(&buffer, 0, payload, 1));
    assert(!message_buffer_append(&buffer, 1025, payload, 1));
    assert(!message_buffer_append(&buffer, 1024, NULL, 1));
    assert(!message_buffer_append(&buffer, 1024, payload, SIZE_MAX));
    assert(!message_buffer_append(&buffer, 1024, payload, 3072));
    assert(buffer.used == 1024 && buffer.data[1024] == 0);
    assert(message_buffer_append(&buffer, 1024, payload, 3071));
    assert(!memcmp(buffer.data, payload, sizeof(payload)) && buffer.data[4095] == 0);
    assert(!message_buffer_append(&buffer, 4095, payload, 1));
    assert(message_buffer_append(&buffer, 4095, NULL, 0));
    /* Interrupted message: beginning the next message wipes the old allocation. */
    assert(message_buffer_begin(&buffer, 16));
    assert(!buffer.used && buffer.capacity == 16);
    assert(message_buffer_append(&buffer, 0, "secret", 6));
    fail_next = 1;
    assert(!message_buffer_begin(&buffer, 4096));
    assert(!buffer.data && !buffer.capacity && !live);
    for (int i = 0; i < 10000; ++i) {
        assert(message_buffer_begin(&buffer, 4096));
        assert(message_buffer_append(&buffer, 0, payload, sizeof(payload)));
        message_buffer_release(&buffer);
        message_buffer_release(&buffer);
        assert(!buffer.data && !buffer.used && !buffer.capacity && !live);
    }
    puts("message buffer: bounds, fragments, OOM, wipe, 10000 cycles passed");
    return 0;
}
