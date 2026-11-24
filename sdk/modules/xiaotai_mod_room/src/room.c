// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/link.h"
#include "xiaotai/media.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"

static xiaotai_room_config_t s_config;
static bool s_initialized;
static atomic_bool s_started;
static xiaotai_mutex_t s_snapshot_lock, s_post_lock;
static uint32_t s_requested_attendance; /* low bit is the requested value */
static room_t *s_room;
static xiaotai_room_attempt_fn_t s_attempt_observer;

static void command_sizes(xiaotai_room_config_t *cfg)
{
    const bool external = xiaotai_mem_has_external();
    if (!cfg->command_max_bytes && !cfg->command_pool_bytes) {
        cfg->command_max_bytes = external ? 40960U : 12288U;
        cfg->command_pool_bytes = external ? 81920U : 16384U;
    } else if (!cfg->command_max_bytes) {
        const uint32_t default_max = external ? 40960U : 12288U;
        cfg->command_max_bytes = cfg->command_pool_bytes < default_max ? cfg->command_pool_bytes : default_max;
    } else if (!cfg->command_pool_bytes) {
        const uint32_t limit = XIAOTAI_LINK_COMMAND_LIMIT_BYTES;
        cfg->command_pool_bytes = cfg->command_max_bytes > limit / 2U ? limit : 2U * cfg->command_max_bytes;
    }
}

xiaotai_err_t xiaotai_room_configure(const xiaotai_room_config_t *config)
{
    if (atomic_load(&s_started)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_room_config_t next = config ? *config : (xiaotai_room_config_t){0};
    if (s_initialized) {
        if ((next.command_max_bytes && next.command_max_bytes != s_config.command_max_bytes) ||
            (next.command_pool_bytes && next.command_pool_bytes != s_config.command_pool_bytes)) {
            return XIAOTAI_ERR_INVALID_STATE;
        }
        next.command_max_bytes = s_config.command_max_bytes;
        next.command_pool_bytes = s_config.command_pool_bytes;
    }
    if ((next.command_max_bytes && (next.command_max_bytes < 2048U ||
                                    next.command_max_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES)) ||
        (next.command_pool_bytes && (next.command_pool_bytes < 2048U ||
                                     next.command_pool_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES ||
                                     next.command_pool_bytes < next.command_max_bytes)) ||
        (next.members_max > XIAOTAI_ROOM_MEMBERS_LIMIT && next.members_max != XIAOTAI_ROOM_NO_MEMBER_LIST) ||
        (next.talk_keepalive_ms && next.talk_keepalive_ms < 200U)) return XIAOTAI_ERR_INVALID_ARG;
    s_config = next;
    return XIAOTAI_OK;
}

int64_t room_now_ms(void) { return xiaotai_time_us() / 1000; }
room_t *room_context(void) { return s_room; }
bool room_started(void) { return atomic_load(&s_started); }

xiaotai_err_t xiaotai_room_observe_attempts(xiaotai_room_attempt_fn_t observer)
{
    if (!observer) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !s_initialized || room_started() || s_attempt_observer)
        return XIAOTAI_ERR_INVALID_STATE;
    s_attempt_observer = observer;
    return XIAOTAI_OK;
}

void room_attempt_changed(uint32_t activity, xiaotai_room_attempt_t phase)
{
    if (!activity || !s_attempt_observer) return;
    /* An active Room can occupy the last slot while transport stays online. */
    if (phase != XIAOTAI_ROOM_ATTEMPT_STARTED && xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE) return;
    s_attempt_observer(activity, phase);
}

