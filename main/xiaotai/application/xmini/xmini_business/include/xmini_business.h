#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "demo_platform.h"
#include "xmini_rtc.h"
#include "xmini_group.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XMINI_BUSINESS_CONTACTS 8
typedef enum {
    XMINI_BUSINESS_NONE,
    XMINI_BUSINESS_AI,
    XMINI_BUSINESS_DEVICE,
    XMINI_BUSINESS_WECHAT,
    XMINI_BUSINESS_LIVE,
    XMINI_BUSINESS_GROUP
} xmini_business_owner_t;

typedef enum {
    XMINI_BUSINESS_IDLE,
    XMINI_BUSINESS_FETCHING,
    XMINI_BUSINESS_OUTGOING,
    XMINI_BUSINESS_RINGING,
    XMINI_BUSINESS_CONNECTING,
    XMINI_BUSINESS_SESSION,
    XMINI_BUSINESS_ACTIVE,
    XMINI_BUSINESS_ENDING,
    XMINI_BUSINESS_ERROR
} xmini_business_phase_t;

typedef enum {
    XMINI_INTENT_AI_START,
    XMINI_INTENT_DEVICE_CALL,
    XMINI_INTENT_WECHAT_CALL,
    XMINI_INTENT_ACCEPT,
    XMINI_INTENT_REJECT,
    XMINI_INTENT_HANGUP,
    XMINI_INTENT_REFRESH_CONTACTS,
    XMINI_INTENT_AI_INTERRUPT,
    XMINI_INTENT_AI_SUBMIT_SPEECH,
    XMINI_INTENT_DISMISS_ERROR
} xmini_business_intent_t;

typedef enum {
    XMINI_CONTACT_REQUEST, XMINI_CONTACT_ACCEPT, XMINI_CONTACT_REJECT,
    XMINI_CONTACT_REMARK, XMINI_CONTACT_DELETE, XMINI_CONTACT_REFRESH
} xmini_contact_action_t;

typedef enum {
    XMINI_CONTACT_ALL, XMINI_CONTACT_DEVICE, XMINI_CONTACT_WECHAT
} xmini_contact_filter_t;

typedef struct {
    char id[65];
    char name[65];
    char app_id[65];
    bool wechat;
} xmini_business_contact_t;

typedef struct {
    xmini_business_owner_t owner;
    xmini_business_phase_t phase;
    uint32_t generation;
    bool rtc_online, platform_online, profile_ready, media_active, speaking, caption_truncated, maintenance;
    uint8_t tx_stream;
    int error;
    unsigned revision;
    unsigned contacts_count, contacts_total;
    xmini_contact_filter_t contacts_filter;
    unsigned contacts_page, contacts_filtered_total;
    bool contacts_loading;
    int contacts_error;
    unsigned pending_count, pending_total;
    unsigned pending_page;
    bool pending_loading;
    int pending_error;
    char pending[4][65];
    xmini_business_contact_t contacts[XMINI_BUSINESS_CONTACTS];
    char peer[65], room[129], caption[193], reason[65];
    uint32_t platform_dropped, stale_events;
    xmini_group_status_t group;
} xmini_business_status_t;

typedef enum {
    XMINI_COMMAND_DISPATCH_NONE,
    XMINI_COMMAND_DISPATCH_STALE_GENERATION,
    XMINI_COMMAND_DISPATCH_ENDING,
    XMINI_COMMAND_DISPATCH_OTHER,
    XMINI_COMMAND_DISPATCH_INVALID_OR_ROOM_MISMATCH,
    XMINI_COMMAND_DISPATCH_SUBSCRIBE_ERROR,
    XMINI_COMMAND_DISPATCH_ACTIVATION_BRANCH
} xmini_business_command_disposition_t;

typedef struct {
    uint32_t sequence, generation, connection32, word, disposition;
    int32_t error;
} xmini_business_command_diagnostics_t;

typedef struct {
    xmini_rtc_receiver_t receive;
    /* Start may return BUSY until an earlier local prompt has actually stopped. */
    int (*media_start)(uint32_t generation, uint8_t tx_stream, void *context);
    int (*media_stop)(uint32_t generation, void *context);
    bool (*media_busy)(uint32_t generation, void *context);
    /* Interrupt clears queued playback while preserving the active microphone. */
    void (*media_flush)(uint32_t generation, void *context);
    void *context;
} xmini_business_callbacks_t;

/* One >=16 KiB control task owns init/process and all SDK calls. No extra task
 * is created. Platform and UI callers only post copied, bounded queue items. */
int xmini_business_init(const xmini_business_callbacks_t *callbacks);
void xmini_business_process(void);
bool xmini_business_platform_event(const platform_event_t *event);
int xmini_business_post_intent(xmini_business_intent_t intent, const char *target);
/* Session actions carry the identity displayed by the UI, checked both before
 * enqueue and at consumption. Generation zero is only valid for dismissing an
 * ownerless terminal error; the compatibility API binds its current snapshot. */
int xmini_business_post_session_intent(xmini_business_intent_t intent, uint32_t expected_generation);
int xmini_business_post_group(xmini_group_action_t action, const char *room_code,
                               const char *password);
/* REQUEST target=device ID; ACCEPT/REJECT target=pending peer_device_id;
 * REMARK target=peer_id and value=UTF-8 <=64 Unicode characters; DELETE only
 * manual device contacts (the service enforces auto/VoIP protection).
 * UI must present ACCEPT and REJECT explicitly; closing a popup does neither. */
int xmini_business_post_contact(xmini_contact_action_t action, const char *target,
                                 const char *value);
/* Zero-based client windows over a fresh authoritative GET response. The
 * service has no paging query. Only eight contacts/four pending IDs stay in RAM;
 * stale in-flight windows never overwrite a more recent selection. */
int xmini_business_post_contacts_page(xmini_contact_filter_t filter, unsigned page);
int xmini_business_post_pending_page(unsigned page);
/* Control task only. Exclusive idle admission for explicit Wi-Fi setup; MQTT stays online. */
int xmini_business_maintenance(bool active);
void xmini_business_status(xmini_business_status_t *status);
/* Last delivered COMMAND only; zero sequence means none, wrapping skips zero.
 * OTHER includes normally handled non-DEV routes. ACTIVATION_BRANCH means the
 * existing DEV activation call ran, not that media became ACTIVE. Error is the
 * DEV subscribe return; other dispositions carry zero. No payload is retained. */
void xmini_business_command_diagnostics(xmini_business_command_diagnostics_t *diagnostics);
/* Nonblocking audio-task bridge, using current business generation/stream. */
int xmini_business_send_audio(uint32_t generation, const uint8_t *pcma,
                              size_t length, uint32_t pts);

#ifdef __cplusplus
}
#endif
