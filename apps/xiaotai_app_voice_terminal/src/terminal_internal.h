// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/apps/voice_terminal.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/wechat.h"

/* The rules set this private marker; public terminal intents have no args. */
#define TERMINAL_GESTURE_VALUE 1

typedef enum { TERMINAL_IDLE, TERMINAL_RINGING, TERMINAL_BUSY, TERMINAL_CLOSING } terminal_situation_t;

typedef struct {
    bool initialized, ring_silenced, network_error, reset_pending, outgoing, releasing, gesture_pending, portal_error;
    uint32_t gesture_number, gesture_session, key_answer, call_session;
    terminal_situation_t gesture_situation;
    xiaotai_wechat_state_t call_state;
    const char *light;
    const char *ring;
    uint32_t accepted, refused, stale, blocked, superseded;
} terminal_state_t;

extern xiaotai_app_voice_terminal_config_t terminal_config;
extern terminal_state_t terminal;

terminal_situation_t terminal_situation(const xiaotai_wechat_status_t *call);
void terminal_refuse(const char *intent, xiaotai_err_t result, bool stale);
xiaotai_err_t terminal_key_init(void);
xiaotai_err_t terminal_state_init(void);
xiaotai_err_t terminal_tables_init(void);
void terminal_show(const xiaotai_wechat_status_t *call);
void terminal_refresh(void);
void terminal_silence_ring(void);

#define TERMINAL_CONFIRM_AUDIO "terminal.confirm.audio"
xiaotai_err_t terminal_reset_init(void);
void terminal_reset_request(bool network);
void terminal_reset_teardown(void);
void terminal_reset_environment_result(bool cancelled, xiaotai_err_t error);
typedef struct {
    xiaotai_err_t (*init)(void);
    xiaotai_err_t (*prepare)(bool *waiting);
} terminal_environment_t;
extern const terminal_environment_t terminal_with_environment;

xiaotai_err_t terminal_binding_init(void);
void terminal_binding_suspend(void);
void terminal_binding_resume(void);
void terminal_binding_teardown(void);
