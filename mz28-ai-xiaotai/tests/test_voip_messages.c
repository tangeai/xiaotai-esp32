#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

static pthread_mutex_t slot_mux = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned in_mux;
static atomic_bool fail_allocation;
static atomic_uint allocations;
static atomic_size_t allocated_bytes;
typedef struct { size_t size; max_align_t alignment; } allocation_t;

static void *message_malloc(size_t size)
{
    assert(!in_mux);
    if (atomic_exchange(&fail_allocation, false))
        return NULL;
    allocation_t *p = malloc(sizeof(*p) + size);
    assert(p);
    p->size = size;
    atomic_fetch_add(&allocations, 1);
    atomic_fetch_add(&allocated_bytes, size);
    return p + 1;
}
static void message_free(void *data)
{
    assert(!in_mux && data);
    allocation_t *p = (allocation_t *)data - 1;
    for (size_t i = 0; i < p->size; ++i)
        assert(((unsigned char *)data)[i] == 0);
    atomic_fetch_sub(&allocations, 1);
    atomic_fetch_sub(&allocated_bytes, p->size);
    free(p);
}
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(lock) do { (void)(lock); pthread_mutex_lock(&slot_mux); ++in_mux; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); --in_mux; pthread_mutex_unlock(&slot_mux); } while (0)
#define malloc message_malloc
#define free message_free
#include "../components/wechat_voip/voip.c"
#undef malloc
#undef free

static event_t queued[MESSAGE_SLOTS];
static unsigned queue_count;
static bool queue_full;
static rtc_event_t queued_rtc;
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks)
{
    assert(!in_mux && !ticks);
    if (queue_full)
        return 0;
    if (queue == s_rtc_queue) {
        queued_rtc = *(const rtc_event_t *)item;
        return pdTRUE;
    }
    assert(queue == s_queue);
    assert(queue_count < MESSAGE_SLOTS);
    queued[queue_count++] = *(const event_t *)item;
    return pdTRUE;
}
static void check_empty(void)
{
    assert(atomic_load(&allocations) == 0);
    assert(atomic_load(&allocated_bytes) == 0);
    for (unsigned i = 0; i < MESSAGE_SLOTS; ++i)
        assert(!s_slot_used[i] && !s_messages[i] && !s_message_sizes[i]);
}
static void *stress(void *arg)
{
    char message[257];
    memset(message, 'A' + (uintptr_t)arg, sizeof(message) - 1);
    message[sizeof(message) - 1] = 0;
    for (unsigned n = 0; n < 10000; ++n) {
        int slot = reserve_slot(message);
        if (slot < 0) {
            assert(slot == -2);
            continue;
        }
        assert(!strcmp(s_messages[slot], message));
        assert(s_message_sizes[slot] == sizeof(message));
        release_slot(slot);
    }
    return NULL;
}
int main(void)
{
    s_queue = (void *)(uintptr_t)1;
    s_rtc_queue = (void *)(uintptr_t)2;
    assert(sizeof(event_t) < sizeof(rtc_event_t));
    assert(sizeof(s_queue_buffer) == 8 * sizeof(event_t));
    assert(sizeof(s_rtc_queue_buffer) == 16 * sizeof(rtc_event_t));
    rtc_event_t r = {.kind = RTC_COMMAND, .generation = 7, .command = 0x2001,
                     .length = 12, .data = "{\"reason\":1}"};
    assert(rtc_event(&r));
    memset(r.data, 0, sizeof(r.data));
    assert(queued_rtc.generation == 7 && queued_rtc.command == 0x2001);
    assert(!strcmp(queued_rtc.data, "{\"reason\":1}"));
    queue_full = true;
    assert(!rtc_event(&r) && atomic_exchange(&s_event_failure, false));
    queue_full = false;
    assert(reserve_slot(NULL) == -1);
    release_slot(-1);
    int empty = reserve_slot("");
    assert(empty >= 0 && s_message_sizes[empty] == 1);
    release_slot(empty);
    char json[MESSAGE_SIZE + 1];
    memset(json, 'x', sizeof(json));
    json[MESSAGE_SIZE] = 0;
    assert(reserve_slot(json) == -2);
    json[MESSAGE_SIZE - 1] = 0;
    int slots[MESSAGE_SLOTS];
    for (unsigned i = 0; i < MESSAGE_SLOTS; ++i) {
        slots[i] = reserve_slot(json);
        assert(slots[i] >= 0 && !strcmp(s_messages[slots[i]], json));
    }
    assert(atomic_load(&allocated_bytes) == MESSAGE_SLOTS * MESSAGE_SIZE);
    assert(reserve_slot("overflow") == -2);
    for (unsigned i = 0; i < MESSAGE_SLOTS; ++i)
        release_slot(slots[i]);
    check_empty();
    atomic_store(&fail_allocation, true);
    assert(reserve_slot("oom") == -2);
    check_empty();

    char source[] = "{\"token\":\"private\"}";
    platform_event_t p = {.kind = PLATFORM_MESSAGE, .json = source};
    assert(wechat_voip_platform_event(&p));
    source[0] = 'x';
    assert(!strcmp(s_messages[queued[0].slot], "{\"token\":\"private\"}"));
    assert(queued[0].platform.json == NULL);
    release_slot(queued[0].slot);
    queue_count = 0;
    queue_full = true;
    assert(!wechat_voip_platform_event(&p));
    assert(!atomic_load(&s_event_failure));
    check_empty();
    p.kind = PLATFORM_RESPONSE;
    assert(!wechat_voip_platform_event(&p));
    assert(atomic_load(&s_event_failure));
    check_empty();

    pthread_t threads[8];
    for (uintptr_t i = 0; i < 8; ++i)
        assert(!pthread_create(&threads[i], NULL, stress, (void *)i));
    for (unsigned i = 0; i < 8; ++i)
        assert(!pthread_join(threads[i], NULL));
    check_empty();
    puts("Actual VoIP message ownership, capacity, OOM, queue rollback and concurrency passed");
}
