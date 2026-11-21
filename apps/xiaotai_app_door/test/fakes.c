// SPDX-License-Identifier: MIT
#include "fakes.h"

#include "xiaotai/apps/door.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/feedback.h"
#include "xiaotai/framework.h"
#include "xiaotai/ui.h"
#ifdef XIAOTAI_DOOR_WITH_LOCK
#include "xiaotai/modules/lock.h"
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

xiaotai_net_status_t fake_net;
xiaotai_platform_status_t fake_platform;
xiaotai_call_status_t fake_call;
xiaotai_video_call_status_t fake_video_call;
xiaotai_stream_video_status_t fake_stream_video;
xiaotai_binding_status_t fake_binding;
xiaotai_provisioning_status_t fake_provisioning;
xiaotai_ui_info_t fake_ui = {.awake = true};
xiaotai_contacts_status_t fake_list;
xiaotai_contact_t fake_contacts[3];
unsigned fake_refreshes;
char fake_saved_peer[65];
unsigned fake_ring_in, fake_ring_out, fake_activity;
unsigned fake_calls, fake_mics, fake_mic_saves, fake_volume_saves, fake_stream_stops, fake_lock_calls;
unsigned fake_stream_stop_attempts;
uint32_t fake_control_session, fake_stream_generation, fake_stop_generation;
xiaotai_err_t fake_control_error, fake_preview_error, fake_lock_error, fake_mic_error;
xiaotai_err_t fake_stream_stop_error;
bool fake_requested_mute, fake_saved_mute, fake_previewing;
bool fake_saved_sender, fake_save_pending, fake_feedback_pending;
unsigned fake_role_saves, fake_open_announcements;
xiaotai_err_t fake_announce_error;
#ifdef XIAOTAI_DOOR_WITH_WECHAT
xiaotai_wechat_status_t fake_wechat;
unsigned fake_wechat_calls;
#endif
uint8_t fake_volume = 70, fake_preview, fake_lock_index;
uint8_t fake_volume_step = 10;
size_t fake_lock_count;
const char *fake_verb;
static _Atomic unsigned time_ms;

int64_t __wrap_xiaotai_time_us(void) { return 1000000LL + 1000LL * atomic_load(&time_ms); }
void fake_advance_ms(unsigned ms) { atomic_fetch_add(&time_ms, ms); }

void xiaotai_net_status(xiaotai_net_status_t *status) { *status = fake_net; }
void xiaotai_platform_status(xiaotai_platform_status_t *status) { *status = fake_platform; }
void xiaotai_call_status(xiaotai_call_status_t *status) { *status = fake_call; }
void xiaotai_video_call_status(xiaotai_video_call_status_t *status) { *status = fake_video_call; }
void xiaotai_stream_status(xiaotai_stream_status_t *status)
{
    *status = (xiaotai_stream_status_t){.generation = fake_stream_generation,
        .phase = fake_stream_generation ? XIAOTAI_STREAM_ACTIVE : XIAOTAI_STREAM_LISTENING};
}
void xiaotai_stream_video_status(xiaotai_stream_video_status_t *status) { *status = fake_stream_video; }
void xiaotai_binding_status(xiaotai_binding_status_t *status) { *status = fake_binding; }
void xiaotai_provisioning_status(xiaotai_provisioning_status_t *status) { *status = fake_provisioning; }
bool xiaotai_provisioning_active(void) { return fake_provisioning.phase != XIAOTAI_PROVISIONING_NONE; }
void xiaotai_contacts_status(xiaotai_contacts_status_t *status) { *status = fake_list; }
void xiaotai_settings_status(xiaotai_settings_status_t *status)
{
    *status = (xiaotai_settings_status_t){.volume = fake_volume, .previewing = fake_previewing,
        .preview_volume = fake_preview, .mic_muted = fake_saved_mute, .save_pending = fake_save_pending};
}

void xiaotai_ui_activity(void) { ++fake_activity; }
xiaotai_err_t xiaotai_ui_inspect(uint8_t display, xiaotai_ui_info_t *status)
{
    if (display) return XIAOTAI_ERR_NOT_FOUND;
    *status = fake_ui;
    return XIAOTAI_OK;
}

bool xiaotai_feedback_sound_pending(void) { return fake_feedback_pending; }
xiaotai_err_t xiaotai_binding_announce(void)
{
    assert(fake_binding.phase == XIAOTAI_BINDING_WAITING && fake_binding.verification[0]);
    ++fake_open_announcements;
    return fake_announce_error;
}

#ifdef XIAOTAI_DOOR_WITH_WECHAT
void xiaotai_wechat_status(xiaotai_wechat_status_t *status) { *status = fake_wechat; }
#endif

static xiaotai_err_t action(const char *verb, uint32_t session)
{
    assert(session != 0U && session == fake_call.session);
    fake_verb = verb;
    fake_control_session = session;
    ++fake_calls;
    return fake_control_error;
}

xiaotai_err_t xiaotai_call_accept(uint32_t session) { return action("accept", session); }
xiaotai_err_t xiaotai_call_reject(uint32_t session) { return action("reject", session); }
xiaotai_err_t xiaotai_call_hangup(uint32_t session) { return action("hangup", session); }

xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table)
{
    assert(table->state_count == 2 && !table->effect_count);
    assert(!strcmp(table->states[0].name, "door.incoming") && !strcmp(table->states[0].sound.name, "ring.incoming"));
    assert(!strcmp(table->states[1].name, "door.outgoing") && !strcmp(table->states[1].sound.name, "ring.outgoing"));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_feedback_set(const char *state, bool active)
{
    if (!strcmp(state, "door.incoming")) fake_ring_in = active;
    else if (!strcmp(state, "door.outgoing")) fake_ring_out = active;
    else assert(false);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_settings_volume_preview(uint8_t percent)
{
    if (fake_preview_error != XIAOTAI_OK) return fake_preview_error;
    fake_preview = percent;
    fake_previewing = true;
    return XIAOTAI_OK;
}

void xiaotai_settings_volume_restore(void) { fake_previewing = false; }

xiaotai_err_t xiaotai_settings_confirm_text(const char *name, const char *value)
{
    return xiaotai_setting_set_text(name, value);
}

xiaotai_err_t xiaotai_stream_stop(uint32_t generation)
{
    ++fake_stream_stop_attempts;
    if (!generation) return XIAOTAI_ERR_INVALID_ARG;
    fake_stop_generation = generation;
    if (fake_stream_stop_error != XIAOTAI_OK) return fake_stream_stop_error;
    if (generation == fake_stream_generation) ++fake_stream_stops;
    return XIAOTAI_OK;
}

#ifdef XIAOTAI_DOOR_WITH_LOCK
size_t xiaotai_lock_count(void) { return fake_lock_count; }

xiaotai_err_t xiaotai_lock_open(uint8_t index, uint32_t hold_ms)
{
    assert(hold_ms == 0 && index < fake_lock_count);
    fake_lock_index = index;
    ++fake_lock_calls;
    return fake_lock_error;
}
#endif

xiaotai_err_t xiaotai_contacts_kind_find(const char *kind, int *index)
{
    if (strcmp(kind, "device") != 0) return XIAOTAI_ERR_NOT_FOUND;
    *index = 1;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_refresh(void)
{
    fake_refreshes++;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_show(int kind, uint16_t page)
{
    if (kind != 1 || page != 0) return XIAOTAI_ERR_INVALID_ARG;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *contact)
{
    if (flags != XIAOTAI_CONTACTS_STALE_OK || !fake_list.generation) return XIAOTAI_ERR_INVALID_STATE;
    if (index >= fake_list.page_count || index >= 3) return XIAOTAI_ERR_NOT_FOUND;
    *contact = fake_contacts[index];
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *contact)
{
    for (uint8_t i = 0; i < fake_list.page_count; ++i) {
        xiaotai_contact_t entry;
        if (xiaotai_contacts_at(i, flags, &entry) == XIAOTAI_OK && entry.kind == kind &&
            strcmp(id, entry.id) == 0) {
            *contact = entry;
            return XIAOTAI_OK;
        }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t store_text(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    if (strcmp(key, XIAOTAI_SETTING_KEY_APP_DOOR_PEER) != 0) return XIAOTAI_ERR_NOT_FOUND;
    (void)snprintf(fake_saved_peer, sizeof(fake_saved_peer), "%s", value);
    return XIAOTAI_OK;
}

static xiaotai_err_t load_text(void *ctx, const char *key, char *value, size_t capacity)
{
    (void)ctx;
    if (strcmp(key, XIAOTAI_SETTING_KEY_APP_DOOR_PEER) != 0 || !fake_saved_peer[0]) return XIAOTAI_ERR_NOT_FOUND;
    if (strlen(fake_saved_peer) >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(value, fake_saved_peer, strlen(fake_saved_peer) + 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t load_int(void *ctx, const char *key, int32_t fallback, int32_t *value)
{
    (void)ctx;
    (void)fallback;
    if (strcmp(key, XIAOTAI_SETTING_KEY_APP_DOOR_SENDER)) return XIAOTAI_ERR_NOT_FOUND;
    *value = fake_saved_sender;
    return XIAOTAI_OK;
}

static xiaotai_err_t store_int(void *ctx, const char *key, int32_t value)
{
    (void)ctx;
    if (!strcmp(key, XIAOTAI_SETTING_KEY_AUDIO_MIC_MUTED)) {
        fake_saved_mute = value != 0;
        ++fake_mic_saves;
    } else if (!strcmp(key, XIAOTAI_SETTING_KEY_AUDIO_VOLUME)) {
        fake_volume = (uint8_t)value;
        ++fake_volume_saves;
    } else if (!strcmp(key, XIAOTAI_SETTING_KEY_APP_DOOR_SENDER)) {
        fake_saved_sender = value != 0;
        ++fake_role_saves;
    } else return XIAOTAI_ERR_NOT_SUPPORTED;
    return XIAOTAI_OK;
}

uint8_t xiaotai_settings_volume_step_percent(void) { return fake_volume_step; }

xiaotai_err_t xiaotai_call_set_saved_mute(uint32_t session, bool muted)
{
    assert(session != 0 && session == fake_call.session);
    ++fake_mics;
    fake_requested_mute = muted;
    if (fake_mic_error != XIAOTAI_OK) return fake_mic_error;
    fake_call.muted = muted;
    return xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, muted);
}

xiaotai_err_t fake_store_init(void)
{
    const xiaotai_setting_store_t store = {
        .load_text = load_text, .store_text = store_text, .load_int = load_int, .store_int = store_int,
    };
    assert(xiaotai_setting_store_set(&store) == XIAOTAI_OK);
    const xiaotai_setting_t mute = {
        .name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .key = XIAOTAI_SETTING_KEY_AUDIO_MIC_MUTED,
        .kind = XIAOTAI_SETTING_BOOL, .min = 0, .max = 1,
    };
    const xiaotai_setting_t volume = {
        .name = XIAOTAI_SETTING_AUDIO_VOLUME, .key = XIAOTAI_SETTING_KEY_AUDIO_VOLUME,
        .kind = XIAOTAI_SETTING_INT, .min = 0, .max = 100, .default_value = 70,
    };
    assert(xiaotai_setting_register(&mute) == XIAOTAI_OK);
    assert(xiaotai_setting_register(&volume) == XIAOTAI_OK);
    return XIAOTAI_OK;
}
