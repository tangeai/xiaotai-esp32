// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

static xiaotai_mutex_t s_list_lock;

xiaotai_err_t room_members_init(room_t *room)
{
    const xiaotai_err_t err = xiaotai_mutex_init(&s_list_lock);
    if (err != XIAOTAI_OK) return err;
    if (room->config.members_max == XIAOTAI_ROOM_NO_MEMBER_LIST) return XIAOTAI_OK;
    room->members.rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, room->config.members_max,
                                          sizeof(*room->members.rows));
    if (room->members.rows) return XIAOTAI_OK;
    xiaotai_mutex_deinit(&s_list_lock);
    return XIAOTAI_ERR_NO_MEM;
}

void room_members_deinit(room_t *room)
{
    xiaotai_mem_free(room->members.rows);
    xiaotai_mutex_deinit(&s_list_lock);
}

size_t xiaotai_room_members(uint16_t offset, xiaotai_room_member_t *out, size_t capacity, uint32_t *revision)
{
    if (revision) *revision = 0;
    if (!room_started()) return 0;
    const room_t *room = room_context();
    (void)xiaotai_mutex_lock(&s_list_lock, XIAOTAI_WAIT_FOREVER);
    size_t count = 0;
    if (offset < room->members.count && out && capacity) {
        count = room->members.count - offset;
        if (count > capacity) count = capacity;
        memcpy(out, room->members.rows + offset, count * sizeof(*out));
    }
    if (revision) *revision = room->members.revision;
    xiaotai_mutex_unlock(&s_list_lock);
    return count;
}

static void changed(room_t *room, uint16_t count, uint16_t listed, uint16_t remote_speaking,
                    bool self_present, bool self_speaking, bool row_changed)
{
    const uint16_t speaking = remote_speaking + (uint16_t)self_speaking;
    if (row_changed || count != room->view.member_count || listed != room->members.count ||
        speaking != room->view.speaking_count || remote_speaking != room->members.remote_speaking) {
        room->view.members_revision = ++room->members.revision;
        room->members.event = true;
    }
    if (remote_speaking != room->members.remote_speaking) room->members.voice_event = true;
    room->members.remote_speaking = remote_speaking;
    room->members.count = listed;
    room->members.self_present = self_present;
    room->members.self_speaking = self_speaking;
    room->view.member_count = count;
    room->view.listed_count = listed;
    room->view.speaking_count = speaking;
    room->view.members_truncated = room->members.rows && listed < count;
}

void room_members_clear(room_t *room)
{
    (void)xiaotai_mutex_lock(&s_list_lock, XIAOTAI_WAIT_FOREVER);
    changed(room, 0, 0, 0, false, false, false);
    room->view.members_ready = false;
    xiaotai_mutex_unlock(&s_list_lock);
}

void room_members_publish(room_t *room)
{
    if (room->members.event) {
        const xiaotai_args_t args = {.value = room->view.member_count};
        room->members.event = xiaotai_event_emit(XIAOTAI_EVENT_ROOM_MEMBERS, &args, NULL, 0) != XIAOTAI_OK;
        if (room->members.event) return;
    }
    if (room->members.voice_event) {
        const xiaotai_args_t args = {.value = room->members.remote_speaking};
        room->members.voice_event = xiaotai_event_emit(XIAOTAI_EVENT_ROOM_VOICE, &args, NULL, 0) != XIAOTAI_OK;
    }
}

static bool read_mic(const xiaotai_json_doc_t *doc, int index, xiaotai_room_mic_t *mic)
{
    if (xiaotai_json_equals(doc, index, "off")) *mic = XIAOTAI_ROOM_MIC_OFF;
    else if (xiaotai_json_equals(doc, index, "on")) *mic = XIAOTAI_ROOM_MIC_ON;
    else if (xiaotai_json_equals(doc, index, "speaking")) *mic = XIAOTAI_ROOM_MIC_SPEAKING;
    else return false;
    return true;
}

