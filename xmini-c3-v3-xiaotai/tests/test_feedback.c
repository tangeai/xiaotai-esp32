// SPDX-License-Identifier: MIT
#include <assert.h>
#include "xmini_feedback.h"

static void transition(xmini_feedback_identity_t previous,
                       xmini_feedback_identity_t current, int expected)
{
    int pending = XMINI_PROMPT_INCOMING;
    bool changed = xmini_feedback_identity_changed(previous, current);
    assert(changed);
    if (changed) pending = -1;
    // The product poll must not consume the previous session's pending prompt.
    assert(pending == -1);
    if (changed) pending = xmini_session_feedback(previous.owner, current.owner, current.phase);
    assert(pending == expected);
    previous = current;
    assert(!xmini_feedback_identity_changed(previous, current));
}

int main(void)
{
    xmini_feedback_identity_t ringing = {XMINI_BUSINESS_WECHAT, XMINI_BUSINESS_RINGING, 7};
    transition(ringing, (xmini_feedback_identity_t){XMINI_BUSINESS_NONE, XMINI_BUSINESS_IDLE, 7},
               XMINI_PROMPT_ENDED);
    transition(ringing, (xmini_feedback_identity_t){XMINI_BUSINESS_NONE, XMINI_BUSINESS_ERROR, 7},
               XMINI_PROMPT_FAILED);
    transition(ringing, (xmini_feedback_identity_t){XMINI_BUSINESS_WECHAT, XMINI_BUSINESS_RINGING, 8},
               XMINI_PROMPT_INCOMING);
    transition(ringing, (xmini_feedback_identity_t){XMINI_BUSINESS_DEVICE, XMINI_BUSINESS_RINGING, 7},
               XMINI_PROMPT_INCOMING);
    transition(ringing, (xmini_feedback_identity_t){XMINI_BUSINESS_WECHAT, XMINI_BUSINESS_ACTIVE, 7}, -1);
    assert(xmini_session_feedback(XMINI_BUSINESS_NONE, XMINI_BUSINESS_NONE,
                                 XMINI_BUSINESS_IDLE) == -1);
    assert(xmini_session_feedback(XMINI_BUSINESS_NONE, XMINI_BUSINESS_NONE,
                                 XMINI_BUSINESS_ERROR) == -1);
    for (unsigned owner = XMINI_BUSINESS_AI; owner <= XMINI_BUSINESS_GROUP; ++owner) {
        assert(xmini_session_feedback(XMINI_BUSINESS_NONE, owner,
                                     XMINI_BUSINESS_RINGING) == XMINI_PROMPT_INCOMING);
        assert(xmini_session_feedback(XMINI_BUSINESS_NONE, owner,
                                     XMINI_BUSINESS_OUTGOING) == XMINI_PROMPT_DIALING);
        assert(xmini_session_feedback(owner, owner, XMINI_BUSINESS_ACTIVE) == -1);
        assert(xmini_session_feedback(owner, owner, XMINI_BUSINESS_CONNECTING) == -1);
        assert(xmini_session_feedback(owner, owner, XMINI_BUSINESS_ENDING) == -1);
        assert(xmini_session_feedback(owner, XMINI_BUSINESS_NONE,
                                     XMINI_BUSINESS_IDLE) == XMINI_PROMPT_ENDED);
        assert(xmini_session_feedback(owner, XMINI_BUSINESS_NONE,
                                     XMINI_BUSINESS_ERROR) == XMINI_PROMPT_FAILED);
    }
    return 0;
}
