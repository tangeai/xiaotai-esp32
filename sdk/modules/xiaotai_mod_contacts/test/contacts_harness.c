// SPDX-License-Identifier: MIT
/* Harness of the contacts module's host tests (contacts_test.h): the real
 * framework on the host port, the module under test, a fake call module that
 * registers kinds in its init (after the contacts module's, as in a product),
 * an observer of contacts.changed, and the fake platform core. */
#include <string.h>
#include "contacts_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"

const xiaotai_contact_kind_def_t TEST_KIND_DEVICE = {
    .name = "device",
    .alias = "DEV",
    .server_type = "device",
    .presence = true,
    .friendships = true,
};

const xiaotai_contact_kind_def_t TEST_KIND_WECHAT = {
    .name = "wechat",
    .alias = "WX",
    .server_type = "voip",
    .extra_fields = {"wx_app_id", "wx_model_id"},
    .extra_required = 1U,
};

const xiaotai_contacts_source_t TEST_SOURCE_VOIP = {
    .service = "voip",
    .path = "/v1/voip/device/contacts",
    .success_code = 0,
    .id_field = "wx_open_id",
    .type_field = NULL,
    .default_type = "voip",
    .friendships = false,
};

static const harness_setup_t *s_setup;
static int s_kinds[XIAOTAI_CONTACT_KINDS_MAX];
static int s_changed;
static xiaotai_contacts_status_t s_last;
static harness_answer_t s_answer;
static uint32_t s_activity;
static char s_text[65536];

bool xiaotai_net_provisioning_boot(void) { return s_setup && s_setup->provisioning_boot; }

/* ------------------------------------------------------------- the modules */

static xiaotai_err_t dialer_init(void)
{
    for (size_t i = 0; s_setup && i < s_setup->kind_count; ++i) {
        xiaotai_err_t err = xiaotai_contacts_kind_register(s_setup->kinds[i], &s_kinds[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

static void on_changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    ++s_changed;
    if (event->size == sizeof(s_last)) memcpy(&s_last, event->data, sizeof(s_last));
}

static xiaotai_err_t observer_init(void)
{
    const xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_CONTACTS_CHANGED, on_changed, NULL);
    if (err != XIAOTAI_OK || !s_setup || !s_setup->change_done) return err;
    return xiaotai_event_subscribe(XIAOTAI_EVENT_CONTACTS_CHANGE_DONE, s_setup->change_done, NULL);
}

static const char *const DIALER_DEPENDS[] = {"contacts", NULL};
static const xiaotai_module_t CONTACTS = {
    .name = "contacts",
    .init = xiaotai_module_contacts_init,
    .start = xiaotai_module_contacts_start,
};
static const xiaotai_module_t DIALER = {.name = "dialer", .depends = DIALER_DEPENDS, .init = dialer_init};
static const xiaotai_module_t OBSERVER = {.name = "observer", .depends = DIALER_DEPENDS, .init = observer_init};
static const xiaotai_module_t *const MODULES[] = {&CONTACTS, &DIALER, &OBSERVER};

xiaotai_err_t harness_try_start(const harness_setup_t *setup)
{
    s_setup = setup;
    xiaotai_err_t err = xiaotai_contacts_configure(setup ? setup->config : NULL);
    if (err != XIAOTAI_OK) return err;
    xiaotai_framework_config_t config = {
        .modules = MODULES,
        .module_count = sizeof(MODULES) / sizeof(MODULES[0]),
    };
    return xiaotai_framework_start(&config);
}

void harness_start(const harness_setup_t *setup)
{
    CHECK(harness_try_start(setup) == XIAOTAI_OK);
    harness_drain();
}

void harness_stop(void)
{
    xiaotai_framework_reset();
    xiaotai_contacts_reset();
    fake_platform_reset();
    xiaotai_identity_reset();
    s_setup = NULL;
    memset(s_kinds, 0, sizeof(s_kinds));
    s_changed = 0;
    memset(&s_last, 0, sizeof(s_last));
    memset(&s_answer, 0, sizeof(s_answer));
    s_activity = 0;
}

int harness_kind(size_t index) { return s_kinds[index]; }

/* -------------------------------------------------------------- owner task */

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
    xiaotai_signal_t done;
} owner_job_t;

static void owner_job(void *ctx)
{
    owner_job_t *job = ctx;
    if (job->fn) job->fn(job->ctx);
    xiaotai_signal_give(&job->done);
}

static void on_owner(void (*fn)(void *ctx), void *ctx)
{
    owner_job_t job = {.fn = fn, .ctx = ctx};
    CHECK(xiaotai_signal_init(&job.done) == XIAOTAI_OK);
    CHECK(xiaotai_run(owner_job, &job) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&job.done, 5000));
    xiaotai_signal_deinit(&job.done);
}

void harness_drain(void)
{
    /* Twice: an event emitted by a handler of the first round is queued
     * behind the first job and handled before the second. */
    on_owner(NULL, NULL);
    on_owner(NULL, NULL);
}

void harness_emit(const char *name, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    CHECK(xiaotai_event_emit(name, &args, NULL, 0) == XIAOTAI_OK);
    harness_drain();
}

