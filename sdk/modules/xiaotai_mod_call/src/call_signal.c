// SPDX-License-Identifier: MIT
#include "call_internal.h"
#include <string.h>

bool call_string(const xiaotai_json_view_t *value, char *text, size_t capacity)
{
    if (xiaotai_json_view_string(value, text, capacity, NULL) == XIAOTAI_OK && text[0]) return true;
    text[0] = '\0';
    return false;
}

static void signal_received(const xiaotai_platform_signal_t *signal, void *context)
{
    (void)context;
    if (!signal || !signal->type || (signal->channel && !strcmp(signal->channel, "wx"))) return;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE || !call_binding_current(signal->generation)) return;
    xiaotai_json_view_t doc, payload, item;
    if (!call_parse(signal->json, signal->size, &doc) || !xiaotai_json_view_get(&doc, "payload", &payload))
        return;
    char room[CALL_ROOM_SIZE];
    if (!xiaotai_json_view_get(&payload, "room_id", &item) || !call_string(&item, room, sizeof(room))) return;
    if (!strcmp(signal->type, "call_incoming")) {
        char peer[XIAOTAI_CALL_PEER_ID_MAX], name[XIAOTAI_CALL_PEER_NAME_MAX];
        if (!xiaotai_json_view_get(&payload, "caller_id", &item) || !call_string(&item, peer, sizeof(peer)))
            return;
        (void)xiaotai_json_view_get(&payload, "caller_name", &item);
        (void)call_string(&item, name, sizeof(name));
        const bool has_type = xiaotai_json_view_get(&payload, "call_type", &item);
        const bool video = xiaotai_json_view_equals(&item, "video");
        const bool unsupported = has_type && !video && !xiaotai_json_view_equals(&item, "voice") &&
                                 !xiaotai_json_view_equals(&item, "audio") && !xiaotai_json_view_equals(&item, "");
        call_incoming(room, peer, name, video, unsupported);
    } else if (!strcmp(signal->type, "callee_answered")) {
        char peer[XIAOTAI_CALL_PEER_ID_MAX];
        (void)xiaotai_json_view_get(&payload, "callee_id", &item);
        (void)call_string(&item, peer, sizeof(peer));
        call_cloud_answer(room, peer);
    } else {
        char reason[24] = "";
        if (xiaotai_json_view_get(&payload, "reason", &item)) (void)call_string(&item, reason, sizeof(reason));
        call_cloud_end(room, !strcmp(signal->type, "call_reject"), reason);
    }
}

xiaotai_err_t call_signals_register(void)
{
    static const char *const types[] = {"call_incoming", "callee_answered", "room_cancel",
                                      "call_cancel", "call_reject"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        const xiaotai_err_t err = xiaotai_platform_signal_subscribe(types[i], NULL, signal_received, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t call_send_answer(uint32_t link, const char *room)
{
    char body[CALL_COMMAND_BYTES];
    size_t size;
    xiaotai_json_writer_t w;
    xiaotai_json_writer_init(&w, body, sizeof(body));
    xiaotai_json_object_begin(&w, NULL);
    xiaotai_json_put_string(&w, "room_id", room);
    xiaotai_json_object_end(&w);
    const xiaotai_err_t err = xiaotai_json_writer_end(&w, &size);
    return err == XIAOTAI_OK ? xiaotai_link_command(link, CALL_COMMAND_ANSWER, body, size) : err;
}
