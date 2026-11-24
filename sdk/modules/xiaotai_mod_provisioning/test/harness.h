// SPDX-License-Identifier: MIT
#pragma once
/* Shared harness of the provisioning module's host tests: the real framework
 * on the host port, the module built from source, and fakes of what it calls
 * below the framework (network engine, preferences, platform gate, AT verb
 * tables). Engine state changes and their events go through on_owner(),
 * which also waits until everything queued before has been handled; plain
 * switches the module only polls (results to return, the gate drained) are
 * set from the test thread. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

/* ------------------------------------------------------------ fake engine */

typedef struct {
    xiaotai_net_provision_status_t provision;
    xiaotai_net_status_t link;
    bool provisioning_boot;
    bool exclusive_mode;            /* the product's mode (xiaotai_net_provisioning_exclusive) */
    xiaotai_err_t reprovision_result, close_result, store_retry_result, retry_result, scan_result;
    unsigned reprovisions, closes, store_retries, retries, scans, network_reads;
    bool last_forget;
    /* What a reprovision call saw: the quiesce, the gate drained, nothing pending. */
    bool quiesced_at_reprovision, gate_closed_at_reprovision, settings_pending_at_reprovision;
    char calls[512];                /* "reprovision(0) close ..." in call order */
} fake_net_t;
extern fake_net_t fake_net;

/* Owner task: set the engine's provisioning state (revision advances) and
 * emit net.provision with value and data as the engine does. */
void fake_net_provision(xiaotai_net_provision_state_t state, xiaotai_err_t error);
/* Owner task: emit net.state with the present link status (revision advances). */
void fake_net_link_changed(void);
/* Owner task: the engine finishes the pending request with result: control
 * cleared, and on success either restart_required (exclusive mode) or the
 * link HELD with the methods OPEN (reason REQUESTED). Emits the events. */
void fake_net_finish(xiaotai_err_t result);
void fake_net_reset(void);

/* ------------------------------------------------------------ other fakes */

extern atomic_bool fake_settings_pending;          /* xiaotai_preferences_pending() */

typedef struct {
    xiaotai_err_t close_result;
    bool closed, drained;
    unsigned closes, opens;
    uint32_t ticket;
} fake_gate_t;
extern fake_gate_t fake_gate;                      /* only with the platform core */

/* The verb tables registered with xiaotai_at_verbs_register. */
#define FAKE_AT_TABLES_MAX 8
typedef struct {
    const char *command;
    const xiaotai_at_verb_t *verbs;
    size_t count;
} fake_at_table_t;
extern fake_at_table_t fake_at_tables[FAKE_AT_TABLES_MAX];
extern size_t fake_at_table_count;

/* ---------------------------------------------------------------- harness */

/* Event trace of the probe module: "changed:<phase> opened:<reason>:<ssid>
 * done:<v> failed:<text>:<err> quiesce:<v> restarting" in dispatch order,
 * each followed by a space. */
extern char trace[2048];
void trace_clear(void);
/* Number of times word appears in the trace. */
int trace_count(const char *word);

/* Starts the framework with the provisioning module and a probe module.
 * restart_delay_ms 0: the framework's default. */
int harness_start(uint32_t restart_delay_ms);
void harness_stop(void);
/* Runs fn(ctx) on the owner task and waits until it and everything queued
 * before it ran. */
void on_owner(void (*fn)(void *ctx), void *ctx);
/* Waits until everything queued so far has run. */
void drain(void);
/* Calls on the owner task, returning the result. */
xiaotai_err_t owner_begin(uint32_t flags);
xiaotai_err_t owner_cancel(void);
xiaotai_err_t owner_retry(void);
uint32_t owner_activity_begin(void);   /* begins activity "call", returns its id (0: refused) */
bool owner_quiesced(void);
/* The current phase. */
xiaotai_provisioning_phase_t phase(void);
/* Polls (every 5 ms) until phase() is the given one or timeout_ms passed. */
bool wait_phase(xiaotai_provisioning_phase_t expected, uint32_t timeout_ms);
/* Polls until fake_net.reprovisions reaches count or timeout_ms passed. */
bool wait_reprovisions(unsigned count, uint32_t timeout_ms);
/* Runs one console line through the framework; the reply text lands in reply_text. */
extern char reply_text[1024];
xiaotai_err_t command(const char *line);
/* Restarts the fake device reported (the framework's restart function). */
extern atomic_int restarts;
extern atomic_int_fast64_t probe_restart_us;
extern atomic_int_fast64_t probe_restarting_us;
extern atomic_bool probe_restart_off_owner;
/* When set, the probe takes a restart hold through the module in its
 * handler of XIAOTAI_EVENT_PROVISIONING_DONE (an application's prompt). */
extern atomic_bool probe_hold_on_done;
extern xiaotai_provisioning_store_fn_t probe_store_observer;
bool on_owner_with_store_pressure(xiaotai_net_provision_state_t state, void (*fn)(void *ctx), void *ctx);
/* Revocations of the probe's activity. */
extern atomic_int revocations;
extern atomic_int probe_guard_calls;
extern atomic_bool probe_guard_blocked;
extern atomic_int probe_settings_flushes;
/* One diagnostics section into reply_text. */
void diag_section(const char *name);
/* Holds the framework counted: read from its diagnostics line. */
int framework_holds(void);
int64_t now_ms(void);
