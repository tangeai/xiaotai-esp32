// SPDX-License-Identifier: MIT
/* Finding a contact by a spoken or typed name. A name that an assistant or a
 * user supplied is only ever resolved against the cached list: the match must
 * be unique, and an id given along with it must agree; an id is never taken
 * on its own. */
#include <string.h>
#include "contacts_internal.h"

static bool ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool contacts_normalize_name(const char *input, char *output, size_t capacity)
{
    if (!input || !output || capacity == 0) return false;
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor; ++cursor) {
        if (ascii_space(*cursor)) continue;
        if (used + 1U >= capacity) {
            output[0] = '\0';
            return false;
        }
        unsigned char c = *cursor;
        output[used++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    output[used] = '\0';
    return used > 0;
}

xiaotai_err_t xiaotai_contacts_match(const char *name, int kind, const char *id, uint32_t flags,
                                     xiaotai_contact_t *contact, size_t *matches)
{
    if (matches) *matches = 0;
    if (!contact || (kind != XIAOTAI_CONTACT_ANY && !contacts_kind_valid(kind))) return XIAOTAI_ERR_INVALID_ARG;
    char target[XIAOTAI_CONTACT_NAME_MAX + 1U];
    bool by_id = id && id[0];
    contacts_lock();
    xiaotai_err_t err = contacts_usable_locked(flags);
    if (err != XIAOTAI_OK) {
        contacts_unlock();
        return err;
    }
    size_t found = 0;
    const xiaotai_contact_t *match = NULL;
    if (contacts_normalize_name(name, target, sizeof(target))) {
        for (uint16_t i = 0; i < g_contacts.status.cached; ++i) {
            const xiaotai_contact_t *entry = &g_contacts.cache[i];
            if (kind != XIAOTAI_CONTACT_ANY && entry->kind != kind) continue;
            /* An entry without a name is known by its id, which is what the
             * device shows for it. */
            char candidate[XIAOTAI_CONTACT_ID_MAX + 1U];
            const char *shown = entry->name[0] ? entry->name : entry->id;
            if (!contacts_normalize_name(shown, candidate, sizeof(candidate)) || strcmp(candidate, target) != 0) {
                continue;
            }
            ++found;
            match = entry;
        }
    }
    if (found > 1) {
        err = XIAOTAI_ERR_INVALID_STATE;
        if (matches) *matches = found;
    } else if (found == 1 && (!by_id || strcmp(match->id, id) == 0)) {
        *contact = *match;
        if (matches) *matches = 1;
    } else {
        err = XIAOTAI_ERR_NOT_FOUND;
    }
    contacts_unlock();
    return err;
}
