// SPDX-License-Identifier: MIT
#pragma once
#include "xmini_audio.h"
#include "xmini_business.h"

typedef struct {
    xmini_business_owner_t owner;
    xmini_business_phase_t phase;
    uint32_t generation;
} xmini_feedback_identity_t;

static inline bool xmini_feedback_identity_changed(xmini_feedback_identity_t previous,
                                                   xmini_feedback_identity_t current)
{
    return previous.owner != current.owner || previous.phase != current.phase ||
           previous.generation != current.generation;
}

static inline int xmini_session_feedback(xmini_business_owner_t previous_owner,
                                         xmini_business_owner_t owner,
                                         xmini_business_phase_t phase)
{
    if (owner != XMINI_BUSINESS_NONE) {
        if (phase == XMINI_BUSINESS_RINGING) return XMINI_PROMPT_INCOMING;
        if (phase == XMINI_BUSINESS_OUTGOING) return XMINI_PROMPT_DIALING;
    }
    if (phase == XMINI_BUSINESS_ERROR &&
        (owner != XMINI_BUSINESS_NONE || previous_owner != XMINI_BUSINESS_NONE))
        return XMINI_PROMPT_FAILED;
    if (phase == XMINI_BUSINESS_IDLE && previous_owner != XMINI_BUSINESS_NONE)
        return XMINI_PROMPT_ENDED;
    return -1;
}
