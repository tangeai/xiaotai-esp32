// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/framework.h"
#include "xiaotai/json_view.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/platform.h"

#define WX_ID_BYTES 129U
#define WX_COMMAND_BYTES 1024U
#define WX_COMMAND_POOL_BYTES 2048U

typedef enum {WX_TYPE_UNSET, WX_TYPE_VOICE, WX_TYPE_VIDEO, WX_TYPE_OTHER} wx_type_t;

/* Owned by the room or copied by the consumer; no borrowed JSON storage. */
typedef struct {
    char room_id[WX_ID_BYTES];
    char call_id[WX_ID_BYTES];
    char app_id[WX_ID_BYTES];
    char model_id[WX_ID_BYTES];
    char server_token[257];
    char payload[513];
    char peer[XIAOTAI_LINK_PEER_MAX];
    char token[XIAOTAI_LINK_TOKEN_MAX];
    char remark[XIAOTAI_WECHAT_PEER_NAME_MAX];
    wx_type_t type;
    bool invalid_refusal_fields;
} wx_room_t;

/* Owner entries used by the room router. A matched outgoing room is submitted
 * in the next owner message, never inside the signal parser. */
void wx_announce(const wx_room_t *room);
void wx_join(const wx_room_t *room);
/* Reuse the parsed allocation on boards without a reserved room buffer. */
void wx_room_handoff(wx_room_t **room, bool incoming);
void wx_cancel(const char *room_id, bool rejected);
void wx_early_cancel(bool rejected);
void wx_bad_dial_reply(uint32_t session);
bool wx_same_room(const char *room_id);
bool wx_waiting_room(void);
void wx_route_stale(void);

/* Routing owns correlation IDs, early-room storage and retired-room memory. */
xiaotai_err_t wx_route_init(void);
xiaotai_err_t wx_route_begin(uint32_t session, const char *openid, const char *app_id);
void wx_route_reply(uint32_t session, const xiaotai_platform_reply_t *reply);
void wx_route_end(uint32_t session, const wx_room_t *room);
void wx_route_clear(void);
void wx_route_reset(void);
void wx_route_room(const xiaotai_json_view_t *payload, const char *json, size_t size);
void wx_route_cancel(const char *room, bool rejected);

/* Platform envelopes are borrowed. Decode or copy before the handler returns. */
xiaotai_err_t wx_signal_init(void);
bool wx_signal_room(const char *json, size_t size, wx_room_t *room);
bool wx_decode_room(const xiaotai_json_view_t *payload, wx_room_t *room);
bool wx_signal_text(const xiaotai_json_view_t *object, const char *key,
                    char *out, size_t capacity, bool required);

/* The service layer copies a refusal before returning, including when offline. */
void wx_refuse(const wx_room_t *room, uint8_t reason, bool defer);
xiaotai_err_t wx_refusals_init(void);
void wx_refusals_reset(void);
void wx_refusals_platform(const xiaotai_event_t *event);
void wx_refusal_noted(bool failed);
void wx_refusal_invalid(void);
xiaotai_err_t wx_profile_init(void);
const xiaotai_media_format_t *wx_profile_audio(void);
uint32_t wx_profile_video_fallbacks(void);
void wx_profile_reset(void);
int wx_contact_kind(void);
void wx_diagnostics(uint32_t *extra_video, uint32_t *lost_events, uint32_t *invalid_refusals);
xiaotai_err_t wx_at_init(void);
const xiaotai_media_format_t *wx_audio_format(void);
const xiaotai_wechat_config_t *wx_config(void);
/* Keep the input bytes valid until the last use of the view. */
xiaotai_err_t wx_parse(const char *data, size_t size, xiaotai_json_view_t *root);
