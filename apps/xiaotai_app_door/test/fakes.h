// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/stream_video.h"
#include "xiaotai/modules/video_call.h"
#include "xiaotai/platform.h"
#include "xiaotai/net.h"
#include "xiaotai/ui.h"

extern xiaotai_net_status_t fake_net;
extern xiaotai_platform_status_t fake_platform;
extern xiaotai_call_status_t fake_call;
extern xiaotai_video_call_status_t fake_video_call;
extern xiaotai_stream_video_status_t fake_stream_video;
extern xiaotai_binding_status_t fake_binding;
extern xiaotai_provisioning_status_t fake_provisioning;
extern xiaotai_ui_info_t fake_ui;
extern xiaotai_contacts_status_t fake_list;
extern xiaotai_contact_t fake_contacts[3];
extern unsigned fake_refreshes;
extern char fake_saved_peer[65];
extern unsigned fake_ring_in, fake_ring_out, fake_activity;
extern unsigned fake_calls, fake_mics, fake_mic_saves, fake_volume_saves, fake_stream_stops, fake_lock_calls;
extern unsigned fake_stream_stop_attempts;
extern uint32_t fake_control_session, fake_stream_generation, fake_stop_generation;
extern xiaotai_err_t fake_control_error, fake_preview_error, fake_lock_error, fake_mic_error;
extern xiaotai_err_t fake_stream_stop_error;
extern bool fake_requested_mute, fake_saved_mute, fake_previewing;
extern bool fake_saved_sender, fake_save_pending, fake_feedback_pending;
extern unsigned fake_role_saves, fake_open_announcements;
extern xiaotai_err_t fake_announce_error;
extern uint8_t fake_volume, fake_preview, fake_lock_index, fake_volume_step;
extern size_t fake_lock_count;
extern const char *fake_verb;
#ifdef XIAOTAI_DOOR_WITH_WECHAT
#include "xiaotai/modules/wechat.h"
extern xiaotai_wechat_status_t fake_wechat;
extern unsigned fake_wechat_calls;
#endif
void fake_advance_ms(unsigned time_ms);
xiaotai_err_t fake_store_init(void);
