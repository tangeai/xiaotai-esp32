// SPDX-License-Identifier: MIT
/* Native MQTT allocation/RTOS resource observer for the joined owner fixture.
 * Compile the implementation once, after that fixture's host_group definition.
 * Apply allocator hooks only to generated native code, never the port or host OS.
 */
#ifndef XIAOTAI_MQTT_OWNER_LIFECYCLE_PROVIDER_H
#define XIAOTAI_MQTT_OWNER_LIFECYCLE_PROVIDER_H
#include <stddef.h>
#include "mqtt_client_priv.h"

typedef struct {
    size_t attempts, live, live_bytes, dirty, wiped, tx_wiped;
    size_t mutex_created, mutex_deleted, group_created, group_deleted;
    size_t loop_created, loop_deleted;
} fixture_lifecycle_stats_t;

void *fixture_lifecycle_malloc(size_t size);
void *fixture_lifecycle_calloc(size_t count, size_t size);
char *fixture_lifecycle_strdup(const char *text);
void fixture_lifecycle_free(void *pointer);
void *fixture_lifecycle_caps_calloc(size_t count, size_t size, unsigned caps);
void fixture_lifecycle_begin(size_t fail_nth);
fixture_lifecycle_stats_t fixture_lifecycle_snapshot(void);
void fixture_lifecycle_mark_tx(void *pointer);
SemaphoreHandle_t fixture_lifecycle_mutex_create(void);
void fixture_lifecycle_mutex_delete(SemaphoreHandle_t mutex);
EventGroupHandle_t fixture_lifecycle_group_create(void);
void fixture_lifecycle_group_delete(EventGroupHandle_t group);
esp_err_t fixture_lifecycle_event_loop_create(const esp_event_loop_args_t *args,
                                             esp_event_loop_handle_t *handle);
esp_err_t fixture_lifecycle_event_loop_delete(esp_event_loop_handle_t handle);
#endif

#if defined(XIAOTAI_MQTT_LIFECYCLE_IMPLEMENTATION) && !defined(XIAOTAI_MQTT_LIFECYCLE_IMPLEMENTED)
#define XIAOTAI_MQTT_LIFECYCLE_IMPLEMENTED
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "mqtt_client_priv.h"

typedef struct { void *pointer; size_t size; bool sensitive, tx; } fixture_lifecycle_allocation_t;
static pthread_mutex_t fixture_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static fixture_lifecycle_allocation_t fixture_lifecycle_allocations[1024];
static fixture_lifecycle_stats_t fixture_lifecycle_stats;
static size_t fixture_lifecycle_failure;

static fixture_lifecycle_allocation_t *fixture_lifecycle_find(void *pointer)
{
    for (size_t i = 0; i < sizeof(fixture_lifecycle_allocations) / sizeof(fixture_lifecycle_allocations[0]); ++i)
        if (fixture_lifecycle_allocations[i].pointer == pointer) return &fixture_lifecycle_allocations[i];
    assert(!"untracked native allocation or exhausted fixture table");
    return NULL;
}

void *fixture_lifecycle_malloc(size_t size)
{
    assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
    if (++fixture_lifecycle_stats.attempts == fixture_lifecycle_failure) {
        assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
        return NULL;
    }
    void *pointer = malloc(size);
    assert(pointer && size);
    *fixture_lifecycle_find(NULL) = (fixture_lifecycle_allocation_t){.pointer = pointer, .size = size};
    ++fixture_lifecycle_stats.live;
    fixture_lifecycle_stats.live_bytes += size;
    assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
    return pointer;
}