bool room_digits(const char *text, size_t digits, bool optional)
{
    if (optional && (!text || !text[0])) return true;
    if (!text) return false;
    for (size_t i = 0; i < digits; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return text[digits] == '\0';
}

void xiaotai_room_status(xiaotai_room_status_t *status)
{
    if (!status) return;
    if (!atomic_load(&s_started)) { memset(status, 0, sizeof(*status)); return; }
    (void)xiaotai_mutex_lock(&s_snapshot_lock, XIAOTAI_WAIT_FOREVER);
    *status = s_room->published;
    xiaotai_mutex_unlock(&s_snapshot_lock);
}

void xiaotai_room_stats(xiaotai_room_stats_t *stats)
{
    if (!stats) return;
    if (!atomic_load(&s_started)) { memset(stats, 0, sizeof(*stats)); return; }
    (void)xiaotai_mutex_lock(&s_snapshot_lock, XIAOTAI_WAIT_FOREVER);
    *stats = s_room->published_stats;
    xiaotai_mutex_unlock(&s_snapshot_lock);
    room_talk_stats(stats);
}

static void publish(room_t *room)
{
    room->publish_retry = !xiaotai_mutex_lock(&s_snapshot_lock, 0);
    if (room->publish_retry) return;
    const bool phase_changed = room->view.phase != room->published.phase;
    if (memcmp(&room->view, &room->published, sizeof(room->view))) {
        ++room->view.revision;
        memcpy(&room->published, &room->view, sizeof(room->view));
        room->publish_event = true;
    }
    room->published_stats = room->stats;
    xiaotai_mutex_unlock(&s_snapshot_lock);
    if (phase_changed) {
        static const char *const names[] = {"offline", "syncing", "member of no room", "waiting", "connecting",
                                            "listening", "speaking", "leaving"};
        const unsigned now = (unsigned)room->view.phase;
        xiaotai_eventlog_record(ROOM_LOG_SOURCE, "state", room->view.generation, room->view.phase);
        const char *name = now < sizeof(names) / sizeof(names[0]) ? names[now] : "?";
        /* Before a room is joined the phases are bookkeeping, not the story. */
        if (now >= (unsigned)XIAOTAI_ROOM_WAITING) XIAOTAI_LOGI(ROOM_LOG_SOURCE, "%s gen=%" PRIu32, name, room->view.generation);
        else XIAOTAI_LOGD(ROOM_LOG_SOURCE, "%s", name);
    }
    if (room->publish_event) {
        xiaotai_args_t args = {.value = room->view.phase};
        memcpy(args.text, room->view.room_code, sizeof(room->view.room_code));
        room->publish_event = xiaotai_event_emit(XIAOTAI_EVENT_ROOM_STATE, &args, NULL, 0) != XIAOTAI_OK;
    }
}

void room_result(xiaotai_room_action_t action, xiaotai_err_t error, int32_t code)
{
    static const char *const names[] = {"create", "join", "leave", "refresh", "connect", "disconnect"};
    const xiaotai_room_result_t result = {.action = action, .error = error, .code = code};
    xiaotai_args_t args = {.value = error};
    strcpy(args.text, names[action]);
    const xiaotai_err_t emitted = xiaotai_event_emit(XIAOTAI_EVENT_ROOM_RESULT, &args, &result, sizeof(result));
    if (emitted != XIAOTAI_OK) XIAOTAI_LOGW("room", "result event %s: %d", names[action], emitted);
}

/* A close/open moves the epoch even when both land before the owner wakes. */
static uint32_t requested_attendance(void)
{
    (void)xiaotai_mutex_lock(&s_post_lock, XIAOTAI_WAIT_FOREVER);
    const uint32_t attendance = s_requested_attendance;
    xiaotai_mutex_unlock(&s_post_lock);
    return attendance;
}

void room_admit_talk(room_t *room)
{
    (void)xiaotai_mutex_lock(&s_post_lock, XIAOTAI_WAIT_FOREVER);
    if ((s_requested_attendance & 1U) && s_requested_attendance == room->attendance) room_talk_admit(room);
    xiaotai_mutex_unlock(&s_post_lock);
}

void room_end_connection(room_t *room, room_end_t reason)
{
    room->link.no_mem_reported = false;
    if (room->end_connection && ((reason == ROOM_END_BINDING && room->end_connection == room_connection_end) ||
        room->link.activity || room->link.stage || room->view.generation ||
        room->view.phase == XIAOTAI_ROOM_CONNECTING ||
        room->view.phase == XIAOTAI_ROOM_LISTENING || room->view.phase == XIAOTAI_ROOM_SPEAKING)) {
        room->end_connection(room, reason);
    }
    room->view.generation = 0;
    room->view.transmitting = false;
    room_members_clear(room);
    room->view.phase = XIAOTAI_ROOM_OFFLINE;
}

void room_update_context(room_t *room)
{
    const uint32_t binding = xiaotai_identity_generation();
    if (room->binding != binding) {
        room_end_connection(room, ROOM_END_BINDING);
        room_assignment_reset(room);
        const uint32_t revision = room->view.revision;
        const uint32_t members_revision = room->view.members_revision;
        memset(&room->view, 0, sizeof(room->view));
        room->view.revision = revision;
        room->view.members_revision = members_revision;
        memset(&room->token_shape, 0, sizeof(room->token_shape));
        room->view.attending = (room->attendance & 1U) != 0;
        room->binding = binding;
        room->online = false;
        room->release_pending = false;
    }
    const uint32_t attendance = requested_attendance();
    if (attendance != room->attendance) {
        room->attendance = attendance;
        room->view.attending = (attendance & 1U) != 0;
        room_retire_read(room);
        room_end_connection(room, ROOM_END_ATTENDANCE);
        room->manual_requested = room->manual_disconnected = false;
        room->view.attempts = 0;
        room->link.retry_at = 0;
        room->sync_at = INT64_MAX;
        if (room->view.attending) {
            room->view.assignment_known = false;
            room_read_again(room);
        }
    }
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    const bool online = platform.phase == XIAOTAI_PLATFORM_ONLINE && platform.generation == binding;
    if (online && (!room->online || platform.notify_revision != room->notify_revision)) {
        room->notify_revision = platform.notify_revision;
        if (room->view.attending) room_read_again(room);
        if (room->mutation_queued || room->confirming) room->mutation_at = room_now_ms();
    }
    if (!online && room->online) room_end_connection(room, ROOM_END_OFFLINE);
    room->online = online;
}

static void timer(void *ctx)
{
    room_t *room = ctx;
    room->timer = 0;
    room_drive(room);
}

void room_drive(room_t *room)
{
    room_update_context(room);
    const int64_t now = room_now_ms();
    room_release_drive(room, now);
    room_assignment_drive(room, now);
    room_connection_drive(room, now);
    publish(room);
    if (!room->publish_retry && !room->publish_event) room_members_publish(room);
    xiaotai_timer_cancel(room->timer);
    room->timer = 0;
    int64_t due = INT64_MAX;
    if (room->online && !xiaotai_quiesced() && room->pending == ROOM_REQUEST_NONE && room->flights < 2U) {
        if (room->mutation_queued || room->confirming) due = room->mutation_at;
        else if (room->view.attending) due = room->sync_at;
    }
    if ((room->publish_retry || room->publish_event || room->members.event || room->members.voice_event) &&
        due > now + 1) due = now + 1;
    const int64_t link_due = room_connection_due(room);
    if (link_due < due) due = link_due;
    if (due != INT64_MAX) {
        const uint32_t delay = due > now ? (uint32_t)(due - now) : 1U;
        const xiaotai_err_t err = xiaotai_timer_start(delay, timer, room, &room->timer);
        if (err != XIAOTAI_OK) XIAOTAI_LOGE("room", "assignment timer: %d", err);
    }
}

static void on_platform(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    room_t *room = s_room;
    if (!room) return;
    if (!strcmp(event->name, XIAOTAI_EVENT_PLATFORM_ONLINE) ||
        (!strcmp(event->name, XIAOTAI_EVENT_QUIESCE) && !xiaotai_quiesced())) {
        if (room->view.attending) room_read_again(room);
        if (room->mutation_queued || room->confirming) room->mutation_at = room_now_ms();
    } else if (!strcmp(event->name, XIAOTAI_EVENT_PLATFORM_IDLE) && room->mutation_queued) {
        room->mutation_at = room_now_ms();
    }
    room_drive(room);
}

static void on_signal(const xiaotai_platform_signal_t *signal, void *unused)
{
    (void)unused;
    room_t *room = s_room;
    if (!room) return;
    room_update_context(room);
    if (signal->generation != room->binding) ++room->stats.stale;
    else if (room->view.attending) room_read_again(room);
    room_drive(room);
}

static void attend_job(void *unused)
{
    (void)unused;
    if (s_room) room_drive(s_room);
}

xiaotai_err_t xiaotai_room_attend(bool attending)
{
    if (!atomic_load(&s_started)) return XIAOTAI_ERR_INVALID_STATE;
    (void)xiaotai_mutex_lock(&s_post_lock, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = XIAOTAI_OK;
    if ((s_requested_attendance & 1U) != (uint32_t)attending) {
        err = xiaotai_run(attend_job, NULL);
        if (err == XIAOTAI_OK) {
            s_requested_attendance = ((s_requested_attendance + 2U) & ~1U) | attending;
            if (!attending) room_talk_retire(0);
        }
    }
    xiaotai_mutex_unlock(&s_post_lock);
    return err;
}

static void action_job(void *ctx)
{
    room_action_t *action = ctx;
    room_t *room = s_room;
    if (room) {
        room_update_context(room);
        if (action->binding != room->binding ||
            (action->action != XIAOTAI_ROOM_ACTION_LEAVE && action->attendance != room->attendance)) {
            ++room->stats.stale;
        } else {
            room_assignment_action(room, action);
        }
        room_drive(room);
    }
    memset(action, 0, sizeof(*action));
    xiaotai_mem_free(action);
}

static xiaotai_err_t post_action(xiaotai_room_action_t kind, const char *code, const char *password)
{
    if (!atomic_load(&s_started)) return XIAOTAI_ERR_INVALID_STATE;
    if (xiaotai_quiesced()) {
        room_result(kind, XIAOTAI_ERR_INVALID_STATE, 0);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    room_action_t *action = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*action));
    if (!action) return XIAOTAI_ERR_NO_MEM;
    action->action = kind;
    if (code) memcpy(action->code, code, sizeof(action->code));
    if (password && password[0]) memcpy(action->password, password, sizeof(action->password));
    (void)xiaotai_mutex_lock(&s_post_lock, XIAOTAI_WAIT_FOREVER);
    action->attendance = s_requested_attendance;
    action->binding = xiaotai_identity_generation();
    const bool needs_attendance = kind == XIAOTAI_ROOM_ACTION_CREATE || kind == XIAOTAI_ROOM_ACTION_JOIN;
    const xiaotai_err_t err = needs_attendance && !(action->attendance & 1U) && !xiaotai_quiesced()
                                  ? XIAOTAI_ERR_INVALID_STATE : xiaotai_run(action_job, action);
    xiaotai_mutex_unlock(&s_post_lock);
    if (err != XIAOTAI_OK) {
        memset(action, 0, sizeof(*action));
        xiaotai_mem_free(action);
    }
    return err;
}

xiaotai_err_t xiaotai_room_create(const char *password)
{
    if (!room_digits(password, XIAOTAI_ROOM_PASSWORD_DIGITS, true)) return XIAOTAI_ERR_INVALID_ARG;
    return post_action(XIAOTAI_ROOM_ACTION_CREATE, NULL, password);
}

xiaotai_err_t xiaotai_room_join(const char *code, const char *password)
{
    if (!room_digits(code, XIAOTAI_ROOM_CODE_DIGITS, false) ||
        !room_digits(password, XIAOTAI_ROOM_PASSWORD_DIGITS, true)) return XIAOTAI_ERR_INVALID_ARG;
    return post_action(XIAOTAI_ROOM_ACTION_JOIN, code, password);
}

xiaotai_err_t xiaotai_room_leave(void) { return post_action(XIAOTAI_ROOM_ACTION_LEAVE, NULL, NULL); }
xiaotai_err_t xiaotai_room_refresh(void) { return post_action(XIAOTAI_ROOM_ACTION_REFRESH, NULL, NULL); }
xiaotai_err_t xiaotai_room_connect(void) { return post_action(XIAOTAI_ROOM_ACTION_CONNECT, NULL, NULL); }
xiaotai_err_t xiaotai_room_disconnect(void) { return post_action(XIAOTAI_ROOM_ACTION_DISCONNECT, NULL, NULL); }

bool xiaotai_room_settled(void)
{
    return !s_room || !(s_room->flights || s_room->release_pending ||
                       s_room->mutation_queued || s_room->confirming);
}

xiaotai_err_t xiaotai_module_room_init(void)
{
    if (s_initialized) return XIAOTAI_ERR_INVALID_STATE;
    command_sizes(&s_config);
    const xiaotai_transport_requirement_t transport = {.links = 1, .command_pool_bytes = s_config.command_pool_bytes};
    static const xiaotai_media_requirement_t media = {
        .packet_ms = 20, .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .downlink_burst_ms = 200, .latency = XIAOTAI_MEDIA_LATENCY_RELAYED,
    };
    static const xiaotai_platform_requirement_t platform = {
        .request_body_bytes = 256, .response_bytes = 4096, .signal_bytes = 512, .concurrent_requests = 2,
    };
    xiaotai_err_t err = xiaotai_transport_require(&transport);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&media);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require(&platform);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require_service(XIAOTAI_PLATFORM_SERVICE_CALL);
    if (err == XIAOTAI_OK) err = xiaotai_platform_signal_subscribe("room_assignment_changed", NULL, on_signal, NULL);
    if (err == XIAOTAI_OK) err = room_connection_init();
    if (err == XIAOTAI_OK) err = room_controls_init();
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_PLATFORM_ONLINE, on_platform, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, on_platform, NULL},
        {XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, on_platform, NULL},
        {XIAOTAI_EVENT_PLATFORM_IDLE, on_platform, NULL},
        {XIAOTAI_EVENT_QUIESCE, on_platform, NULL},
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]) && err == XIAOTAI_OK; ++i) {
        err = xiaotai_event_subscribe_static(&events[i]);
    }
    if (err == XIAOTAI_OK) s_initialized = true;
    return err;
}

