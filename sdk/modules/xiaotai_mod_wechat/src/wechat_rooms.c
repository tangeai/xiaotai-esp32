// SPDX-License-Identifier: MIT
#include "wechat_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#include <string.h>

#define EARLY_SLOTS 2U
#define EARLY_FIXED_BYTES 4097U
#define EARLY_TOTAL_BYTES 8192U
#define RETIRED_MAX 8U
#define RETIRED_US INT64_C(60000000)
#define NONE XIAOTAI_WECHAT_REASON_NONE

typedef enum {EARLY_PENDING, EARLY_CANCELLED, EARLY_REJECTED} early_state_t;

typedef struct {
    char call[WX_ID_BYTES], room[WX_ID_BYTES];
    int64_t since;
    early_state_t ended;
} retired_t;

typedef struct {
    char call[WX_ID_BYTES], room[WX_ID_BYTES];
    char *json;
    size_t bytes;
    early_state_t state;
} early_t;

static struct {
    uint32_t generation, session;
    char target[XIAOTAI_CONTACT_ID_MAX + 1U], app[XIAOTAI_CONTACT_EXTRA_MAX + 1U], call[WX_ID_BYTES];
    early_t early[EARLY_SLOTS];
    char *early_storage;
    retired_t *retired;
    unsigned retired_count, next_late;
    uint32_t late[RETIRED_MAX]; /* HTTP replies owed to ended owners; no credentials. */
    size_t early_bytes;
} route;

static void refuse(const wx_room_t *room, uint8_t reason)
{
    if (reason != NONE) wx_refuse(room, reason, false);
}

static void clear_early(early_t *early)
{
    if (early->json) {
        xiaotai_identity_wipe(early->json, early->bytes + 1U);
        if (!route.early_storage) xiaotai_mem_free(early->json);
        route.early_bytes -= early->bytes + 1U;
        early->bytes = 0;
        early->json = NULL;
    }
}

static void clear_wait(void)
{
    for (size_t i = 0; i < EARLY_SLOTS; ++i) clear_early(&route.early[i]);
    memset(route.early, 0, sizeof(route.early));
    xiaotai_identity_wipe(route.target, sizeof(route.target));
    xiaotai_identity_wipe(route.app, sizeof(route.app));
    route.call[0] = '\0';
    route.session = 0;
}

void wx_route_clear(void)
{
    clear_wait();
    if (route.retired) memset(route.retired, 0, route.retired_count * sizeof(*route.retired));
    memset(route.late, 0, sizeof(route.late));
    route.next_late = 0;
    route.generation = xiaotai_identity_generation();
}

void wx_route_reset(void)
{
    wx_route_clear();
    xiaotai_mem_free(route.early_storage);
    xiaotai_mem_free(route.retired);
    memset(&route, 0, sizeof(route));
}

static void check_generation(void)
{
    if (route.generation != xiaotai_identity_generation()) wx_route_clear();
}

xiaotai_err_t wx_route_init(void)
{
    const bool external = xiaotai_mem_has_external();
    route.retired_count = external ? RETIRED_MAX : RETIRED_MAX / 2U;
    route.retired = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, route.retired_count, sizeof(*route.retired));
    if (!route.retired) return XIAOTAI_ERR_NO_MEM;
    if (external) {
        route.early_storage = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, EARLY_SLOTS * EARLY_FIXED_BYTES);
        if (!route.early_storage) {
            wx_route_reset();
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    const xiaotai_err_t error = wx_signal_init();
    if (error != XIAOTAI_OK) wx_route_reset();
    return error;
}

static bool live(const retired_t *entry, int64_t now)
{
    return (entry->call[0] || entry->room[0]) && now >= entry->since && now - entry->since < RETIRED_US;
}

static const retired_t *retired(const char *id, bool room)
{
    if (!id[0]) return NULL;
    const int64_t now = xiaotai_time_us();
    for (unsigned i = 0; i < route.retired_count; ++i) {
        const retired_t *entry = &route.retired[i];
        if (live(entry, now) && strcmp(id, room ? entry->room : entry->call) == 0) return entry;
    }
    return NULL;
}

