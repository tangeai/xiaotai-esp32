// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/room.h"
#include "xiaotai/json.h"
#include "xiaotai/link.h"
#include "xiaotai/platform.h"

#define ROOM_LOG_SOURCE "room"

typedef enum {
    ROOM_REQUEST_NONE, ROOM_REQUEST_ASSIGNMENT, ROOM_REQUEST_CREATE,
    ROOM_REQUEST_JOIN, ROOM_REQUEST_LEAVE, ROOM_REQUEST_TOKEN, ROOM_REQUEST_PRESENCE,
} room_request_t;

typedef struct {
    xiaotai_room_action_t action;
    uint32_t attendance;
    uint32_t binding;
    char code[XIAOTAI_ROOM_CODE_DIGITS + 1U];
    char password[XIAOTAI_ROOM_PASSWORD_DIGITS + 1U];
} room_action_t;

typedef enum {
    ROOM_END_ATTENDANCE, ROOM_END_ASSIGNMENT, ROOM_END_LEAVE,
    ROOM_END_OFFLINE, ROOM_END_BINDING, ROOM_END_MANUAL, ROOM_END_FAILED, ROOM_END_REVOKED,
} room_end_t;

typedef enum { ROOM_LINK_NONE, ROOM_LINK_TOKEN, ROOM_LINK_OPEN, ROOM_LINK_JOIN, ROOM_LINK_ACTIVE } room_link_t;

typedef struct {
    room_link_t stage;
    uint32_t activity;
    uint32_t token_ticket;
    uint32_t heartbeat_ms, lease_ms;
    bool no_mem_reported, superseded_unknown;
    int64_t deadline, sent_at, lease_until, heartbeat_at, retry_at, snapshot_at;
    int64_t snapshot_not_before, members_until;
    uint8_t oversized;
    bool link_alive, opened, media_open, parked, transport_online;
    char session_id[33];
    char signal_room[257];
    char command[512];
} room_connection_t;

typedef struct {
    uint32_t sequence, await_token, version;
    int64_t at;
    bool inflight, failed, left;
    char room_id[XIAOTAI_ROOM_ID_MAX], session_id[33];
} room_release_t;

typedef struct room room_t;
struct room {
    xiaotai_room_config_t config;
    xiaotai_room_status_t view, published;
    xiaotai_room_stats_t stats, published_stats;
    uint32_t binding, attendance, notify_revision, notice, read_notice;
    uint32_t ticket, flights, timer;
    int64_t sync_at, mutation_at;
    room_request_t pending;
    room_action_t mutation;
    bool mutation_queued, confirming, online, manual_requested, manual_disconnected;
    bool publish_event, publish_retry;
    char body[256];

    /* The connection phase supplies teardown before the assignment is changed. */
    void (*end_connection)(room_t *room, room_end_t reason);
    bool release_pending;
    room_connection_t link;
    room_release_t release;
    struct {
        uint32_t peer_bytes, token_bytes;
        uint8_t peer_type, token_type;
        bool valid;
    } token_shape;
    struct {
        xiaotai_room_member_t *rows;
        xiaotai_room_member_t scratch;
        uint32_t revision;
        uint16_t count, remote_speaking;
        bool self_present, self_speaking, event, voice_event;
    } members;
};

bool room_digits(const char *text, size_t digits, bool optional);
xiaotai_err_t room_controls_init(void);
int64_t room_now_ms(void);
room_t *room_context(void);
bool room_started(void);
void room_attempt_changed(uint32_t activity, xiaotai_room_attempt_t phase);
void room_update_context(room_t *room);
void room_drive(room_t *room);
void room_end_connection(room_t *room, room_end_t reason);
void room_result(xiaotai_room_action_t action, xiaotai_err_t error, int32_t code);
void room_read_again(room_t *room);
void room_retire_read(room_t *room);
void room_assignment_drive(room_t *room, int64_t now);
void room_assignment_action(room_t *room, const room_action_t *action);
void room_assignment_reset(room_t *room);

/* Keep all ordinary session requests in the same ticket/domain. The engine
 * calls the handler once even for a logically retired request. */
bool room_request_can_yield(room_request_t request, xiaotai_room_action_t action);
int32_t room_reply_code(const xiaotai_platform_reply_t *reply);
void room_connection_end(room_t *room, room_end_t reason);
void room_connection_drive(room_t *room, int64_t now);
void room_release_drive(room_t *room, int64_t now);
int64_t room_connection_due(const room_t *room);
bool room_connection_sync_failed(room_t *room, xiaotai_err_t error, int32_t code);
xiaotai_err_t room_connection_init(void);
xiaotai_err_t room_connection_send(room_t *room, const char *method, bool speaking);
xiaotai_err_t room_connection_accept_join(room_t *room, const xiaotai_json_doc_t *doc);
void room_connection_fail(room_t *room, xiaotai_err_t error, int32_t code, bool permanent);

void room_signal_command(void *ctx, uint32_t generation, const xiaotai_link_command_t *message);
void room_snapshot_request(room_t *room, bool coalesce);
void room_snapshot_drive(room_t *room, int64_t now);
int64_t room_snapshot_due(const room_t *room);

xiaotai_err_t room_members_init(room_t *room);
void room_members_deinit(room_t *room);
void room_members_clear(room_t *room);
void room_members_publish(room_t *room);
bool room_members_snapshot(room_t *room, const xiaotai_json_doc_t *doc, int params);
void room_members_delta(room_t *room, const xiaotai_json_doc_t *doc, int params, bool leaving);
void room_members_self_mic(room_t *room, bool speaking);

xiaotai_err_t room_talk_init(void);
void room_talk_deinit(void);
void room_talk_admit(room_t *room);
void room_admit_talk(room_t *room);
void room_talk_retire(uint32_t generation);
void room_talk_drive(room_t *room, int64_t now);
int64_t room_talk_due(const room_t *room, int64_t now);
void room_talk_stats(xiaotai_room_stats_t *stats);
