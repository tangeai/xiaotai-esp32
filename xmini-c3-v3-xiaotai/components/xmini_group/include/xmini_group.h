// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "demo_platform.h"

typedef enum {
    XMINI_GROUP_EMPTY, XMINI_GROUP_SYNCING, XMINI_GROUP_ASSIGNED,
    XMINI_GROUP_TOKEN, XMINI_GROUP_CONNECTING, XMINI_GROUP_JOINING,
    XMINI_GROUP_LISTENING, XMINI_GROUP_SPEAKING, XMINI_GROUP_STOPPING,
    XMINI_GROUP_ERROR
} xmini_group_phase_t;
typedef enum {
    XMINI_GROUP_REFRESH, XMINI_GROUP_CREATE, XMINI_GROUP_JOIN,
    XMINI_GROUP_CONNECT, XMINI_GROUP_DISCONNECT, XMINI_GROUP_LEAVE,
    XMINI_GROUP_PTT_DOWN, XMINI_GROUP_PTT_UP
} xmini_group_action_t;
typedef struct {
    xmini_group_phase_t phase;
    bool assignment_known, assigned, foreground, active, speaking, snapshot_ready;
    uint32_t generation, assignment_version, revision, stale_events;
    unsigned participant_count;
    int error;
    char room_id[65], room_code[7];
} xmini_group_status_t;
typedef struct {
    int (*request)(platform_operation_t operation, uint32_t ticket, const char *json, void *context);
    int (*connect)(uint32_t generation, const char *peer, const char *token, void *context);
    int (*signal)(uint32_t generation, uint32_t command, const char *json, void *context);
    int (*set_profile)(uint32_t generation, uint8_t tx_stream, uint8_t rx_stream, void *context);
    void (*finish)(uint32_t generation, int error, void *context);
    uint32_t (*random)(void *context);
    void *context;
} xmini_group_callbacks_t;

/* Pure control-thread module: no SDK, tasks, audio or stored credentials.
 * Callbacks borrow JSON/credentials for the duration of the call only.
 * request tickets and active/cancel generations are separate identities. */
int xmini_group_init(const xmini_group_callbacks_t *callbacks);
int xmini_group_open(uint32_t generation, const char *device_id, int64_t now_ms);
/* CREATE/JOIN/CONNECT can be deferred behind the initial authoritative read.
 * Acceptance is not success. DISCONNECT preserves membership; LEAVE does not
 * clear it until the server returns a new authoritative assignment. */
int xmini_group_action(xmini_group_action_t action, uint32_t generation,
                       const char *room_code, const char *password, int64_t now_ms);
void xmini_group_on_http(platform_operation_t operation, uint32_t ticket,
                          int transport_error, const char *json, int64_t now_ms);
void xmini_group_on_connected(uint32_t generation, int error, int64_t now_ms);
void xmini_group_on_signal(uint32_t generation, const char *json, size_t length, int64_t now_ms);
void xmini_group_on_closed(uint32_t generation, int error, int64_t now_ms);
/* Page-close/preemption: stop local PTT now and release only the connection. */
void xmini_group_suspend(uint32_t generation, int error, int64_t now_ms);
void xmini_group_assignment_changed(int64_t now_ms);
void xmini_group_tick(int64_t now_ms);
xmini_group_status_t xmini_group_status(void);