static retired_t *retire(const char *call, const char *room)
{
    const int64_t now = xiaotai_time_us();
    retired_t *entry = NULL, *oldest = NULL;
    if ((!call[0] && !room[0]) || !route.retired) return NULL;
    for (unsigned i = 0; i < route.retired_count; ++i) {
        retired_t *candidate = &route.retired[i];
        if (live(candidate, now)) {
            const bool same_call = call[0] && strcmp(candidate->call, call) == 0;
            const bool same_room = room[0] && strcmp(candidate->room, room) == 0;
            if ((same_call && (!room[0] || !candidate->room[0] || same_room)) ||
                (same_room && (!call[0] || !candidate->call[0] || same_call))) {
                if (!candidate->call[0]) strcpy(candidate->call, call);
                if (!candidate->room[0]) strcpy(candidate->room, room);
                return candidate;
            }
            if (!oldest || candidate->since < oldest->since) oldest = candidate;
        } else if (!entry) {
            entry = candidate;
        }
    }
    if (!entry) entry = oldest;
    entry->since = now;
    entry->ended = EARLY_PENDING;
    strcpy(entry->call, call);
    strcpy(entry->room, room);
    return entry;
}

static bool hold(const wx_room_t *room, const char *json, size_t size, early_state_t state)
{
    size_t slot = EARLY_SLOTS;
    for (size_t i = 0; i < EARLY_SLOTS; ++i) {
        early_t *early = &route.early[i];
        if (!early->call[0]) {
            if (slot == EARLY_SLOTS) slot = i;
        } else if (strcmp(early->call, room->call_id) == 0) {
            return strcmp(early->room, room->room_id) == 0; /* Keep the first room's credential. */
        }
    }
    if (slot == EARLY_SLOTS) return false;
    const size_t bytes = size + 1U;
    char *copy = NULL;
    if (state == EARLY_PENDING) {
        if (route.early_storage) {
            if (bytes > EARLY_FIXED_BYTES) return false;
            copy = route.early_storage + slot * EARLY_FIXED_BYTES;
        } else {
            if (bytes > EARLY_TOTAL_BYTES - route.early_bytes) return false;
            copy = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
            if (!copy) return false;
        }
        memcpy(copy, json, size);
        copy[size] = '\0';
        route.early_bytes += bytes;
    }
    early_t *early = &route.early[slot];
    strcpy(early->call, room->call_id);
    strcpy(early->room, room->room_id);
    early->json = copy;
    early->bytes = size;
    early->state = state;
    return true;
}

xiaotai_err_t wx_route_begin(uint32_t session, const char *openid, const char *app_id)
{
    if (!route.retired) return XIAOTAI_ERR_INVALID_STATE;
    if (!session || !openid || !openid[0] || !app_id ||
        strlen(openid) >= sizeof(route.target) || strlen(app_id) >= sizeof(route.app))
        return XIAOTAI_ERR_INVALID_ARG;
    check_generation();
    clear_wait();
    route.session = session;
    strcpy(route.target, openid);
    strcpy(route.app, app_id);
    return XIAOTAI_OK;
}

void wx_route_end(uint32_t session, const wx_room_t *room)
{
    if (route.generation != xiaotai_identity_generation()) {
        wx_route_clear();
        return;
    }
    if (session == route.session) {
        retire(route.call, room ? room->room_id : "");
        if (!route.call[0]) {
            route.late[route.next_late++ % route.retired_count] = session;
            /* Redelivery can omit wx_from. Fence only rooms actually seen
             * during the old dial, never retire their unconfirmed call IDs. */
            for (size_t i = 0; i < EARLY_SLOTS; ++i) retire("", route.early[i].room);
        }
        clear_wait();
    } else if (room && !wx_config()->allow_reopen_incoming_room) {
        retire(room->call_id, room->room_id);
    }
}

static bool reply_id(const xiaotai_platform_reply_t *reply, char *id, size_t capacity)
{
    if (reply->result != XIAOTAI_OK || reply->http_status != 200 || reply->code != 0 ||
        !reply->body || !reply->size || reply->size > 1024) return false;
    xiaotai_json_view_t doc, data, field;
    if (wx_parse(reply->body, reply->size, &doc) != XIAOTAI_OK) return false;
    int64_t code;
    return xiaotai_json_view_get(&doc, "code", &field) &&
           xiaotai_json_view_int(&field, &code) == XIAOTAI_OK && code == 0 &&
           xiaotai_json_view_get(&doc, "data", &data) && wx_signal_text(&data, "call_id", id, capacity, true);
}

