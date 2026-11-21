// SPDX-License-Identifier: MIT
#pragma once
/* Seams between the provisioning module's sources; not part of the contract.
 *
 * Every function here runs on the framework owner task, except those marked
 * [any]. The module's state is owned by that task; the published snapshot and
 * the counters are copied under prov.lock so [any] readers get one moment. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/provisioning_config.h"
#include "xiaotai/port/os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event-log source and the diagnostics section. */
#define PROV_SOURCE "provisioning"
/* Period of the settle and hold polls (2.3 step 4, 2.4 of the specification). */
#define PROV_POLL_MS 100U

/* Where a begin stands. */
typedef enum {
    PROV_BEGIN_IDLE = 0,
    PROV_BEGIN_SETTLING, /* quiesced (and gated), waiting for settings and the gate */
    PROV_BEGIN_HELD,     /* stopped on a setting failure, everything kept until cancel */
    PROV_BEGIN_HANDED,   /* xiaotai_net_reprovision() accepted */
} prov_begin_t;

typedef struct {
    bool initialized;
    bool started;
    bool restarting;              /* XIAOTAI_EVENT_RESTARTING was seen */
    xiaotai_err_t error;          /* status.error */

    /* The begin in progress. */
    prov_begin_t begin;
    uint32_t flags;
    bool exclusive_mode;          /* a restart follows the request (decided at begin) */
    bool direct_begin;             /* local product policy, no quiesce/gate/settle */
    uint32_t quiesce;             /* framework quiesce ticket, 0 none */
    uint32_t gate;                /* platform gate ticket, 0 none */
    int64_t deadline_us;          /* end of the settle wait; 0 none */
    uint32_t settle_timer;
    xiaotai_err_t settle_failure; /* a setting not saved during the wait */
    bool restart_handed;          /* the quiesce went to xiaotai_restart_request */
    bool restart_handoff_failed;  /* commit succeeded; framework refused direct restart */
    uint32_t restart_notice_timer;

    /* The restart hold after stored credentials. */
    bool holding;
    bool hold_settings;           /* also wait for preferences, not just for DONE delivery */
    bool hold_failed;             /* a setting failed; kept until retry (option) */
    int64_t hold_deadline_us;
    uint32_t hold_timer;
    bool hold_check_queued;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    uint32_t hold_epoch;
#endif

    xiaotai_provisioning_store_fn_t store_observer;
    void *store_observer_ctx;

    uint32_t store_retry_timer;
    /* Engine provisioning state of the last net.provision event handled. */
    xiaotai_net_provision_state_t last_state;
    bool seen_state;

    xiaotai_mutex_t lock;
    xiaotai_provisioning_status_t status;   /* published; under lock */
    xiaotai_provisioning_stats_t stats;     /* under lock */
} prov_state_t;

extern prov_state_t prov;

/* Takes both engine snapshots, derives the status and, when anything changed,
 * publishes it and emits XIAOTAI_EVENT_PROVISIONING_CHANGED (when emit). */
void prov_refresh(bool emit);
/* Adds one to a counter of prov.stats under the lock. */
void prov_count(uint32_t *counter);
/* Records one event-log entry of this module. */
void prov_log(const char *label, xiaotai_err_t value, bool failure);
/* Emits XIAOTAI_EVENT_PROVISIONING_FAILED. */
xiaotai_err_t prov_emit_failed(xiaotai_err_t error, const char *what);
void prov_restart_refused(xiaotai_err_t error);

/* provisioning_begin.c */
xiaotai_err_t prov_begin(uint32_t flags, bool local);
/* Drops a begin that has not reached the engine: gate, quiesce, timer. */
void prov_begin_drop(void);
/* The engine's status changed: completes a handed begin. */
void prov_begin_update(const xiaotai_net_provision_status_t *provision, const xiaotai_net_status_t *link);
/* XIAOTAI_EVENT_SETTING_NOT_SAVED. */
void prov_setting_not_saved(xiaotai_err_t error);
/* The restart hold after stored credentials. */
void prov_hold_take(bool settings);
void prov_hold_release(void);
void prov_hold_wait(void);
bool prov_hold_rearm(void);
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
bool prov_hold_packed(void);
xiaotai_err_t prov_hold_retry_packed(void);
#endif
void prov_store_retry_schedule(void);
void prov_begin_reset(void);

/* Whether a request to start over restarts the device (the network engine's
 * product configuration). */
bool prov_restart_follows(void);
bool prov_direct_store(void);

/* Platform gate seam: src/with_platform.c with the platform core in the
 * build, src/platform_none.c without it. */
bool prov_gate_present(void);
xiaotai_err_t prov_gate_close(uint32_t *ticket);
bool prov_gate_drained(uint32_t ticket);
void prov_gate_open(uint32_t ticket);

/* provisioning_at.c */
xiaotai_err_t prov_at_register(void);
void prov_diag(const xiaotai_reply_t *out);
/* provisioning_at_forget.c, compiled and registered only with
 * XIAOTAI_NET_CONSOLE_FORGET (xiaotai/net_config.h). */
xiaotai_err_t xiaotai_provisioning_at_forget_register(void);

#ifdef __cplusplus
}
#endif