void xiaotai_module_room_start(void)
{
    if (!s_initialized || s_room) return;
    s_room = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s_room));
    if (!s_room) { XIAOTAI_LOGE("room", "room state allocation failed"); return; }
    xiaotai_err_t err = xiaotai_mutex_init(&s_snapshot_lock);
    if (err == XIAOTAI_OK) {
        err = xiaotai_mutex_init(&s_post_lock);
        if (err != XIAOTAI_OK) xiaotai_mutex_deinit(&s_snapshot_lock);
    }
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE("room", "room lock init failed: %d", err);
        xiaotai_mem_free(s_room);
        s_room = NULL;
        return;
    }
    s_room->config = s_config;
    if (!s_room->config.members_max) {
        s_room->config.members_max = xiaotai_mem_has_external() ? XIAOTAI_ROOM_MEMBERS_LIMIT : XIAOTAI_ROOM_NO_MEMBER_LIST;
    }
    err = room_members_init(s_room);
    if (err == XIAOTAI_OK) {
        err = room_talk_init();
        if (err != XIAOTAI_OK) room_members_deinit(s_room);
    }
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE("room", "member/talk initialization failed: %d", err);
        xiaotai_mutex_deinit(&s_post_lock);
        xiaotai_mutex_deinit(&s_snapshot_lock);
        xiaotai_mem_free(s_room);
        s_room = NULL;
        return;
    }
    if (!s_room->config.talk_tail_ms) s_room->config.talk_tail_ms = 600U;
    else if (s_room->config.talk_tail_ms == XIAOTAI_ROOM_NO_TAIL) s_room->config.talk_tail_ms = 0U;
    if (!s_room->config.retry_max) s_room->config.retry_max = 5U;
    s_room->binding = xiaotai_identity_generation();
    s_room->end_connection = room_connection_end;
    s_room->sync_at = s_room->mutation_at = INT64_MAX;
    atomic_store(&s_started, true);
}

void xiaotai_room_reset(void)
{
    atomic_store(&s_started, false);
    s_attempt_observer = NULL;
    if (s_room) {
        if (xiaotai_on_owner() && s_room->link.stage) room_end_connection(s_room, ROOM_END_BINDING);
        xiaotai_timer_cancel(s_room->timer);
        room_talk_deinit();
        room_members_deinit(s_room);
        xiaotai_mutex_deinit(&s_snapshot_lock);
        xiaotai_mutex_deinit(&s_post_lock);
        xiaotai_mem_free(s_room);
        s_room = NULL;
    }
    s_requested_attendance = 0;
    s_initialized = false;
    memset(&s_config, 0, sizeof(s_config));
}
