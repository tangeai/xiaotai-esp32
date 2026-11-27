// SPDX-License-Identifier: MIT
#pragma once
/* Build options of the provisioning module (xiaotai/config.h explains the
 * scheme). On ESP-IDF the values come from this component's Kconfig;
 * elsewhere the defaults below apply, and a host test overrides one on the
 * compiler command line (-DCONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=600).
 *
 * The defaults are the behaviour xiaotai/modules/provisioning.h describes.
 * The other values exist so that a product keeps what its deployed firmware
 * did where that differs (a wait without a bound, a begin that stays held
 * after a setting could not be written, a store failure retried on a fixed
 * period); the product configuration selects them. */
#include "xiaotai/config.h"

/* Local product provisioning policy. Intents from external callers and
 * console commands cannot select a direct policy. Match the policy to the
 * product's network mode; a mismatch rejects a local begin. */
#define XIAOTAI_PROVISIONING_LOCAL_ORDERLY 0
#define XIAOTAI_PROVISIONING_LOCAL_DIRECT_LINK 1
#define XIAOTAI_PROVISIONING_LOCAL_DIRECT_RESTART 2
#ifdef CONFIG_XIAOTAI_PROVISIONING_LOCAL_POLICY
#define XIAOTAI_PROVISIONING_LOCAL_POLICY CONFIG_XIAOTAI_PROVISIONING_LOCAL_POLICY
#else
#define XIAOTAI_PROVISIONING_LOCAL_POLICY XIAOTAI_PROVISIONING_LOCAL_ORDERLY
#endif
#if XIAOTAI_PROVISIONING_LOCAL_POLICY < XIAOTAI_PROVISIONING_LOCAL_ORDERLY || \
    XIAOTAI_PROVISIONING_LOCAL_POLICY > XIAOTAI_PROVISIONING_LOCAL_DIRECT_RESTART
#error "Invalid XiaoTai local provisioning policy"
#endif

/* Longest wait, in milliseconds, for accepted setting changes to reach
 * storage and for the platform gate to drain: steps 2 and 3 of
 * xiaotai_provisioning_begin() share it, and the restart hold this module
 * takes after stored credentials is released at the latest when it ends.
 * 0: no bound, the wait ends only when everything is written (or a write
 * failed). Range 0..60000. */
#ifdef CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS
#define XIAOTAI_PROVISIONING_SETTLE_WAIT_MS CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS
#else
#define XIAOTAI_PROVISIONING_SETTLE_WAIT_MS 5000
#endif

/* What a begin does, where a restart follows, when a setting could not be
 * written during the wait (XIAOTAI_EVENT_SETTING_NOT_SAVED) or the wait ran
 * out:
 *   0 release   stop: open the platform gate, end the quiesce, report the
 *               failure; the user may begin again
 *   1 hold      stop and keep the quiesce and the gate, report the failure;
 *               only xiaotai_provisioning_cancel() leaves (a new begin is
 *               XIAOTAI_ERR_INVALID_STATE until then), so nothing runs with
 *               the setting unsaved
 *   2 continue  count it and hand the request to the network engine anyway,
 *               as where provisioning is concurrent
 * Where provisioning is concurrent the wait always continues. */
#define XIAOTAI_PROVISIONING_BEGIN_FAILURE_RELEASE 0
#define XIAOTAI_PROVISIONING_BEGIN_FAILURE_HOLD 1
#define XIAOTAI_PROVISIONING_BEGIN_FAILURE_CONTINUE 2
#ifdef CONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE
#define XIAOTAI_PROVISIONING_BEGIN_FAILURE CONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE
#else
#define XIAOTAI_PROVISIONING_BEGIN_FAILURE XIAOTAI_PROVISIONING_BEGIN_FAILURE_RELEASE
#endif

/* After stored credentials: a setting that could not be written while this
 * module holds the restart (XIAOTAI_EVENT_SETTING_NOT_SAVED) keeps the hold.
 * The failure is reported (status.error, XIAOTAI_EVENT_PROVISIONING_FAILED
 * with text "store") and xiaotai_provisioning_retry() waits for the settings
 * again. Off: the failure is counted and the restart goes ahead. */
#define XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD, 0)

#ifdef CONFIG_XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
#define XIAOTAI_PROVISIONING_STORE_PACKED_RETRY CONFIG_XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
#else
#define XIAOTAI_PROVISIONING_STORE_PACKED_RETRY 0
#endif

/* Accepted credentials that could not be stored (phase STORE_FAILED) are
 * stored again every this many milliseconds, until stored or until the phase
 * changes. 0: only an explicit xiaotai_provisioning_retry() stores again.
 * Range 0..600000. */
#ifdef CONFIG_XIAOTAI_PROVISIONING_STORE_RETRY_MS
#define XIAOTAI_PROVISIONING_STORE_RETRY_MS CONFIG_XIAOTAI_PROVISIONING_STORE_RETRY_MS
#else
#define XIAOTAI_PROVISIONING_STORE_RETRY_MS 0
#endif
