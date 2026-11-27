// SPDX-License-Identifier: MIT
#include "companion_contact_route.h"
#include "companion_internal.h"
#ifdef COMPANION_WITH_RECENTS
#include "companion_recents.h"
#endif

xiaotai_err_t companion_dial_contact(const xiaotai_contact_t *contact, bool video)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!contact || !contact->id[0]) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_contact_kind_def_t *kind = xiaotai_contacts_kind(contact->kind);
    if (!kind || (video && !companion_contact_video_supported(contact))) return XIAOTAI_ERR_NOT_SUPPORTED;
#ifdef COMPANION_WITH_RECENTS
    companion_recents_dialling(contact);
#endif
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (!part->dial_contact) continue;
        const xiaotai_err_t err = part->dial_contact(kind->name, contact->id, video);
        if (err != XIAOTAI_ERR_NOT_SUPPORTED) return err;
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

bool companion_contact_video_supported(const xiaotai_contact_t *contact)
{
    const xiaotai_contact_kind_def_t *kind = contact ? xiaotai_contacts_kind(contact->kind) : NULL;
    if (!kind) return false;
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->contact_video_available && part->contact_video_available(kind->name)) return true;
    }
    return false;
}
