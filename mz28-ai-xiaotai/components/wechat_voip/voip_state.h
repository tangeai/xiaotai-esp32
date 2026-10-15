#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    CALL_IDLE,
    CALL_DIALING,
    CALL_RINGING,
    CALL_CONNECTING,
    CALL_WAITING_PEER,
    CALL_ACTIVE,
    CALL_CLOSING
} call_stage_t;
typedef enum {
    CALL_PRIMARY_NONE,
    CALL_PRIMARY_DIAL_FIRST,
    CALL_PRIMARY_ANSWER,
    CALL_PRIMARY_HANGUP
} call_primary_action_t;
typedef struct {
    call_stage_t stage;
    uint32_t generation;
    bool outgoing, transport_ready, peer_answered;
} call_state_t;
bool call_begin(call_state_t *state, bool outgoing);
bool call_accept(call_state_t *state);
bool call_join_outgoing(call_state_t *state);
bool call_transport_ready(call_state_t *state, uint32_t generation);
bool call_peer_answered(call_state_t *state, uint32_t generation);
void call_close(call_state_t *state);
void call_finish(call_state_t *state);
call_primary_action_t call_primary_action(call_stage_t stage);
const char *call_stage_name(call_stage_t stage);
