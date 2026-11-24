// SPDX-License-Identifier: MIT
#include "fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "xiaotai/apps/voice_terminal.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/port/os.h"

xiaotai_wechat_status_t fake_call;
xiaotai_net_state_t fake_network;
xiaotai_platform_phase_t fake_platform;
bool fake_setup, fake_bound, fake_has_identity;
xiaotai_err_t fake_lookup, fake_request;
const xiaotai_feedback_table_t *fake_table;
const char *fake_action, *fake_dial_id, *fake_log[128];
uint32_t fake_session;
unsigned fake_refreshes;
size_t fake_used;
static atomic_uint edges;
static atomic_llong now_us = 1000000;
static atomic_bool rebooted;
xiaotai_binding_status_t fake_binding;
xiaotai_provisioning_status_t fake_provision;
xiaotai_environment_status_t fake_environment;
xiaotai_err_t fake_error = XIAOTAI_ERR_NOT_SUPPORTED, fake_announce, fake_sound_error;
unsigned fake_forgets, fake_begins, fake_cancels, fake_retries, fake_holds, fake_spoken, fake_stops, fake_sound_cancels;
bool fake_saved_wifi = true, fake_saved_identity = true, fake_sound_pending, fake_direct;
xiaotai_feedback_options_t fake_feedback;
static xiaotai_binding_forget_done_fn_t binding_done;
static void *binding_ctx;
static xiaotai_provisioning_store_fn_t provision_done;
static void *provision_ctx;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }
void fake_advance(uint32_t ms) { atomic_fetch_add(&now_us, (int64_t)ms * 1000); }
void fake_restart(void) { atomic_store(&rebooted, true); }
bool fake_restarted(void) { return atomic_load(&rebooted); }

void fake_clear(void) { fake_action = fake_dial_id = NULL; fake_session = 0; fake_used = 0; }
void fake_edges(uint32_t value) { atomic_store(&edges, value); }
static uint32_t poll(void *self) { (void)self; return atomic_exchange(&edges, 0); }
static xiaotai_err_t open_button(void *self) { (void)self; return XIAOTAI_OK; }
static const xiaotai_input_t button = {
    .desc = {.role = "main", .events = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED},
    .open = open_button, .poll = poll,
};
const xiaotai_input_t *xiaotai_hal_input(size_t index) { return index ? NULL : &button; }
xiaotai_err_t __wrap_interaction_poll_start(void);
xiaotai_err_t __wrap_interaction_poll_start(void) { return XIAOTAI_OK; }