void wx_route_reply(uint32_t session, const xiaotai_platform_reply_t *reply)
{
    check_generation();
    if (!session || !reply || reply->generation != route.generation) return;
    char id[WX_ID_BYTES];
    if (session != route.session || !wx_waiting_room()) {
        for (unsigned i = 0; i < route.retired_count; ++i) {
            if (route.late[i] != session) continue;
            route.late[i] = 0;
            if (reply_id(reply, id, sizeof(id))) retire(id, "");
            break;
        }
        return;
    }
    /* The first valid reply already selected this call's room route. */
    if (route.call[0]) return;
    if (!reply_id(reply, id, sizeof(id))) {
        wx_bad_dial_reply(session);
        return;
    }
    strcpy(route.call, id);
    for (size_t i = 0; i < EARLY_SLOTS; ++i) {
        early_t *early = &route.early[i];
        if (strcmp(early->call, id) != 0) continue;
        const retired_t *ended = retired(early->room, true);
        if (early->state != EARLY_PENDING || ended) {
            wx_early_cancel(early->state == EARLY_REJECTED || (ended && ended->ended == EARLY_REJECTED));
        } else {
            wx_room_t *room = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*room));
            if (room && wx_signal_room(early->json, early->bytes, room)) {
                /* Reclaim the early bytes before the state machine takes its room. */
                clear_early(early);
                wx_room_handoff(&room, false);
            } else {
                wx_bad_dial_reply(session);
            }
            if (room) xiaotai_identity_wipe(room, sizeof(*room));
            xiaotai_mem_free(room);
        }
        break;
    }
    for (size_t i = 0; i < EARLY_SLOTS; ++i) clear_early(&route.early[i]);
    memset(route.early, 0, sizeof(route.early));
}

void wx_route_cancel(const char *room, bool rejected)
{
    check_generation();
    const bool waiting = route.session && wx_waiting_room();
    if (wx_same_room(room)) {
        wx_cancel(room, rejected);
    } else if (waiting) {
        const early_state_t state = rejected ? EARLY_REJECTED : EARLY_CANCELLED;
        retired_t *ended = retire("", room);
        if (ended && ended->ended == EARLY_PENDING) ended->ended = state;
        for (size_t i = 0; i < EARLY_SLOTS; ++i) {
            early_t *early = &route.early[i];
            if (strcmp(early->room, room) == 0 && early->state == EARLY_PENDING) {
                clear_early(early);
                early->state = state;
            }
        }
    }
}

void wx_route_room(const xiaotai_json_view_t *payload, const char *json, size_t size)
{
    check_generation();
    char room_id[WX_ID_BYTES];
    if (!wx_signal_text(payload, "wx_room_id", room_id, sizeof(room_id), true) ||
        wx_same_room(room_id)) return;
    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U], from[sizeof(device)];
    char openid[XIAOTAI_CONTACT_ID_MAX + 1U], app[WX_ID_BYTES];
    if (xiaotai_identity_device_id(device, sizeof(device)) != XIAOTAI_OK ||
        !wx_signal_text(payload, "wx_from", from, sizeof(from), false) ||
        !wx_signal_text(payload, "wx_user_openid", openid, sizeof(openid), false) ||
        !wx_signal_text(payload, "wx_app_id", app, sizeof(app), false)) return;
    wx_room_t *room = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*room));
    if (!room) {
        xiaotai_eventlog_warn("wechat", "room.memory", route.session, XIAOTAI_ERR_NO_MEM);
        return;
    }
    if (!wx_decode_room(payload, room)) goto done;

    const xiaotai_wechat_config_t *cfg = wx_config();
    const xiaotai_wechat_reasons_t *reason = &cfg->reasons;
    const bool own = from[0] && strcmp(from, device) == 0;
    const bool waiting = route.session && wx_waiting_room();
    const bool candidate = waiting && (!from[0] || own) && room->call_id[0] &&
                           (!openid[0] || strcmp(openid, route.target) == 0) &&
                           (!app[0] || strcmp(app, route.app) == 0);
    const bool match = candidate && route.call[0] && strcmp(room->call_id, route.call) == 0;
    const retired_t *ended = retired(room->room_id, true);
    if (retired(room->call_id, false)) {
        refuse(room, reason->retired_call);
        wx_route_stale();
    } else if (ended) {
        if (match) wx_early_cancel(ended->ended == EARLY_REJECTED);
        else if (candidate && !route.call[0]) {
            const early_state_t state = ended->ended == EARLY_REJECTED ? EARLY_REJECTED : EARLY_CANCELLED;
            if (!hold(room, NULL, 0, state)) refuse(room, reason->overflow);
        } else {
            refuse(room, reason->retired_room);
            wx_route_stale();
        }
    } else if (candidate && !route.call[0]) {
        if (!hold(room, json, size, EARLY_PENDING)) refuse(room, reason->overflow);
    } else if (match) {
        wx_room_handoff(&room, false);
    } else if (waiting && !from[0] && !room->call_id[0] && cfg->anonymous_waiting_is_busy) {
        refuse(room, reason->busy_dialing);
    } else if (own || (waiting && !from[0])) {
        refuse(room, reason->stale);
        wx_route_stale();
    } else {
        wx_room_handoff(&room, true);
    }
done:
    if (room) xiaotai_identity_wipe(room, sizeof(*room));
    xiaotai_mem_free(room);
}
