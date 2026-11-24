// SPDX-License-Identifier: MIT
#pragma once
/* Fakes of the engines the transport core calls (identity, platform core,
 * network engine), for the transport's host tests. The real framework, event
 * log and host port are linked; these engines have no host implementation
 * the tests could drive, so the test plays them. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaotai/identity.h"
#include "xiaotai/media.h"
#include "xiaotai/media_uplink.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"

/* Everything back to: no identity, nothing discovered, network stopped
 * without MAC, platform stopped, no hold, holds suspend at once. */
void fakes_reset(void);
bool fake_media_uplink_wake(xiaotai_media_uplink_work_t (*step)(void));

/* ---- identity ---- */
void fake_identity_set(xiaotai_identity_state_t state, const char *device_id, const char *device_key,
                       const char *client_id);

/* ---- platform core ---- */
/* Answer of xiaotai_platform_endpoint("tirtc"): result and, on XIAOTAI_OK, url. */
void fake_platform_endpoint(xiaotai_err_t result, const char *url);
void fake_platform_phase(xiaotai_platform_phase_t phase);
void fake_platform_gated(bool gated);
bool fake_platform_profile_ready(void);

/* One signaling hold call, as the platform saw it. */
typedef struct {
    bool take;                   /* true: hold taken; false: released */
    uint32_t hold;
    int64_t at_ms;               /* engine clock */
    /* The transport's view when the platform got the call. */
    bool connect_reserve_held;
    bool start_reserve_held;
    int state;
} fake_hold_record_t;

size_t fake_platform_hold_records(fake_hold_record_t *out, size_t capacity);
/* Holds taken and not released. */
uint32_t fake_platform_holds_active(void);
/* true (default): "suspended" runs on the owner task right after a hold is
 * taken; false: only when the test calls fake_platform_suspend(). */
void fake_platform_hold_auto(bool automatic);
void fake_platform_suspend(void);

/* ---- network engine ---- */
void fake_net_set(xiaotai_net_state_t state, bool mac_valid);
void fake_net_provisioning(bool provisioning);
void fake_net_trace(bool active, bool transport);

/* Default downlink consumer. Snapshot contains no borrowed payload pointer. */
typedef struct {
    uint32_t calls;
    uint32_t generation;
    xiaotai_media_packet_t frame;
    uint8_t first_byte;
} fake_media_record_t;

void fake_media_accept(bool accept);
void fake_media_opus(bool available);
fake_media_record_t fake_media_record(void);