static bool decode(const xiaotai_json_doc_t *doc, int object, const char *device, bool speaking, bool self_header,
                   xiaotai_room_member_t *row, int *id)
{
    memset(row, 0, sizeof(*row));
    *id = xiaotai_json_get(doc, object, "participant_id");
    if ((!self_header || *id >= 0) &&
        (xiaotai_json_string(doc, *id, row->participant_id, sizeof(row->participant_id), NULL) != XIAOTAI_OK ||
         !row->participant_id[0])) return false;
    const int dev = xiaotai_json_get(doc, object, "device_id");
    if (dev >= 0 && xiaotai_json_string(doc, dev, row->device_id, sizeof(row->device_id), NULL) != XIAOTAI_OK)
        return false;
    row->self = strcmp(row->device_id, device) == 0;
    const int name = xiaotai_json_get(doc, object, "device_name");
    if (name >= 0 && xiaotai_json_string(doc, name, row->name, sizeof(row->name), NULL) != XIAOTAI_OK)
        return false;
    const int mic = xiaotai_json_get(doc, object, "mic_state");
    if (mic >= 0 && !read_mic(doc, mic, &row->mic)) return false;
    if (row->self) row->mic = speaking ? XIAOTAI_ROOM_MIC_SPEAKING : XIAOTAI_ROOM_MIC_OFF;
    return true;
}

bool room_members_snapshot(room_t *room, const xiaotai_json_doc_t *doc, int params)
{
    const int list = xiaotai_json_get(doc, params, "participants");
    if (list < 0 || doc->tokens[list].type != XIAOTAI_JSON_ARRAY ||
        doc->tokens[list].children > XIAOTAI_ROOM_MEMBERS_LIMIT) return false;
    const uint16_t declared = doc->tokens[list].children;
    const int self = xiaotai_json_get(doc, params, "self");
    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];
    if (xiaotai_identity_device_id(device, sizeof(device)) != XIAOTAI_OK || !device[0]) return false;
    xiaotai_room_member_t *row = &room->members.scratch;
    int self_id;
    if (!decode(doc, self, device, room->view.transmitting, true, row, &self_id) || !row->self) return false;
    uint16_t ids[XIAOTAI_ROOM_MEMBERS_LIMIT];
    uint16_t remote_speaking = 0;
    int name = xiaotai_json_get(doc, self, "device_name");
    const bool speaking = room->view.transmitting;
    bool has_self = false;
    /* Compare decoded IDs. Different escape spellings cannot create two
     * entries for one participant, even on a count-only board. */
    for (uint16_t i = 0; i < declared; ++i) {
        int id;
        const int member = xiaotai_json_at(doc, list, i);
        if (!decode(doc, member, device, speaking, false, row, &id)) return false;
        const bool same_as_self = self_id < 0 ? row->self : xiaotai_json_equals(doc, self_id, row->participant_id);
        if (same_as_self != row->self || (same_as_self && has_self)) return false;
        if (same_as_self && name < 0) name = xiaotai_json_get(doc, member, "device_name");
        has_self |= same_as_self;
        for (uint16_t j = 0; j < i; ++j)
            if (xiaotai_json_equals(doc, ids[j], row->participant_id)) return false;
        ids[i] = (uint16_t)id;
        if (!row->self && row->mic == XIAOTAI_ROOM_MIC_SPEAKING) ++remote_speaking;
    }
    /* Older snapshots omit the ID in `self` when its row is already in the
     * array. Still require one unambiguous local row, or an ID to insert. */
    if (!has_self && self_id < 0) return false;
    const uint16_t total = declared + (uint16_t)!has_self;
    if (total > XIAOTAI_ROOM_MEMBERS_LIMIT) return false;
    const uint16_t limit = room->config.members_max;
    const uint16_t listed = !room->members.rows ? 0 : total < limit ? total : limit;
    bool row_changed = false;
    /* The document is immutable. Validate it all before replacing the old
     * table, then decode only as many rows as this product keeps. */
    (void)xiaotai_mutex_lock(&s_list_lock, XIAOTAI_WAIT_FOREVER);
    for (uint16_t i = 0; i < listed; ++i) {
        int id;
        (void)decode(doc, i < declared ? xiaotai_json_at(doc, list, i) : self, device, speaking,
                     i == declared, row, &id);
        if (i >= room->members.count || memcmp(row, room->members.rows + i, sizeof(*row)) != 0) {
            memcpy(room->members.rows + i, row, sizeof(*row));
            row_changed = true;
        }
    }
    changed(room, total, listed, remote_speaking, true, speaking, row_changed);
    if (room->view.members_truncated) room->stats.members_dropped += total - listed;
    room->view.members_ready = true;
    if (name >= 0) {
        memset(room->view.self_name, 0, sizeof(room->view.self_name));
        (void)xiaotai_json_string(doc, name, room->view.self_name, sizeof(room->view.self_name), NULL);
    }
    xiaotai_mutex_unlock(&s_list_lock);
    return true;
}

