// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <string.h>
#include "xiaotai/port/mem.h"

#define ROOM_COMMAND 0x2200U
#define SNAPSHOT_COALESCE_MS 200U
#define SNAPSHOT_INTERVAL_MS 500U
#define MEMBERS_MAX_AGE_MS 45000U
#define SNAPSHOT_MAX_TOKENS 2048U
#define NOTICE_MAX_BYTES 2048U
#define NOTICE_MAX_TOKENS 64U

void room_snapshot_request(room_t *room, bool coalesce)
{
    room_connection_t *link = &room->link;
    if (link->stage != ROOM_LINK_ACTIVE) return;
    int64_t due = room_now_ms() + (coalesce ? SNAPSHOT_COALESCE_MS : 0U);
    if (due < link->snapshot_not_before) due = link->snapshot_not_before;
    if (!due) due = 1;
    if (!link->snapshot_at || due < link->snapshot_at) link->snapshot_at = due;
}

int64_t room_snapshot_due(const room_t *room)
{
    const room_connection_t *link = &room->link;
    if (!link->snapshot_at) return link->members_until ? link->members_until : INT64_MAX;
    if (!link->members_until) return link->snapshot_at;
    return link->snapshot_at < link->members_until ? link->snapshot_at : link->members_until;
}

void room_snapshot_drive(room_t *room, int64_t now)
{
    room_connection_t *link = &room->link;
    if (link->members_until && now >= link->members_until) {
        link->members_until = 0;
        room->view.members_ready = false;
        room_snapshot_request(room, false);
    }
    if (link->snapshot_at && now >= link->snapshot_at && now >= link->snapshot_not_before) {
        link->snapshot_at = 0;
        link->snapshot_not_before = now + SNAPSHOT_INTERVAL_MS;
        (void)room_connection_send(room, "get_room_snapshot", false);
    }
}

static void too_large(room_t *room)
{
    ++room->stats.commands_dropped;
    if (room->link.stage == ROOM_LINK_JOIN) room_connection_fail(room, XIAOTAI_ERR_INVALID_SIZE, 0, false);
    else if (room->link.stage == ROOM_LINK_ACTIVE) {
        if (++room->link.oversized >= 2U) room_connection_fail(room, XIAOTAI_ERR_INVALID_SIZE, 0, true);
        else room_snapshot_request(room, false);
    }
}

static void reject_snapshot(room_t *room)
{
    ++room->stats.snapshots_rejected;
    room_connection_fail(room, XIAOTAI_ERR_INVALID_SIZE, 0, true);
}

static bool scope_matches(const room_t *room, const char *scope)
{
    if (room->link.signal_room[0]) return strcmp(scope, room->link.signal_room) == 0;
    const char *business = room->view.room_id;
    const size_t len = strlen(scope), id_len = strlen(business);
    return strcmp(scope, business) == 0 ||
           (len > id_len + 1U && scope[len - id_len - 1U] == ':' && strcmp(scope + len - id_len, business) == 0);
}

static void notice(room_t *room, const xiaotai_json_doc_t *doc, bool snapshot)
{
    room_connection_t *link = &room->link;
    const int method = xiaotai_json_get(doc, 0, "method");
    const int params = xiaotai_json_get(doc, 0, "params");
    const bool closed = xiaotai_json_equals(doc, method, "room_closed");
    const bool joined = xiaotai_json_equals(doc, method, "participant_joined");
    const bool left = xiaotai_json_equals(doc, method, "participant_left");
    const bool mic = xiaotai_json_equals(doc, method, "participant_mic_state_changed");
    if (!snapshot && !closed && !joined && !left && !mic) return;
    if (!snapshot && !link->signal_room[0]) {
        room_snapshot_request(room, true);
        return;
    }
    const int id = xiaotai_json_get(doc, params, "room_id");
    /* No stack buffer and no borrow of the received payload beyond this callback. */
    char *scope = link->command;
    if (xiaotai_json_string(doc, id, scope, sizeof(link->signal_room), NULL) != XIAOTAI_OK || !scope[0]) {
        if (snapshot) reject_snapshot(room);
        return;
    }
    if (!scope_matches(room, scope)) {
        ++room->stats.stale;
        return;
    }
    if (snapshot) {
        if (!room_members_snapshot(room, doc, params)) { reject_snapshot(room); return; }
        if (!link->signal_room[0]) memcpy(link->signal_room, scope, strlen(scope) + 1U);
        ++room->stats.snapshots;
        link->snapshot_at = 0;
        link->members_until = room_now_ms() + MEMBERS_MAX_AGE_MS;
    } else if (closed) {
        room_connection_fail(room, XIAOTAI_ERR_NOT_FOUND, XIAOTAI_ROOM_CODE_NOT_FOUND, false);
    } else {
        if (!room->members.rows) room->view.members_ready = false;
        if (joined) room_snapshot_request(room, true);
        else room_members_delta(room, doc, params, left);
    }
}

