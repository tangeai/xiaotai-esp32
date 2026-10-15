#include <assert.h>
#include <stdio.h>
#include "../components/wechat_voip/voip_state.h"
int main(void)
{
    call_state_t s = {0};
    assert(call_primary_action(CALL_IDLE) == CALL_PRIMARY_DIAL_FIRST);
    assert(call_primary_action(CALL_RINGING) == CALL_PRIMARY_ANSWER);
    assert(call_primary_action(CALL_DIALING) == CALL_PRIMARY_HANGUP);
    assert(call_primary_action(CALL_CONNECTING) == CALL_PRIMARY_HANGUP);
    assert(call_primary_action(CALL_WAITING_PEER) == CALL_PRIMARY_HANGUP);
    assert(call_primary_action(CALL_ACTIVE) == CALL_PRIMARY_HANGUP);
    assert(call_primary_action(CALL_CLOSING) == CALL_PRIMARY_NONE);
    assert(call_primary_action((call_stage_t)-1) == CALL_PRIMARY_NONE);
    assert(call_begin(&s, true));
    uint32_t g = s.generation;
    assert(!call_begin(&s, false));
    assert(!call_accept(&s));
    assert(call_join_outgoing(&s));
    assert(call_transport_ready(&s, g));
    assert(s.stage == CALL_WAITING_PEER); /* WHIP success is NOT answered. */
    assert(call_peer_answered(&s, g));
    assert(s.stage == CALL_ACTIVE);
    call_close(&s);
    assert(!call_peer_answered(&s, g));
    assert(!call_begin(&s, true));
    call_finish(&s);
    assert(call_begin(&s, false));
    assert(s.stage == CALL_RINGING);
    assert(!call_transport_ready(&s, g)); /* stale callback */
    assert(call_accept(&s));
    assert(call_peer_answered(&s, s.generation)); /* command before connected event */
    assert(s.stage == CALL_CONNECTING);
    assert(call_transport_ready(&s, s.generation));
    assert(s.stage == CALL_ACTIVE);
    call_close(&s);
    call_finish(&s);
    assert(call_begin(&s, true));
    assert(call_join_outgoing(&s));
    call_close(&s);
    assert(!call_transport_ready(&s, s.generation)); /* canceled pending WHIP */
    assert(!call_peer_answered(&s, s.generation));
    call_finish(&s);
    for (int i = 0; i < 10000; i++) {
        assert(call_begin(&s, i & 1));
        call_close(&s);
        call_finish(&s);
    }
    s.generation = UINT32_MAX;
    assert(call_begin(&s, true) && s.generation == 1);
    assert(call_stage_name((call_stage_t)-1)[0] == 'i');
    puts("state tests passed");
}