void room_members_delta(room_t *room, const xiaotai_json_doc_t *doc, int params, bool leaving)
{
    char id[XIAOTAI_ROOM_PARTICIPANT_MAX];
    xiaotai_room_mic_t mic = XIAOTAI_ROOM_MIC_OFF;
    if (xiaotai_json_string(doc, xiaotai_json_get(doc, params, "participant_id"), id, sizeof(id), NULL) != XIAOTAI_OK ||
        !id[0] || (!leaving && !read_mic(doc, xiaotai_json_get(doc, params, "mic_state"), &mic))) {
        room_snapshot_request(room, true);
        return;
    }
    (void)xiaotai_mutex_lock(&s_list_lock, XIAOTAI_WAIT_FOREVER);
    uint16_t i = 0;
    while (i < room->members.count && strcmp(id, room->members.rows[i].participant_id) != 0) ++i;
    const bool unknown = i == room->members.count;
    const bool truncated = room->view.members_truncated;
    if (!unknown) {
        xiaotai_room_member_t *row = room->members.rows + i;
        uint16_t remote = room->members.remote_speaking;
        const bool was_speaking = row->mic == XIAOTAI_ROOM_MIC_SPEAKING;
        if (leaving) {
            const bool self = row->self;
            if (!self && was_speaking && remote) --remote;
            const uint16_t listed = room->members.count - 1U;
            memmove(row, row + 1, (listed - i) * sizeof(*row));
            changed(room, room->view.member_count - 1U, listed, remote,
                    room->members.self_present && !self, room->members.self_speaking && !self, true);
        } else if (!row->self && row->mic != mic) {
            if (was_speaking && remote) --remote;
            if (mic == XIAOTAI_ROOM_MIC_SPEAKING) ++remote;
            row->mic = mic;
            changed(room, room->view.member_count, room->members.count, remote,
                    room->members.self_present, room->members.self_speaking, true);
        }
    }
    xiaotai_mutex_unlock(&s_list_lock);
    if (unknown || (leaving && truncated)) room_snapshot_request(room, true);
}

void room_members_self_mic(room_t *room, bool speaking)
{
    (void)xiaotai_mutex_lock(&s_list_lock, XIAOTAI_WAIT_FOREVER);
    if (room->members.self_present && room->members.self_speaking != speaking) {
        for (uint16_t i = 0; i < room->members.count; ++i)
            if (room->members.rows[i].self)
                room->members.rows[i].mic = speaking ? XIAOTAI_ROOM_MIC_SPEAKING : XIAOTAI_ROOM_MIC_OFF;
        changed(room, room->view.member_count, room->members.count, room->members.remote_speaking, true, speaking, false);
    }
    xiaotai_mutex_unlock(&s_list_lock);
}
