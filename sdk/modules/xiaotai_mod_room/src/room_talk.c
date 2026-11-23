// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <stdatomic.h>
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

/* A released gate is immediate even when the queue is full. Reconcile that
 * release during an active press; do not poll a room with no press. */
#define TALK_RECONCILE_MS 200U

static xiaotai_mutex_t s_lock;
static uint32_t s_generation, s_binding;
static uint32_t s_local_ticket, s_last_local_ticket;
static uint32_t s_press_serial;
static uint16_t s_tail_ms, s_keepalive_ms;
static int64_t s_deadline;
static bool s_pressed, s_gate_open, s_signal_first;
static _Atomic uint32_t s_presses, s_refused, s_releases;

xiaotai_err_t room_talk_init(void) { return xiaotai_mutex_init(&s_lock); }

void room_talk_deinit(void)
{
    xiaotai_mutex_deinit(&s_lock);
    s_generation = s_binding = 0;
    s_local_ticket = s_last_local_ticket = 0;
    s_press_serial = 0;
    s_deadline = 0;
    s_pressed = s_gate_open = s_signal_first = false;
    atomic_store(&s_presses, 0);
    atomic_store(&s_refused, 0);
    atomic_store(&s_releases, 0);
}

void room_talk_admit(room_t *room)
{
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    s_generation = room->view.generation;
    s_local_ticket = s_last_local_ticket = 0;
    s_binding = room->binding;
    s_tail_ms = room->config.talk_tail_ms;
    s_keepalive_ms = room->config.talk_keepalive_ms;
    s_deadline = 0;
    s_pressed = s_gate_open = false;
    s_signal_first = room->config.talk_signal_before_gate;
    xiaotai_mutex_unlock(&s_lock);
}

void room_talk_retire(uint32_t generation)
{
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (!generation) generation = s_generation;
    s_generation = 0;
    s_local_ticket = s_last_local_ticket = 0;
    s_deadline = 0;
    s_pressed = s_gate_open = false;
    if (generation) (void)xiaotai_media_uplink_gate(generation, false, 0);
    xiaotai_mutex_unlock(&s_lock);
}

static void drive_job(void *unused)
{
    (void)unused;
    room_t *room = room_context();
    if (room) room_drive(room);
}

static bool mic_muted(void)
{
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    return media.mic_muted;
}

static xiaotai_err_t press_locked(uint32_t generation)
{
    if (!generation || generation != s_generation || s_binding != xiaotai_identity_generation() ||
        xiaotai_quiesced() || mic_muted()) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_pressed) {
        /* The owner cannot read the new intent until the lock is released.
         * A full owner queue must not open an orphan gate. */
        xiaotai_err_t err = xiaotai_run(drive_job, NULL);
        if (err == XIAOTAI_OK && !s_signal_first) err = xiaotai_media_uplink_gate(generation, true, 0);
        if (err != XIAOTAI_OK) return err;
        s_gate_open = !s_signal_first;
        ++s_press_serial;
    }
    s_pressed = true;
    s_deadline = s_keepalive_ms ? room_now_ms() + s_keepalive_ms : 0;
    atomic_fetch_add(&s_presses, 1);
    return XIAOTAI_OK;
}

static bool release_locked(uint16_t tail_ms)
{
    if (s_generation) (void)xiaotai_media_uplink_gate(s_generation, false, tail_ms);
    const bool notify = s_pressed;
    s_local_ticket = 0;
    s_pressed = s_gate_open = false;
    s_deadline = 0;
    return notify;
}

xiaotai_err_t xiaotai_room_talk(uint32_t generation, bool pressed)
{
    if (!room_started()) return pressed ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_OK;
    bool notify = false;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (!pressed) {
        /* Release is a stop command, not an admission check: a stale control
         * must not leave the current mic open, even with a saturated queue. */
        notify = release_locked(s_tail_ms);
    } else err = press_locked(generation);
    xiaotai_mutex_unlock(&s_lock);
    if (err != XIAOTAI_OK) atomic_fetch_add(&s_refused, 1);
    if (notify) (void)xiaotai_run(drive_job, NULL);
    return err;
}

