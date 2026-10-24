// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "platform_routes.h"
#include "platform_control_state.h"
#include "platform_request_types.h"

static bool online = true, queue_full, advance_on_enter, advance_on_exit, advance_when_online;
static unsigned depth, sent;
static int s_request_lock;
static void *s_requests = (void *)1;
static bool s_response_blocked;
static platform_http_gate_t s_http_gate;
static atomic_uint s_epoch;
static request_t queued;
bool platform_is_online(void)
{
    if (advance_when_online) atomic_fetch_add(&s_epoch, 1);
    return online;
}
static void enter(int *lock)
{
    assert(lock == &s_request_lock && depth++ == 0);
    if (advance_on_enter) atomic_fetch_add(&s_epoch, 1);
}
static void leave(int *lock)
{
    assert(lock == &s_request_lock && depth-- == 1);
    if (advance_on_exit) atomic_fetch_add(&s_epoch, 1);
}
#define portENTER_CRITICAL(lock) enter(lock)
#define portEXIT_CRITICAL(lock) leave(lock)
#define pdTRUE 1
static int xQueueSend(void *queue, const void *item, unsigned ticks)
{
    assert(queue == s_requests && depth == 1 && ticks == 0);
    if (queue_full) return 0;
    queued = *(const request_t *)item;
    ++sent;
    return pdTRUE;
}
#include "../components/demo_platform/platform_request_impl.inc"

int main(void)
{
    atomic_store(&s_epoch, 10);
    char body[] = "{\"room_id\":\"owned-room\"}";
    unsigned accepted = 777;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_HANGUP, 42, body, &accepted) == ESP_OK);
    assert(accepted == 10 && queued.epoch == accepted && queued.generation == 42 && s_http_gate.pending == 1);
    body[2] = '!';
    assert(!strcmp(queued.body, "{\"room_id\":\"owned-room\"}"));

    advance_on_enter = advance_on_exit = true;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 43, NULL, &accepted) == ESP_OK);
    assert(queued.epoch == 10 && accepted == queued.epoch && atomic_load(&s_epoch) == 12);
    advance_on_enter = advance_on_exit = false;
    assert(platform_request(PLATFORM_DEVICE_CALL_ROOM, 44, NULL) == ESP_OK);
    assert(queued.epoch == 12 && queued.generation == 44 && sent == 3);
    unsigned caller_pre_read = atomic_load(&s_epoch);
    advance_when_online = true;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, NULL, &accepted) == ESP_OK);
    assert(accepted == queued.epoch && accepted == caller_pre_read + 1 && sent == 4);
    advance_when_online = false;
    accepted = 777;
    queue_full = true;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, NULL, &accepted) == ESP_ERR_NO_MEM && accepted == 777);
    queue_full = false;
    s_http_gate.held = true;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, NULL, &accepted) == ESP_ERR_INVALID_STATE && accepted == 777);
    s_http_gate.held = false;
    s_response_blocked = true;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, NULL, &accepted) == ESP_ERR_INVALID_STATE && accepted == 777);
    s_response_blocked = false;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, "{}", &accepted) == ESP_ERR_INVALID_ARG && accepted == 777);
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_HANGUP, 45, "[]", &accepted) == ESP_ERR_INVALID_ARG && accepted == 777);
    assert(platform_request_epoch(PLATFORM_OPERATION_COUNT, 45, NULL, &accepted) == ESP_ERR_INVALID_ARG && accepted == 777);
    char oversized[769]; memset(oversized, 'x', 768); oversized[768] = 0;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_HANGUP, 45, oversized, &accepted) == ESP_ERR_INVALID_SIZE && accepted == 777);
    online = false;
    assert(platform_request_epoch(PLATFORM_DEVICE_CALL_ROOM, 45, NULL, &accepted) == ESP_ERR_INVALID_STATE && accepted == 777);
    assert(sent == 4 && s_http_gate.pending == 4 && depth == 0);
    puts("Platform actual admission: owned epoch, mid-admission lifecycle change, copied body, legacy API and failures PASS");
    return 0;
}
