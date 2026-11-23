// SPDX-License-Identifier: MIT
#include "wechat_internal.h"
#include "xiaotai/identity.h"

#include <stddef.h>
#include <string.h>

#define WX_SIGNAL_CHANNEL "wx"
#define WX_SIGNAL_LIMIT 8192U

static bool signal_text(const xiaotai_json_view_t *field, char *out, size_t capacity, bool required)
{
    out[0] = '\0';
    if (!field->type) return !required;
    if (xiaotai_json_view_string(field, out, capacity, NULL) != XIAOTAI_OK) {
        out[0] = '\0';
        return false;
    }
    return !required || out[0];
}

bool wx_signal_text(const xiaotai_json_view_t *object, const char *key,
                    char *out, size_t capacity, bool required)
{
    xiaotai_json_view_t field;
    (void)xiaotai_json_view_get(object, key, &field);
    return signal_text(&field, out, capacity, required);
}

static wx_type_t room_type(const xiaotai_json_view_t *value)
{
    if (value->type == XIAOTAI_JSON_NULL || xiaotai_json_view_equals(value, "")) return WX_TYPE_UNSET;
    if (xiaotai_json_view_equals(value, "voice") || xiaotai_json_view_equals(value, "audio"))
        return WX_TYPE_VOICE;
    return xiaotai_json_view_equals(value, "video") ? WX_TYPE_VIDEO : WX_TYPE_OTHER;
}

bool wx_decode_room(const xiaotai_json_view_t *payload, wx_room_t *room)
{
    if (payload->type != XIAOTAI_JSON_OBJECT) return false;
    *room = (wx_room_t){0};
    enum {ROOM, PEER, TOKEN, CALL, APP, MODEL, SERVER, PAYLOAD, REMARK};
#define FIELD(key, member) {key, offsetof(wx_room_t, member), sizeof(room->member)}
    static const struct { const char *key; size_t offset, size; } fields[] = {
        [ROOM] = FIELD("wx_room_id", room_id),
        [PEER] = FIELD("peer_id", peer),
        [TOKEN] = FIELD("token", token),
        [CALL] = FIELD("wx_call_id", call_id),
        [APP] = FIELD("wx_app_id", app_id),
        [MODEL] = FIELD("wx_model_id", model_id),
        [SERVER] = FIELD("wx_server_token", server_token),
        [PAYLOAD] = FIELD("wx_payload", payload),
        [REMARK] = FIELD("wx_user_remark", remark),
    };
#undef FIELD
    /* Preserve legacy key priority independently of the order on the wire. */
    static const char *const type_keys[] = {"wx_room_type", "wxa_room_type", "room_type", "media_type"};
    wx_type_t types[sizeof(type_keys) / sizeof(type_keys[0])] = {WX_TYPE_UNSET};
    const uint32_t required = (1U << ROOM) | (1U << PEER) | (1U << TOKEN);
    const uint32_t refusal = (1U << APP) | (1U << MODEL) | (1U << SERVER) | (1U << PAYLOAD);
    uint32_t seen = 0, invalid = 0, seen_types = 0;
    xiaotai_json_view_iter_t it;
    xiaotai_json_view_iter(payload, &it);
    xiaotai_json_view_t key, value;
    while (xiaotai_json_view_next(&it, &key, &value)) {
        size_t t = 0;
        for (; t < sizeof(types) / sizeof(types[0]); ++t) {
            const uint32_t bit = 1U << t;
            if (!xiaotai_json_view_equals(&key, type_keys[t])) continue;
            if (!(seen_types & bit)) types[t] = room_type(&value);
            seen_types |= bit;
            break;
        }
        if (t < sizeof(types) / sizeof(types[0])) continue;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            const uint32_t bit = 1U << i;
            if ((seen & bit) || !xiaotai_json_view_equals(&key, fields[i].key)) continue;
            seen |= bit;
            if (!signal_text(&value, (char *)room + fields[i].offset, fields[i].size, (required & bit) != 0U))
                invalid |= bit;
            break;
        }
    }
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (types[i] == WX_TYPE_UNSET) continue;
        room->type = types[i];
        break;
    }

    /* Rejection credentials are independent of WHIP. Missing or invalid
     * optional values stay absent; they are never truncated into wire values. */
    room->invalid_refusal_fields = (invalid & refusal) != 0U;
    return (seen & required) == required && !(invalid & (required | (1U << CALL)));
}

bool wx_signal_room(const char *json, size_t size, wx_room_t *room)
{
    xiaotai_json_view_t doc, payload;
    return wx_parse(json, size, &doc) == XIAOTAI_OK &&
           xiaotai_json_view_get(&doc, "payload", &payload) && wx_decode_room(&payload, room);
}

static void signal_received(const xiaotai_platform_signal_t *signal, void *ctx)
{
    (void)ctx;
    if (!signal || !signal->type || !signal->channel || strcmp(signal->channel, WX_SIGNAL_CHANNEL) != 0 ||
        !signal->json || !signal->size || signal->size >= WX_SIGNAL_LIMIT) return;
    if (signal->generation != xiaotai_identity_generation()) {
        wx_route_stale();
        return;
    }
    xiaotai_json_view_t doc, payload;
    if (wx_parse(signal->json, signal->size, &doc) != XIAOTAI_OK) return;
    if (!xiaotai_json_view_get(&doc, "payload", &payload) || payload.type != XIAOTAI_JSON_OBJECT) return;
    if (strcmp(signal->type, "call_incoming") == 0) {
        wx_route_room(&payload, signal->json, signal->size);
    } else if (strcmp(signal->type, "call_cancel") == 0 || strcmp(signal->type, "room_cancel") == 0 ||
               strcmp(signal->type, "call_reject") == 0) {
        char id[WX_ID_BYTES];
        if (wx_signal_text(&payload, "wx_room_id", id, sizeof(id), true))
            wx_route_cancel(id, strcmp(signal->type, "call_reject") == 0);
    }
}

xiaotai_err_t wx_signal_init(void)
{
    static const char *const types[] = {"call_incoming", "call_cancel", "room_cancel", "call_reject"};
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i) {
        const xiaotai_err_t error = xiaotai_platform_signal_subscribe(types[i], WX_SIGNAL_CHANNEL, signal_received, NULL);
        if (error != XIAOTAI_OK) return error;
    }
    return XIAOTAI_OK;
}
