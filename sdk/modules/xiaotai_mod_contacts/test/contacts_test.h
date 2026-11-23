// SPDX-License-Identifier: MIT
#pragma once
/* Shared by the contacts module's host tests: checks, the framework started
 * with the module, a fake call module that registers kinds at init, an
 * observer of contacts.changed, and builders of list replies. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fake_platform.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/contacts.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

/* The kinds the two call modules of today register. */
extern const xiaotai_contact_kind_def_t TEST_KIND_DEVICE;
extern const xiaotai_contact_kind_def_t TEST_KIND_WECHAT;
/* The voip service's list: no type field, every entry of type "voip". */
extern const xiaotai_contacts_source_t TEST_SOURCE_VOIP;

typedef struct {
    const xiaotai_contacts_config_t *config;           /* NULL: defaults */
    const xiaotai_contact_kind_def_t *const *kinds;    /* registered at init, in order */
    size_t kind_count;
    bool provisioning_boot;
    xiaotai_event_fn_t change_done;
} harness_setup_t;

/* Configures the module, starts the framework with it, a call module that
 * registers setup->kinds and an observer; checks success. */
void harness_start(const harness_setup_t *setup);
/* The same, returning the framework's result. */
xiaotai_err_t harness_try_start(const harness_setup_t *setup);
/* Stops the framework and forgets the module, the fake platform, the
 * identity's generation and the observer. */
void harness_stop(void);

/* Everything queued to the owner task so far has run. */
void harness_drain(void);
/* Emit an event and wait until it was handled. */
void harness_emit(const char *name, int32_t value, const char *text);
void harness_online(void);
void harness_offline(void);
void harness_profile(bool accepted);
/* The binding changed (the generation advances, binding.needed follows). */
void harness_rebind(void);

/* [owner] functions, called on the owner task. */
xiaotai_err_t harness_refresh(void);
xiaotai_err_t harness_show(int kind, uint16_t page);
xiaotai_err_t harness_query(int kind, uint32_t timeout_ms);
xiaotai_err_t harness_query_cancel(bool foreign);
void harness_activity_begin(void);
void harness_activity_end(void);

/* The query's answer, copied when it arrived. */
typedef struct {
    int calls;
    xiaotai_err_t result;
    size_t count;
    bool truncated;
    bool deadline_expired;
    size_t response_bytes;
    char first_remark[128];
    xiaotai_contact_t contacts[64];
} harness_answer_t;
const harness_answer_t *harness_answer(void);

/* contacts.changed events seen and the payload of the last one. */
int harness_changed_events(void);
xiaotai_contacts_status_t harness_last_changed(void);

xiaotai_contacts_status_t harness_status(void);
xiaotai_contacts_stats_t harness_stats(void);
/* The kind number registration gave the i-th kind of the setup. */
int harness_kind(size_t index);

/* A call-service reply ({"code":200,"data":{"contacts":[…]}}) with the
 * entries given as JSON objects joined by commas. The text is the harness's
 * and stays valid until the next call. */
const char *harness_call_reply(const char *entries);
/* A call-service reply with `count` entries: device "d00", "d01", … named
 * "Dev 00", …; every `wechat_every`-th one (0: none) a voip entry "w00" … with
 * wx_app_id "app". */
const char *harness_numbered_reply(unsigned count, unsigned wechat_every);
