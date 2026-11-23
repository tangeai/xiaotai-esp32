// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the contacts module (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Kinds that can be registered in one product (xiaotai_contacts_kind_register,
 * one per call module). Registering beyond it is XIAOTAI_ERR_NO_MEM in the
 * registering module's init(), so the start fails instead of skipping every
 * entry of that kind; a product with more call modules raises the option, and
 * no header changes. It also sizes xiaotai_contacts_status_t.totals. */
#ifdef CONFIG_XIAOTAI_CONTACT_KINDS_MAX
#define XIAOTAI_CONTACT_KINDS_MAX CONFIG_XIAOTAI_CONTACT_KINDS_MAX
#else
#define XIAOTAI_CONTACT_KINDS_MAX 4
#endif

#if XIAOTAI_CONTACT_KINDS_MAX < 1 || XIAOTAI_CONTACT_KINDS_MAX > 127
#error "XIAOTAI_CONTACT_KINDS_MAX must be 1..127: a kind number is an int8_t."
#endif

/* Every fetch of the list (a refresh, a signal, a page outside the cache)
 * waits until the device profile was accepted in the present session, not
 * only the first one of XIAOTAI_CONTACTS_AFTER_PROFILE. 0 (default): once the
 * platform is online a requested fetch is sent at once; the profile only
 * decides when the first automatic fetch of a session happens. 1: for
 * products whose deployed firmware never asks for contacts before its profile
 * was accepted (a profile that keeps failing then leaves every fetch
 * waiting). */
#define XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE, 0)
