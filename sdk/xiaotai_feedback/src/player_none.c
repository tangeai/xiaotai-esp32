// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

xiaotai_err_t fb_default_player_init(void) { return XIAOTAI_OK; }
void fb_default_player_reset(void) {}
bool fb_default_player_stale(const xiaotai_feedback_player_t *player, uint32_t generation)
{
    (void)player;
    (void)generation;
    return false;
}
