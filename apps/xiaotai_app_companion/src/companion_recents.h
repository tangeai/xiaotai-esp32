// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_COMPANION_RECENTS_H
#define XIAOTAI_COMPANION_RECENTS_H

#include "companion_internal.h"
#include "xiaotai/modules/contacts.h"

/* Recent calls: who called and who was called since the device started, the
 * newest first, one entry for each person. Kept in memory only. Compiled with
 * the contacts module and a call module; used by touch displays. */
xiaotai_err_t companion_recents_init(void);
void companion_recents_reset(void);
bool companion_recents_any(void);
/* Whom the next outgoing call is for: the call modules tell names, not who a
 * WeChat contact is. */
void companion_recents_dialling(const xiaotai_contact_t *contact);
bool companion_recents_home_notice(const char **text);

/* [with_contacts.c] The card of a contact, with its call buttons. */
void companion_contact_open(const xiaotai_contact_t *contact);
/* [with_contacts.c] Dial and say what happened. */
void companion_contact_dial(const xiaotai_contact_t *contact, bool video);

#endif
