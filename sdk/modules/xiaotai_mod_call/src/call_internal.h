// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/json_view.h"
#include "xiaotai/link.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/platform.h"

#define CALL_ROOM_SIZE 129U
#define CALL_COMMAND_ANSWER 0x2000U
#define CALL_COMMAND_END 0x2001U
#define CALL_COMMAND_BYTES 512U
#define CALL_COMMAND_SHARE 1024U
#define CALL_JSON_TOKENS 64U
#define CALL_RESPONSE_BYTES 2048U
#define CALL_INVALID_RESPONSE 0x108

typedef enum {
    CALL_REPAIR_NONE, CALL_REPAIR_QUERY, CALL_REPAIR_RELEASE, CALL_REPAIR_VERIFY,
} call_repair_step_t;

typedef enum {
    CALL_RELEASE_RESERVED, CALL_RELEASE_DIAL_WAIT, CALL_RELEASE_POST, CALL_RELEASE_QUERY, CALL_RELEASE_UNCONFIRMED,
    CALL_RELEASE_AWAIT_RETIREMENT,
} call_release_step_t;

typedef struct {
    int64_t due_us;
    uint32_t session, binding, ticket;
    xiaotai_call_state_t phase;
    call_release_step_t step;
    uint8_t attempts, post_denials;
    bool awaiting_dial, redial, video, deferred, reconciling, post_once, post_p2p_error;
    char room[CALL_ROOM_SIZE];
} call_release_entry_t;

typedef struct {
    uint32_t profile_fallbacks, profile_parts_ignored, video_parts_ignored, listen_failures;
} call_diagnostic_counters_t;

void call_diagnostic_counters(call_diagnostic_counters_t *out);
void call_profile_fallback(void);
void call_profile_parts_ignored(size_t count);
xiaotai_err_t call_profile_register(xiaotai_call_config_t *config);
const xiaotai_call_config_t *call_config(void);
xiaotai_err_t call_at_register(void);
xiaotai_err_t call_dial_device_at(const char *device_id);
xiaotai_err_t call_index_owner(uint8_t index, bool *owned);

bool call_parse(const char *text, size_t size, xiaotai_json_view_t *root);
bool call_string(const xiaotai_json_view_t *value, char *text, size_t capacity);
bool call_binding_current(uint32_t generation);
void call_incoming(const char *room, const char *peer, const char *name, bool video, bool unsupported);
void call_cloud_answer(const char *room, const char *callee);
void call_cloud_end(const char *room, bool rejected);
void call_finish(xiaotai_call_end_t end, xiaotai_err_t error, bool delay, const char *reason);
void call_dial_reply(uint32_t session, bool redial, const xiaotai_platform_reply_t *reply);
void call_accept_reply(uint32_t session, const xiaotai_platform_reply_t *reply);
void call_repair_reply(uint32_t session, call_repair_step_t step, const xiaotai_platform_reply_t *reply);
xiaotai_err_t call_hold_prepare(bool video, bool *prepared);
void call_hold_after_submit(bool prepared, bool accepted);
bool call_foreground_owns_room(uint32_t except_session, const char *room);

xiaotai_err_t call_release_init(xiaotai_call_status_t *status, size_t count);
void call_release_reset(void);
void call_release_clear(uint32_t binding);
void call_release_online(uint32_t binding);
void call_release_activity(void);
call_release_entry_t *call_release_find(uint32_t session);
call_release_entry_t *call_release_slot(void);
void call_release_counts(void);
bool call_release_finished_room(const char *room);
void call_release_end(uint32_t session, uint32_t binding, xiaotai_call_state_t phase,
                      const char *room, const char *reason, bool video, bool await_retirement);
void call_release_retirement(uint32_t session, bool confirmed);
void call_release_retired_dial(call_release_entry_t *entry, bool definitive, bool got_room);

void call_signaling_start(uint32_t session, uint32_t link);
bool call_signaling_current(uint32_t session, uint32_t link);
bool call_signaling_end(uint32_t session);
void call_signaling_drain(uint32_t session);
void call_signaling_reset(void);

xiaotai_err_t call_signals_register(void);
xiaotai_err_t call_submit_dial(uint32_t session, const char *target, bool video, bool redial, uint32_t binding);
xiaotai_err_t call_submit_accept(uint32_t session, const char *room, const char *peer);
xiaotai_err_t call_submit_release(xiaotai_call_state_t phase, const char *room, const char *reason, uint32_t ticket,
                                 uint32_t flags, xiaotai_platform_reply_fn_t reply);
xiaotai_err_t call_submit_room_check(uint32_t ticket, uint32_t flags, xiaotai_platform_reply_fn_t reply);
xiaotai_err_t call_submit_repair(uint32_t session, call_repair_step_t step, const char *room, xiaotai_call_state_t phase);
typedef struct {
    xiaotai_json_view_t data;
    bool duplicate;
} call_http_payload_t;
int32_t call_http_code(const xiaotai_platform_reply_t *reply, call_http_payload_t *payload);
bool call_http_room(const call_http_payload_t *payload, char room[CALL_ROOM_SIZE]);
bool call_http_room_id_valid(const char *room);
xiaotai_err_t call_http_release_action(const xiaotai_platform_reply_t *reply, const char *expected_room,
                                      xiaotai_call_state_t *phase);
xiaotai_err_t call_http_repair_action(const xiaotai_platform_reply_t *reply, bool verify,
                                    const char *old_room, xiaotai_call_state_t *phase);
xiaotai_err_t call_http_accept_token(const xiaotai_platform_reply_t *reply, const char *expected_peer,
                                   bool *denied, char **token);
void call_http_free_token(char *token);
xiaotai_err_t call_send_answer(uint32_t link, const char *room);
