// SPDX-License-Identifier: MIT
/* The chosen counterparts: the door stations of an indoor station, the indoor
 * stations of a door station. Stored in one setting each, kept in memory
 * without gaps or repetitions; the first one is the one looked at from the
 * home screen, or called first. */
#include "door_internal.h"
#include "xiaotai/eventlog.h"

#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"

#include <stdio.h>
#include <string.h>

static const struct { const char *name, *key; } slots[XIAOTAI_APP_DOOR_PEERS_MAX] = {
    {XIAOTAI_SETTING_APP_DOOR_PEER, XIAOTAI_SETTING_KEY_APP_DOOR_PEER},
    {XIAOTAI_SETTING_APP_DOOR_PEER2, XIAOTAI_SETTING_KEY_APP_DOOR_PEER2},
    {XIAOTAI_SETTING_APP_DOOR_PEER3, XIAOTAI_SETTING_KEY_APP_DOOR_PEER3},
    {XIAOTAI_SETTING_APP_DOOR_PEER4, XIAOTAI_SETTING_KEY_APP_DOOR_PEER4},
};

xiaotai_err_t door_peers_register(void (*changed)(const char *name, void *ctx))
{
    for (size_t i = 0; i < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) {
        const xiaotai_setting_t setting = {
            .name = slots[i].name, .key = slots[i].key, .kind = XIAOTAI_SETTING_TEXT,
            .default_text = "", .changed = changed,
        };
        const xiaotai_err_t err = xiaotai_setting_register(&setting);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

bool door_peers_setting(const char *name)
{
    for (size_t i = 0; i < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) {
        if (!strcmp(name, slots[i].name)) return true;
    }
    return false;
}

int door_peer_slot(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < door.peer_count; ++i) {
        if (!strcmp(door.peers[i], id)) return i;
    }
    return -1;
}

/* From the settings (from NULL) or a page's list, without gaps or repetitions. */
static uint8_t gather(door_peer_id_t *peers, const door_peer_id_t *from)
{
    uint8_t count = 0;
    for (size_t i = 0; i < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) {
        char *id = peers[count];
        id[0] = '\0';
        if (from) (void)snprintf(id, XIAOTAI_SETTING_TEXT_MAX, "%s", from[i]);
        else if (xiaotai_setting_get_text(slots[i].name, id, XIAOTAI_SETTING_TEXT_MAX) != XIAOTAI_OK) id[0] = '\0';
        bool known = !id[0];
        for (uint8_t k = 0; k < count; ++k) known = known || !strcmp(peers[k], id);
        if (!known) ++count;
    }
    for (size_t i = count; i < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) memset(peers[i], 0, sizeof(peers[i]));
    return count;
}

void door_peers_stored(door_peer_id_t *out) { (void)gather(out, NULL); }

void door_peers_read(void)
{
    door_peer_id_t peers[XIAOTAI_APP_DOOR_PEERS_MAX];
    const uint8_t count = gather(peers, door_pages_list(door_mode()));
    bool changed = count != door.peer_count;
    for (uint8_t i = 0; i < count && !changed; ++i) changed = strcmp(peers[i], door.peers[i]) != 0;
    if (!changed) return;
    memcpy(door.peers, peers, sizeof(door.peers));
    door.peer_count = count;
    ++door.peer_revision;
}

/* The whole list is worked out before the first write: each write runs the
 * settings' changed() handlers, which read the list again. The first slot is
 * written last, so a failure never leaves it empty. */
static xiaotai_err_t store(char list[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX])
{
    if (door_pages_on()) return door_pages_store(list);
    for (size_t i = XIAOTAI_APP_DOOR_PEERS_MAX; i-- > 0;) {
        char stored[XIAOTAI_SETTING_TEXT_MAX] = {0};
        (void)xiaotai_setting_get_text(slots[i].name, stored, sizeof(stored));
        const bool same = !strcmp(stored, list[i]);
        if (same && (i > 0 || !door_config.save_peer_on_reselect || !list[i][0])) continue;
        const xiaotai_err_t err = same ? xiaotai_settings_confirm_text(slots[i].name, list[i]) :
                                         xiaotai_setting_set_text(slots[i].name, list[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t door_peer_put_first(const char *id)
{
    /* A doorbell's indoor station has one doorbell: choosing another replaces it. */
    const uint8_t kept = door.bell_indoor ? 0U : door.peer_count;
    if (door_peer_slot(id) < 0 && kept >= XIAOTAI_APP_DOOR_PEERS_MAX) return XIAOTAI_ERR_NO_MEM;
    char list[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX] = {{0}};
    (void)snprintf(list[0], sizeof(list[0]), "%s", id);
    size_t count = 1;
    for (uint8_t i = 0; i < kept && count < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) {
        if (strcmp(door.peers[i], list[0])) memcpy(list[count++], door.peers[i], sizeof(list[0]));
    }
    return store(list);
}

/* Behind the ones chosen: a counterpart learnt from a first contact never
 * takes the place of one that was chosen. */
static xiaotai_err_t append(const char *id)
{
    if (door_peer_slot(id) >= 0) return XIAOTAI_OK;
    if (door.peer_count >= XIAOTAI_APP_DOOR_PEERS_MAX) return XIAOTAI_ERR_NO_MEM;
    char list[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX] = {{0}};
    memcpy(list, door.peers, sizeof(list));
    (void)snprintf(list[door.peer_count], sizeof(list[0]), "%s", id);
    return store(list);
}

/* A doorbell nobody set up: the first device that looks at it is its indoor
 * station from then on, so the visitor's key has somebody to ring. */
void door_peer_learn_indoor(const char *id)
{
    xiaotai_contact_t contact;
    if (!door.sender || door.access || door.peer_count || !door_peer_contact(id, &contact)) return;
    if (append(id) == XIAOTAI_OK) xiaotai_eventlog_note("door", "indoor.learnt", 0, 0);
}

/* A door station said (HELLO) that it placed the running call to this device
 * as its indoor station. Not chosen here yet: added while there is room (a
 * doorbell's indoor station has one doorbell), and said on a door page. The
 * call it came in stays what it was. */
bool door_peer_learn_station(const xiaotai_call_status_t *call, uint8_t mode)
{
    xiaotai_contact_t contact;
    const uint8_t room = door.bell_indoor ? 1U : XIAOTAI_APP_DOOR_PEERS_MAX;
    if (door.sender || (mode != XIAOTAI_APP_DOOR_BELL && mode != XIAOTAI_APP_DOOR_ACCESS) || call->outgoing ||
        call->state != XIAOTAI_CALL_ACTIVE || !door_peer_contact(call->peer_id, &contact) ||
        door_peer_slot(call->peer_id) >= 0 || door.peer_count >= room) return false;
    if (append(call->peer_id) != XIAOTAI_OK) return false;
    xiaotai_eventlog_note("door", "station.learnt", call->session, mode);
    char name[XIAOTAI_SETTING_TEXT_MAX];
    door_device_label(call->peer_id, contact.name, name, sizeof(name));
    (void)snprintf(door.learnt, sizeof(door.learnt), "%s 是门口的室外机，已添加", name);
    door_peer_learnt_say();
    return true;
}

/* Only the application in front draws: learnt during another application's
 * call, it is said with the next door page. */
void door_peer_learnt_say(void)
{
    if (!door.learnt[0] || !door.foreground) return;
    xiaotai_view_toast(door.learnt, 0);
    door.learnt[0] = '\0';
}

xiaotai_err_t door_peer_remove(const char *id)
{
    char list[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX] = {{0}};
    size_t count = 0;
    for (uint8_t i = 0; i < door.peer_count; ++i) {
        if (strcmp(door.peers[i], id)) memcpy(list[count++], door.peers[i], sizeof(list[0]));
    }
    return store(list);
}

bool door_peer_contact(const char *id, xiaotai_contact_t *contact)
{
    const int kind = door_contact_kind("device");
    return id && id[0] && kind != XIAOTAI_CONTACT_ANY &&
           xiaotai_contacts_find(id, kind, XIAOTAI_CONTACTS_STALE_OK, contact) == XIAOTAI_OK;
}

bool door_peer_online(const char *id)
{
    xiaotai_contact_t contact;
    return door_peer_contact(id, &contact) && contact.online_known && contact.online;
}

xiaotai_err_t door_dial(const char *id, bool voice, uint32_t *session)
{
    const uint32_t flags = !voice && xiaotai_module_present("video_call") ? XIAOTAI_CALL_VIDEO : 0;
    const xiaotai_err_t err = xiaotai_call_dial(id, flags, session);
    return err == XIAOTAI_ERR_NOT_SUPPORTED && flags ? xiaotai_call_dial(id, 0, session) : err;
}

bool door_refused_video(const xiaotai_call_status_t *ended)
{
    return ended->end == XIAOTAI_CALL_END_PEER_REJECT && ended->error == XIAOTAI_ERR_NOT_SUPPORTED &&
           (ended->flags & XIAOTAI_CALL_VIDEO);
}

/* Contacts without a remark get a readable name, never their raw id (a call
 * from a device without one carries the id as its name). */
void door_device_label(const char *id, const char *name, char *out, size_t size)
{
    const size_t length = strlen(id);
    if (name && name[0] && strcmp(name, id) != 0) (void)snprintf(out, size, "%s", name);
    else (void)snprintf(out, size, "设备 %.4s", length > 4 ? id + length - 4 : id);
}

void door_peer_label(const char *id, char *out, size_t size)
{
    xiaotai_contact_t contact;
    const bool found = door_peer_contact(id, &contact);
    door_device_label(id, found ? contact.name : NULL, out, size);
}