static xiaotai_err_t read_command(room_t *room, const char *text, size_t size, uint32_t generation)
{
    room_connection_t *link = &room->link;
    size_t tokens = 0, members = 0;
    const xiaotai_err_t counted = xiaotai_json_parse(text, size, NULL, 0, &tokens, NULL);
    if (counted != XIAOTAI_OK) {
        if (counted == XIAOTAI_ERR_INVALID_SIZE || size > NOTICE_MAX_BYTES) too_large(room);
        else if (link->stage == ROOM_LINK_JOIN) return XIAOTAI_FAIL;
        return XIAOTAI_OK;
    }
    if (link->stage == ROOM_LINK_ACTIVE && (tokens > NOTICE_MAX_TOKENS || size > NOTICE_MAX_BYTES)) {
        char method[sizeof("room_snapshot")];
        if (xiaotai_json_text_string(text, size, "method", method, sizeof(method), NULL) != XIAOTAI_OK ||
            strcmp(method, "room_snapshot") != 0) {
            too_large(room);
            return XIAOTAI_OK;
        }
        if (tokens > SNAPSHOT_MAX_TOKENS ||
            xiaotai_json_count(text, size, "params.participants", &members) != XIAOTAI_OK ||
            members > XIAOTAI_ROOM_MEMBERS_LIMIT) {
            /* A bad snapshot from an old namespace never ends this room. */
            char *scope = link->command;
            if (xiaotai_json_text_string(text, size, "params.room_id", scope, sizeof(link->signal_room), NULL) == XIAOTAI_OK &&
                scope[0] && !scope_matches(room, scope)) ++room->stats.stale;
            else reject_snapshot(room);
            return XIAOTAI_OK;
        }
    } else if (link->stage == ROOM_LINK_JOIN && (tokens > NOTICE_MAX_TOKENS || size > NOTICE_MAX_BYTES)) {
        too_large(room);
        return XIAOTAI_OK;
    }
    xiaotai_json_token_t *table = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, tokens * sizeof(*table));
    if (!table) return XIAOTAI_ERR_NO_MEM;
    xiaotai_json_doc_t doc;
    xiaotai_err_t err = xiaotai_json_parse(text, size, table, tokens, &tokens, &doc);
    if (err == XIAOTAI_OK && doc.tokens[0].type == XIAOTAI_JSON_OBJECT) {
        int64_t id;
        if (link->stage == ROOM_LINK_JOIN &&
            xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "id"), &id) == XIAOTAI_OK && id == generation) {
            err = room_connection_accept_join(room, &doc);
        } else if (link->stage == ROOM_LINK_ACTIVE) {
            const bool snapshot = xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "method"), "room_snapshot");
            if (!snapshot && (tokens > NOTICE_MAX_TOKENS || size > NOTICE_MAX_BYTES)) too_large(room);
            else {
                link->oversized = 0;
                notice(room, &doc, snapshot);
            }
        }
    }
    xiaotai_mem_free(table);
    return err;
}

void room_signal_command(void *ctx, uint32_t generation, const xiaotai_link_command_t *message)
{
    room_t *room = ctx;
    room_update_context(room);
    const room_link_t stage = room->link.stage;
    if (generation != room->view.generation) ++room->stats.stale;
    else if ((stage == ROOM_LINK_JOIN || stage == ROOM_LINK_ACTIVE) &&
             xiaotai_link_command_is(message->word, ROOM_COMMAND)) {
        if (room->config.recover_oversized_command && message->size > room->config.command_max_bytes) {
            ++room->stats.commands_dropped;
            room_connection_fail(room, XIAOTAI_ERR_TIMEOUT, 0, false);
        } else if (message->dropped) too_large(room);
        else {
            const xiaotai_err_t err = read_command(room, (const char *)message->data, message->size, generation);
            if (err != XIAOTAI_OK) room_connection_fail(room, err, 0, false);
        }
    }
    room_drive(room);
}
