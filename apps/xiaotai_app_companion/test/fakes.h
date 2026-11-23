// SPDX-License-Identifier: MIT
#pragma once

#include "companion_internal.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"

typedef struct {
    uint32_t needs;
    const xiaotai_interaction_profile_t *profile;
    const xiaotai_feedback_table_t *feedback;
    char id[16], context[24], toast[XIAOTAI_ARG_TEXT_MAX], toast_id[32];
    char title[64], text[256], subtitle[96], status[64], emotion[24];
    char overlay_title[64], overlay_body[128];
    uint8_t kind, phase, level, speaker;
    uint8_t progress, timer_mode;
    uint32_t toast_ms, flags;
    int32_t timer_s;
    uint16_t focus;
    unsigned views, toasts, refused, page_events, main_requests, main_arg, overlays;
    unsigned talk_stops;
    bool fail_talk_stop;
    bool navigation_pages;
    bool board_settings, screen_asleep, sleep_fail, save_pending;
    bool (*read_save_pending)(void);
    unsigned setting_int_writes, choice_confirms;
    char confirmed_setting[XIAOTAI_ARG_TEXT_MAX];
    int32_t confirmed_value;
    xiaotai_err_t confirm_result;
    uint32_t sleep_ms, sleep_requests;
    size_t item_count, action_count, field_count;
    xiaotai_view_item_t items[XIAOTAI_VIEW_ITEMS_MAX];
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
    companion_home_session_t home_session;
    xiaotai_args_t last_refusal;
    xiaotai_provisioning_status_t provision;
    xiaotai_net_status_t net;
    xiaotai_binding_status_t binding;
    xiaotai_platform_status_t platform;
    unsigned binding_retries, binding_announcements, provisioning_retries, provisioning_begins, provisioning_cancels;
    uint32_t provisioning_quiesce;
    bool provisioning_begin_cancellable;
    int32_t provisioning_begin_flags;
    bool rtc_preparing, saved_credentials;
    unsigned activity_notes, invalidations;
} companion_test_fixture_t;

extern companion_test_fixture_t fixture;
xiaotai_err_t companion_fixture_init(void);
