// SPDX-License-Identifier: MIT
#pragma once
/* Build options of the AI talk module (xiaotai/config.h explains the scheme).
 * On ESP-IDF the values come from this component's Kconfig; elsewhere the
 * defaults below apply, and a host test overrides one on the compiler command
 * line (-DCONFIG_XIAOTAI_AI_TALK_ROLE_REQUIRED=1).
 *
 * The defaults are the behaviour xiaotai/modules/ai_talk.h describes. The
 * other values exist so that a product keeps what its deployed firmware does
 * where that differs; the product configuration selects them. Run-time
 * values (deadlines, tail playout, memory) are in xiaotai_ai_talk_config_t. */
#include "xiaotai/config.h"

/* A token reply without a non-empty "data.role_id" is refused (END_REFUSED).
 * Off: the role is optional and the session request then names "". */
#define XIAOTAI_AI_TALK_ROLE_REQUIRED XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AI_TALK_ROLE_REQUIRED, 0)

/* A conversation starts only once the platform reports the device profile
 * accepted in this session (xiaotai_platform_status_t.profile_ready);
 * before that xiaotai_ai_talk_start answers XIAOTAI_ERR_PLATFORM_OFFLINE.
 * Off: an online platform session is enough. */
#define XIAOTAI_AI_TALK_START_NEEDS_PROFILE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AI_TALK_START_NEEDS_PROFILE, 0)

/* A conversation that fails on its own side while its link is open (a
 * deadline, a refused or unsupported session, the audio session, a dropped
 * command, a failed send) first tells the service with end_session, as a
 * local stop does. Off: only a local stop and a revocation tell it. */
#define XIAOTAI_AI_TALK_NOTIFY_FAILURES XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AI_TALK_NOTIFY_FAILURES, 0)

/* A command the transport delivers with `dropped` set (larger than
 * command_max_bytes, or no room in the command pool) ends the conversation
 * (END_PROTOCOL, XIAOTAI_ERR_INVALID_SIZE). Off: it is ignored, and a dropped
 * session reply ends the conversation by the handshake deadline. */
#define XIAOTAI_AI_TALK_DROPPED_ENDS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AI_TALK_DROPPED_ENDS, 0)