xiaotai_err_t xiaotai_room_toggle_talk(uint32_t generation, bool *pressed)
{
    if (!pressed) return XIAOTAI_ERR_INVALID_ARG;
    *pressed = false;
    if (!room_started()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = XIAOTAI_OK;
    bool notify = false;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (!generation || generation != s_generation) err = XIAOTAI_ERR_INVALID_STATE;
    else if (s_pressed) notify = release_locked(s_tail_ms);
    else {
        err = press_locked(generation);
        *pressed = err == XIAOTAI_OK;
    }
    xiaotai_mutex_unlock(&s_lock);
    if (err != XIAOTAI_OK) atomic_fetch_add(&s_refused, 1);
    if (notify) (void)xiaotai_run(drive_job, NULL);
    return err;
}

xiaotai_err_t xiaotai_room_hold_local(uint32_t generation, uint32_t ticket, bool pressed)
{
    if (!generation || !ticket) return XIAOTAI_ERR_INVALID_ARG;
    if (!room_started()) return pressed ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_OK;
    bool notify = false;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (!pressed) {
        if (generation == s_generation && ticket == s_local_ticket) notify = release_locked(s_tail_ms);
    } else if (generation != s_generation || (s_last_local_ticket &&
               (uint32_t)(ticket - s_last_local_ticket) >= UINT32_C(0x80000000)) ||
               ticket == s_last_local_ticket) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else {
        s_last_local_ticket = ticket;
        /* A local release must not claim a gate held by a different control. */
        err = s_pressed ? XIAOTAI_ERR_BUSY : press_locked(generation);
        if (err == XIAOTAI_OK) s_local_ticket = ticket;
    }
    xiaotai_mutex_unlock(&s_lock);
    if (err != XIAOTAI_OK) atomic_fetch_add(&s_refused, 1);
    if (notify) (void)xiaotai_run(drive_job, NULL);
    return err;
}

xiaotai_err_t xiaotai_room_hold_local_refresh(uint32_t generation, uint32_t ticket)
{
    if (!generation || !ticket) return XIAOTAI_ERR_INVALID_ARG;
    if (!room_started()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    bool notify = false;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (s_pressed && generation == s_generation && ticket == s_local_ticket) {
        const int64_t now = room_now_ms();
        const bool expired = s_deadline && now > s_deadline;
        const bool blocked = s_binding != xiaotai_identity_generation() || xiaotai_quiesced() || mic_muted();
        if (expired || blocked) {
            notify = release_locked(blocked ? 0 : s_tail_ms);
            if (expired) atomic_fetch_add(&s_releases, 1);
        } else {
            s_deadline = s_keepalive_ms ? now + s_keepalive_ms : 0;
            atomic_fetch_add(&s_presses, 1);
            err = XIAOTAI_OK;
        }
    }
    xiaotai_mutex_unlock(&s_lock);
    if (notify) (void)xiaotai_run(drive_job, NULL);
    return err;
}

/* Held under s_lock. Both the owner check and the wake time use this boundary. */
static int64_t owner_expiry(const room_t *room)
{
    return s_deadline ? s_deadline + (room->config.talk_keepalive_exclusive ? 1 : 0) : INT64_MAX;
}

static bool open_signalled_gate(room_t *room, uint32_t serial, xiaotai_err_t *err)
{
    const uint32_t generation = room->view.generation;
    const bool submitted = *err == XIAOTAI_OK;
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    const bool pending = s_pressed && generation == s_generation && serial == s_press_serial;
    if (pending) {
        if (submitted) {
            if (s_binding != xiaotai_identity_generation() || xiaotai_quiesced() || mic_muted() ||
                owner_expiry(room) <= room_now_ms()) *err = XIAOTAI_ERR_INVALID_STATE;
            else *err = xiaotai_media_uplink_gate(generation, true, 0);
        }
        if (*err == XIAOTAI_OK) s_gate_open = true;
        else {
            (void)release_locked(0);
            atomic_fetch_add(&s_refused, 1);
        }
    }
    xiaotai_mutex_unlock(&s_lock);
    /* The request may have been released while the SDK was submitting it. */
    if (submitted && (!pending || *err != XIAOTAI_OK))
        (void)room_connection_send(room, "set_mic_state", false);
    return pending && *err == XIAOTAI_OK;
}

void room_talk_drive(room_t *room, int64_t now)
{
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    const bool current = s_generation == room->view.generation;
    if (current && s_pressed) {
        const bool expired = owner_expiry(room) <= now;
        const bool muted = mic_muted();
        if (expired || muted) {
            (void)release_locked(muted ? 0 : s_tail_ms);
            if (expired) atomic_fetch_add(&s_releases, 1);
        }
    }
    bool speaking = current && s_pressed;
    const bool needs_gate = s_signal_first && speaking && !s_gate_open;
    const uint32_t serial = s_press_serial;
    xiaotai_mutex_unlock(&s_lock);
    if (!needs_gate && speaking == room->view.transmitting) return;
    xiaotai_err_t err = room_connection_send(room, "set_mic_state", speaking);
    if (room->config.talk_signal_before_gate) {
        if (needs_gate) speaking = open_signalled_gate(room, serial, &err);
        room->view.error = err;
        room->view.code = 0;
        if (err != XIAOTAI_OK) speaking = false;
    } else if (err != XIAOTAI_OK) { room_connection_fail(room, err, 0, false); return; }
    room->view.transmitting = speaking;
    room->view.phase = speaking ? XIAOTAI_ROOM_SPEAKING : XIAOTAI_ROOM_LISTENING;
    room_members_self_mic(room, speaking);
}

int64_t room_talk_due(const room_t *room, int64_t now)
{
    (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    const int64_t check = now + TALK_RECONCILE_MS;
    const int64_t expiry = owner_expiry(room);
    const int64_t due = s_pressed || room->view.transmitting ? (expiry < check ? expiry : check) : INT64_MAX;
    xiaotai_mutex_unlock(&s_lock);
    return due;
}

void room_talk_stats(xiaotai_room_stats_t *stats)
{
    stats->talk_presses = atomic_load(&s_presses);
    stats->talk_refused = atomic_load(&s_refused);
    stats->talk_keepalive_releases = atomic_load(&s_releases);
}
