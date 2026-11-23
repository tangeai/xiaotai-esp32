// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/link.h"
#include "xiaotai/media.h"
#include "xiaotai/transport.h"

typedef struct {
    bool online, can_connect, permitted, opened, media_open, mic_muted, gate_open, gate_on_owner;
    xiaotai_err_t connect_error, command_error, media_error, gate_error, mic_command_error;
    uint32_t gate_tail_ms, gate_open_calls, gate_close_calls;
    uint32_t generation, connects, disconnects;
    unsigned connecting_attempts_at_connect;
    xiaotai_link_request_t profile;
    xiaotai_link_handlers_t handlers;
    char peer[XIAOTAI_LINK_PEER_MAX], token[XIAOTAI_LINK_TOKEN_MAX];
    xiaotai_media_session_t session;
    char commands[32][512];
    size_t commands_count;
    char steps[128];
    size_t step_count;
} fake_link_t;

extern fake_link_t fake_link;
void fake_step(char step);
void fake_open(void);
void fake_close(xiaotai_link_end_t why);
void fake_command(const char *data);
void fake_command_gen(uint32_t generation, const char *data, bool dropped);
