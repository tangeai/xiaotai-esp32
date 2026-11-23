// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/feedback.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"

extern xiaotai_wechat_status_t fake_call;
extern xiaotai_net_state_t fake_network;
extern xiaotai_platform_phase_t fake_platform;
extern bool fake_setup, fake_bound, fake_has_identity;
extern xiaotai_err_t fake_lookup, fake_request;
extern const xiaotai_feedback_table_t *fake_table;
extern const char *fake_action;
extern const char *fake_dial_id;
extern uint32_t fake_session;
extern unsigned fake_refreshes;
extern const char *fake_log[128];
extern size_t fake_used;

void fake_clear(void);
void fake_edges(uint32_t edges);
extern xiaotai_binding_status_t fake_binding;
extern xiaotai_provisioning_status_t fake_provision;
extern xiaotai_environment_status_t fake_environment;
extern xiaotai_err_t fake_error, fake_announce, fake_sound_error;
extern unsigned fake_forgets, fake_begins, fake_cancels, fake_retries, fake_holds, fake_spoken, fake_stops, fake_sound_cancels;
extern bool fake_saved_wifi, fake_saved_identity, fake_sound_pending, fake_direct;
extern xiaotai_feedback_options_t fake_feedback;
void fake_advance(uint32_t ms);
void fake_restart(void);
bool fake_restarted(void);
void fake_binding_done(void *result);
void fake_provision_result(xiaotai_err_t result, bool restart);