void harness_online(void) { harness_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL); }
void harness_offline(void) { harness_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_SIGNALING, NULL); }
void harness_profile(bool accepted) { harness_emit(XIAOTAI_EVENT_PLATFORM_PROFILE, accepted ? 1 : 0, NULL); }

void harness_rebind(void)
{
    (void)xiaotai_identity_generation_advance();
    harness_emit(XIAOTAI_EVENT_BINDING_NEEDED, 1, NULL);
}

typedef struct {
    int kind;
    uint16_t page;
    uint32_t timeout_ms;
    xiaotai_err_t result;
} call_t;

static void call_refresh(void *ctx) { ((call_t *)ctx)->result = xiaotai_contacts_refresh(); }
static void call_show(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_contacts_show(call->kind, call->page);
}

static void on_answer(const xiaotai_contacts_answer_t *answer, void *ctx)
{
    (void)ctx;
    ++s_answer.calls;
    s_answer.result = answer->result;
    s_answer.count = answer->count;
    s_answer.truncated = answer->truncated;
    s_answer.deadline_expired = answer->deadline_expired;
    s_answer.response_bytes = answer->response_bytes;
    s_answer.first_remark[0] = '\0';
    if (answer->count) (void)xiaotai_contacts_answer_remark(answer, 0, s_answer.first_remark,
                                                          sizeof(s_answer.first_remark), NULL);
    for (size_t i = 0; i < answer->count && i < 64U; ++i) s_answer.contacts[i] = answer->contacts[i];
}

static void call_query(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_contacts_query(call->kind, call->timeout_ms, on_answer, NULL);
}

xiaotai_err_t harness_refresh(void)
{
    call_t call = {0};
    on_owner(call_refresh, &call);
    harness_drain();
    return call.result;
}

xiaotai_err_t harness_show(int kind, uint16_t page)
{
    call_t call = {.kind = kind, .page = page};
    on_owner(call_show, &call);
    harness_drain();
    return call.result;
}

xiaotai_err_t harness_query(int kind, uint32_t timeout_ms)
{
    call_t call = {.kind = kind, .timeout_ms = timeout_ms};
    on_owner(call_query, &call);
    harness_drain();
    return call.result;
}

static void call_query_cancel(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_contacts_query_cancel(on_answer, call->kind ? ctx : NULL);
}

xiaotai_err_t harness_query_cancel(bool foreign)
{
    call_t call = {.kind = foreign};
    on_owner(call_query_cancel, &call);
    return call.result;
}

const harness_answer_t *harness_answer(void) { return &s_answer; }

static void activity_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id;
    (void)reason;
    (void)ctx;
}

static void begin_activity(void *ctx)
{
    (void)ctx;
    xiaotai_activity_t activity = {
        .name = "call",
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT,
        .revoked = activity_revoked,
    };
    CHECK(xiaotai_activity_begin(&activity, &s_activity) == XIAOTAI_OK);
}

static void end_activity(void *ctx)
{
    (void)ctx;
    xiaotai_activity_end(s_activity);
    s_activity = 0;
}

void harness_activity_begin(void)
{
    on_owner(begin_activity, NULL);
    harness_drain();
}

void harness_activity_end(void)
{
    on_owner(end_activity, NULL);
    harness_drain();
}

/* ------------------------------------------------------------- observation */

int harness_changed_events(void)
{
    harness_drain();
    return s_changed;
}

xiaotai_contacts_status_t harness_last_changed(void)
{
    harness_drain();
    return s_last;
}

xiaotai_contacts_status_t harness_status(void)
{
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    return status;
}

xiaotai_contacts_stats_t harness_stats(void)
{
    xiaotai_contacts_stats_t stats;
    xiaotai_contacts_stats(&stats);
    return stats;
}

/* ------------------------------------------------------------------ replies */

const char *harness_call_reply(const char *entries)
{
    snprintf(s_text, sizeof(s_text), "{\"code\":200,\"msg\":\"ok\",\"data\":{\"contacts\":[%s]}}", entries);
    return s_text;
}

const char *harness_numbered_reply(unsigned count, unsigned wechat_every)
{
    static char entries[60000];
    size_t used = 0;
    entries[0] = '\0';
    for (unsigned i = 0; i < count; ++i) {
        bool wechat = wechat_every && i % wechat_every == wechat_every - 1U;
        int n = wechat ? snprintf(entries + used, sizeof(entries) - used,
                                  "%s{\"type\":\"voip\",\"device_id\":\"w%02u\",\"remark\":\"Wx %02u\","
                                  "\"wx_app_id\":\"app\"}",
                                  i ? "," : "", i, i)
                       : snprintf(entries + used, sizeof(entries) - used,
                                  "%s{\"type\":\"device\",\"device_id\":\"d%02u\",\"remark\":\"Dev %02u\","
                                  "\"online\":%s}",
                                  i ? "," : "", i, i, i % 2U ? "false" : "true");
        CHECK(n > 0 && (size_t)n < sizeof(entries) - used);
        used += (size_t)n;
    }
    return harness_call_reply(entries);
}
