// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xmini_key.h"

typedef enum {
    XMINI_PAGE_HOME,
    XMINI_PAGE_AI,
    XMINI_PAGE_WECHAT,
    XMINI_PAGE_DEVICE,
    XMINI_PAGE_SETTINGS,
    XMINI_PAGE_AUDIO,
    XMINI_PAGE_GUIDE,
    XMINI_PAGE_SESSION,
    XMINI_PAGE_GROUP,
    XMINI_PAGE_LIVE,
    XMINI_PAGE_GROUP_JOIN,
    XMINI_PAGE_PENDING,
    XMINI_PAGE_CONTACT_DECISION,
    XMINI_PAGE_WIFI_CONFIRM,
    XMINI_PAGE_GROUP_PASSWORD,
} xmini_page_t;

typedef enum {
    XMINI_INTENT_NONE,
    XMINI_INTENT_VOLUME,
    XMINI_INTENT_GAIN,
    XMINI_INTENT_TONE,
    XMINI_INTENT_RECORD,
    XMINI_INTENT_PLAY,
    XMINI_INTENT_STOP,
    XMINI_UI_AI_START,
    XMINI_UI_CALL_CONTACT,
    XMINI_UI_REFRESH_CONTACTS,
    XMINI_UI_ACCEPT,
    XMINI_UI_REJECT,
    XMINI_UI_HANGUP,
    XMINI_UI_INTERRUPT,
    XMINI_UI_SUBMIT,
    XMINI_UI_DISMISS,
    XMINI_UI_GROUP_REFRESH,
    XMINI_UI_GROUP_CREATE,
    XMINI_UI_GROUP_JOIN,
    XMINI_UI_GROUP_CONNECT,
    XMINI_UI_GROUP_DISCONNECT,
    XMINI_UI_GROUP_LEAVE,
    XMINI_UI_GROUP_TALK,
    XMINI_UI_CONTACT_REFRESH,
    XMINI_UI_CONTACT_ACCEPT,
    XMINI_UI_CONTACT_REJECT,
    XMINI_UI_WIFI_SETUP,
    XMINI_UI_WIFI_CANCEL,
    XMINI_UI_CONTACT_PAGE,
    XMINI_UI_PENDING_PAGE,
    XMINI_UI_GUIDANCE,
} xmini_intent_kind_t;

typedef struct {
    xmini_intent_kind_t kind;
    int value;
} xmini_intent_t;

typedef enum { XMINI_UI_IDLE, XMINI_UI_CONNECTING, XMINI_UI_RINGING,
    XMINI_UI_ACTIVE, XMINI_UI_ENDING, XMINI_UI_ERROR } xmini_ui_session_phase_t;

typedef struct {
    xmini_page_t page;
    uint32_t epoch;
    unsigned selected;
    int volume;
    int gain_db;
    bool audio_ready;
    bool audio_busy;
    bool recording_ready;
    bool audio_error;
    unsigned frames;
    uint32_t peak;
    unsigned contact_count;
    char contact_name[65];
    char contact_peer[65];
    char peer[65];
    char caption[193];
    unsigned session_kind;
    uint32_t session_generation;
    xmini_ui_session_phase_t session_phase;
    int session_error;
    bool rtc_online, platform_online, maintenance;
    uint32_t scroll_tick;
    bool group_assigned, group_active, group_speaking;
    char room_code[7];
    char join_code[7];
    unsigned digit;
    unsigned pending_count;
    char pending_name[65], pending_peer[65];
    bool pending_present;
    unsigned contact_page, contact_pages, pending_page, pending_pages;
    bool contacts_loading, pending_loading;
    int contacts_error, pending_error;
    bool group_use_password, group_create;
    char group_password[5];
    bool wifi_setup_pending, settings_saved, settings_pending;
    bool guidance_enabled;
    int wifi_setup_error, settings_error;
} xmini_ui_t;

void xmini_ui_init(xmini_ui_t *ui);
void xmini_ui_session_identity(xmini_ui_t *ui, unsigned kind, uint32_t generation, const char *peer);
xmini_intent_t xmini_ui_key(xmini_ui_t *ui, xmini_key_event_t key);
void xmini_ui_render(const xmini_ui_t *ui, uint8_t frame[1024]);