void xiaotai_wechat_status(xiaotai_wechat_status_t *out) { *out = fake_call; }
static xiaotai_err_t action(const char *name, uint32_t session)
{
    assert(xiaotai_on_owner());
    fake_action = name;
    fake_session = session;
    return fake_request;
}
xiaotai_err_t xiaotai_wechat_dial(const char *id, uint32_t flags, uint32_t *session)
{
    assert((!strcmp(id, "contact") || !strcmp(id, "contact2")) && !flags && !session);
    fake_dial_id = !strcmp(id, "contact") ? "contact" : "contact2";
    return action("dial", 0);
}
xiaotai_err_t xiaotai_wechat_accept(uint32_t session) { return action("accept", session); }
xiaotai_err_t xiaotai_wechat_reject(uint32_t session) { return action("reject", session); }
xiaotai_err_t xiaotai_wechat_hangup(uint32_t session) { return action("hangup", session); }
xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind)
{
    assert(!strcmp(name, XIAOTAI_WECHAT_CONTACT_KIND));
    *kind = 2;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_contacts_at_kind(int kind, uint16_t index, uint32_t flags, xiaotai_contact_t *contact)
{
    assert(kind == 2 && flags == XIAOTAI_CONTACTS_STALE_OK);
    if (index > 1) return XIAOTAI_ERR_NOT_FOUND;
    *contact = (xiaotai_contact_t){.kind = 2};
    strcpy(contact->id, index ? "contact2" : "contact");
    return fake_lookup;
}
xiaotai_err_t xiaotai_contacts_refresh(void) { ++fake_refreshes; return XIAOTAI_OK; }
void xiaotai_net_status(xiaotai_net_status_t *out) { *out = (xiaotai_net_status_t){.state = fake_network}; }
void xiaotai_platform_status(xiaotai_platform_status_t *out) { *out = (xiaotai_platform_status_t){.phase = fake_platform}; }
bool xiaotai_provisioning_active(void) { return fake_setup; }
void xiaotai_binding_status(xiaotai_binding_status_t *out)
{
    *out = fake_binding;
    if (!out->phase) out->phase = fake_bound ? XIAOTAI_BINDING_BOUND : XIAOTAI_BINDING_NEEDED;
    if (fake_has_identity) strcpy(out->device_id, "device");
}

static void record(const char *name)
{
    assert(xiaotai_on_owner() && fake_used < sizeof(fake_log) / sizeof(*fake_log));
    fake_log[fake_used++] = name;
}
static void effect_event(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    record(((const xiaotai_feedback_effect_t *)ctx)->name);
}
xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table)
{
    fake_table = table;
    for (size_t i = 0; i < table->effect_count; ++i) {
        const xiaotai_feedback_effect_t *effect = &table->effects[i];
        if (effect->event) assert(xiaotai_event_subscribe(effect->event, effect_event, (void *)effect) == XIAOTAI_OK);
    }
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_feedback_set(const char *name, bool active)
{
    if (!active && !strcmp(name, XIAOTAI_APP_VOICE_TERMINAL_STATE_RING)) record("-ring");
    else if (!active && !strcmp(name, XIAOTAI_APP_VOICE_TERMINAL_STATE_RINGBACK)) record("-ringback");
    else if (active) record(name);
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_feedback_fire(const char *effect, const xiaotai_feedback_options_t *options)
{
    record(effect);
    if (options) {
        assert(options->done && options->valid_ms);
        fake_feedback = *options;
        return fake_sound_error;
    }
    return XIAOTAI_OK;
}

bool xiaotai_feedback_sound_pending(void) { return fake_sound_pending; }
xiaotai_err_t xiaotai_feedback_cancel(const char *effect, bool stop_playing)
{
    assert(xiaotai_on_owner() && effect && stop_playing);
    ++fake_sound_cancels;
    return XIAOTAI_OK;
}

void xiaotai_provisioning_status(xiaotai_provisioning_status_t *out) { *out = fake_provision; }
xiaotai_err_t xiaotai_provisioning_observe_store(xiaotai_provisioning_store_fn_t fn, void *ctx)
{
    assert(fn && !provision_done);
    provision_done = fn;
    provision_ctx = ctx;
    return XIAOTAI_OK;
}
void fake_provision_result(xiaotai_err_t result, bool restart)
{
    assert(xiaotai_on_owner() && provision_done);
    provision_done(result, restart, provision_ctx);
}
void xiaotai_provisioning_restart_hold(void) { assert(xiaotai_on_owner()); ++fake_holds; xiaotai_restart_hold(); }
void xiaotai_provisioning_restart_release(void)
{
    assert(xiaotai_on_owner() && fake_holds);
    --fake_holds;
    xiaotai_restart_release();
}
xiaotai_err_t xiaotai_provisioning_begin(uint32_t flags)
{
    assert(xiaotai_on_owner() && fake_holds && flags == (XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT));
    ++fake_begins;
    return fake_error;
}
xiaotai_err_t xiaotai_provisioning_retry(void) { assert(xiaotai_on_owner()); ++fake_retries; return fake_error; }

void xiaotai_environment_status(xiaotai_environment_status_t *out) { *out = fake_environment; }
xiaotai_err_t xiaotai_environment_cancel(void) { assert(xiaotai_on_owner()); ++fake_cancels; return fake_error; }

xiaotai_err_t xiaotai_binding_forget_local(bool direct, xiaotai_binding_forget_done_fn_t done, void *ctx)
{
    assert(xiaotai_on_owner() && fake_holds && done);
    ++fake_forgets;
    fake_direct = direct;
    binding_done = done;
    binding_ctx = ctx;
    return fake_error;
}
void fake_binding_done(void *result)
{
    assert(xiaotai_on_owner() && binding_done);
    const xiaotai_err_t err = *(const xiaotai_err_t *)result;
    if (err == XIAOTAI_OK && fake_saved_identity) {
        fake_saved_identity = false;
        assert(xiaotai_restart_request_direct(XIAOTAI_QUIESCE_BINDING) == XIAOTAI_OK);
    }
    binding_done(err, binding_ctx);
}
xiaotai_err_t xiaotai_binding_announce(void)
{
    assert(xiaotai_on_owner());
    ++fake_spoken;
    if (fake_binding.announcing) return XIAOTAI_ERR_BUSY;
    if (fake_announce == XIAOTAI_OK) fake_binding.announcing = true;
    return fake_announce;
}
void xiaotai_binding_announce_stop(void)
{
    assert(xiaotai_on_owner());
    if (fake_binding.announcing) ++fake_stops;
    fake_binding.announcing = false;
}
