#include "voip_state.h"
bool call_begin(call_state_t *s, bool outgoing)
{
    if (s->stage != CALL_IDLE)
        return false;
    uint32_t g = s->generation + 1;
    *s = (call_state_t){.stage = outgoing ? CALL_DIALING : CALL_RINGING,
                        .generation = g ? g : 1,
                        .outgoing = outgoing};
    return true;
}
bool call_accept(call_state_t *s)
{
    if (s->stage != CALL_RINGING)
        return false;
    s->stage = CALL_CONNECTING;
    return true;
}
bool call_join_outgoing(call_state_t *s)
{
    if (s->stage != CALL_DIALING || !s->outgoing)
        return false;
    s->stage = CALL_CONNECTING;
    return true;
}
bool call_transport_ready(call_state_t *s, uint32_t generation)
{
    if (generation != s->generation || s->stage != CALL_CONNECTING)
        return false;
    s->transport_ready = true;
    s->stage = s->peer_answered ? CALL_ACTIVE : CALL_WAITING_PEER;
    return true;
}
bool call_peer_answered(call_state_t *s, uint32_t generation)
{
    if (generation != s->generation ||
        (s->stage != CALL_CONNECTING && s->stage != CALL_WAITING_PEER))
        return false;
    s->peer_answered = true;
    if (s->transport_ready)
        s->stage = CALL_ACTIVE;
    return true;
}
void call_close(call_state_t *s)
{
    if (s->stage != CALL_IDLE)
        s->stage = CALL_CLOSING;
}
void call_finish(call_state_t *s)
{
    uint32_t generation = s->generation;
    *s = (call_state_t){.generation = generation};
}
call_primary_action_t call_primary_action(call_stage_t stage)
{
    switch (stage) {
    case CALL_IDLE:
        return CALL_PRIMARY_DIAL_FIRST;
    case CALL_RINGING:
        return CALL_PRIMARY_ANSWER;
    case CALL_DIALING:
    case CALL_CONNECTING:
    case CALL_WAITING_PEER:
    case CALL_ACTIVE:
        return CALL_PRIMARY_HANGUP;
    case CALL_CLOSING:
    default:
        return CALL_PRIMARY_NONE;
    }
}
const char *call_stage_name(call_stage_t stage)
{
    static const char *names[] = {"idle",         "dialing", "ringing", "connecting",
                                  "waiting-peer", "active",  "closing"};
    return stage >= CALL_IDLE && stage <= CALL_CLOSING ? names[stage] : "invalid";
}