void *fixture_lifecycle_calloc(size_t count, size_t size)
{
    assert(!count || size <= SIZE_MAX / count);
    void *pointer = fixture_lifecycle_malloc(count * size);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

char *fixture_lifecycle_strdup(const char *text)
{
    const size_t size = strlen(text) + 1U;
    char *copy = fixture_lifecycle_malloc(size);
    if (!copy) return NULL;
    memcpy(copy, text, size);
    if (!strcmp(text, "owner-port") || !strcmp(text, "owner-user") || !strcmp(text, "owner-pass")) {
        assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
        fixture_lifecycle_find(copy)->sensitive = true;
        assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
    }
    return copy;
}

void fixture_lifecycle_free(void *pointer)
{
    if (!pointer) return;
    assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
    fixture_lifecycle_allocation_t *allocation = fixture_lifecycle_find(pointer);
    if (allocation->sensitive) {
        const unsigned char *bytes = pointer;
        unsigned char nonzero = 0;
        for (size_t i = 0; i < allocation->size; ++i) nonzero |= bytes[i];
        if (nonzero) ++fixture_lifecycle_stats.dirty;
        else {
            ++fixture_lifecycle_stats.wiped;
            if (allocation->tx) ++fixture_lifecycle_stats.tx_wiped;
        }
    }
    assert(fixture_lifecycle_stats.live && fixture_lifecycle_stats.live_bytes >= allocation->size);
    --fixture_lifecycle_stats.live;
    fixture_lifecycle_stats.live_bytes -= allocation->size;
    *allocation = (fixture_lifecycle_allocation_t){0};
    /* Observe before free: no reads from released storage and no allocator poison assumptions. */
    free(pointer);
    assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
}

void *fixture_lifecycle_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps; /* Native memory is tracked separately from the owner's INTERNAL bridge. */
    return fixture_lifecycle_calloc(count, size);
}

void fixture_lifecycle_begin(size_t fail_nth)
{
    assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
    assert(!fixture_lifecycle_stats.live && !fixture_lifecycle_stats.live_bytes);
    memset(&fixture_lifecycle_stats, 0, sizeof(fixture_lifecycle_stats));
    fixture_lifecycle_failure = fail_nth;
    assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
}

fixture_lifecycle_stats_t fixture_lifecycle_snapshot(void)
{
    assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
    fixture_lifecycle_stats_t result = fixture_lifecycle_stats;
    assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
    return result;
}

void fixture_lifecycle_mark_tx(void *pointer)
{
    assert(pointer);
    assert(!pthread_mutex_lock(&fixture_lifecycle_lock));
    fixture_lifecycle_allocation_t *allocation = fixture_lifecycle_find(pointer);
    allocation->sensitive = allocation->tx = true;
    assert(!pthread_mutex_unlock(&fixture_lifecycle_lock));
}

SemaphoreHandle_t fixture_lifecycle_mutex_create(void)
{
    pthread_mutex_t *mutex = fixture_lifecycle_malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    pthread_mutexattr_t attributes;
    assert(!pthread_mutexattr_init(&attributes));
    assert(!pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(mutex, &attributes));
    assert(!pthread_mutexattr_destroy(&attributes));
    ++fixture_lifecycle_stats.mutex_created;
    return mutex;
}

void fixture_lifecycle_mutex_delete(SemaphoreHandle_t mutex)
{
    assert(mutex && !pthread_mutex_destroy(mutex));
    ++fixture_lifecycle_stats.mutex_deleted;
    fixture_lifecycle_free(mutex);
}

EventGroupHandle_t fixture_lifecycle_group_create(void)
{
    struct host_group *group = fixture_lifecycle_calloc(1, sizeof(*group));
    if (!group) return NULL;
    assert(!pthread_mutex_init(&group->lock, NULL));
    assert(!pthread_cond_init(&group->changed, NULL));
    ++fixture_lifecycle_stats.group_created;
    return group;
}

void fixture_lifecycle_group_delete(EventGroupHandle_t group)
{
    assert(group && !pthread_mutex_destroy(&group->lock) && !pthread_cond_destroy(&group->changed));
    ++fixture_lifecycle_stats.group_deleted;
    fixture_lifecycle_free(group);
}

esp_err_t fixture_lifecycle_event_loop_create(const esp_event_loop_args_t *args,
                                                    esp_event_loop_handle_t *handle)
{
    assert(args && handle && !args->task_name && args->queue_size == MQTT_EVENT_QUEUE_SIZE);
    assert(!*handle);
    /* Thin event-loop resource, not an ESP event implementation. Native owner
     * interception still supplies the real dispatch ordering in the shared fixture. */
    *handle = fixture_lifecycle_calloc(1, 1);
    if (!*handle) return ESP_ERR_NO_MEM;
    ++fixture_lifecycle_stats.loop_created;
    return ESP_OK;
}

esp_err_t fixture_lifecycle_event_loop_delete(esp_event_loop_handle_t handle)
{
    assert(handle);
    ++fixture_lifecycle_stats.loop_deleted;
    fixture_lifecycle_free(handle);
    return ESP_OK;
}
#endif
