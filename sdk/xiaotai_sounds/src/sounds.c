// SPDX-License-Identifier: MIT
/* Sound index: registration, lookup by name, enumeration and digit names.
 *
 * No task, no queue, no heap. The registry is a static array of pack pointers
 * (internal RAM); packs, tables, names and data are the callers' static
 * constants (firmware read-only data), so nothing is copied. Packs are
 * registered during init only and the index is read-only afterwards, which is
 * why lookups and listings take no lock. Lookups are linear: the largest pack
 * has a few dozen entries and names are resolved when a play request is
 * accepted, never on a real-time path. */
#include <stdbool.h>
#include <string.h>
#include "xiaotai/sounds.h"

/* Built-in packs plus the ones applications add at init. */
#define SOUNDS_PACKS_MAX 8U

static const xiaotai_sound_pack_t *s_packs[SOUNDS_PACKS_MAX];
static size_t s_pack_count;

/* Lower case letters, digits and '_' in non-empty segments separated by '.',
 * 1..XIAOTAI_SOUND_NAME_MAX characters. */
static bool name_valid(const char *name)
{
    size_t length = 0;
    bool segment_empty = true;
    for (const char *p = name; *p; ++p, ++length) {
        if (length >= XIAOTAI_SOUND_NAME_MAX) return false;
        char c = *p;
        if (c == '.') {
            if (segment_empty) return false;
            segment_empty = true;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        segment_empty = false;
    }
    return length > 0 && !segment_empty;
}

static xiaotai_err_t entry_valid(const xiaotai_sound_t *sound)
{
    if (!sound->name || !sound->data || !sound->size) return XIAOTAI_ERR_INVALID_ARG;
    if (!XIAOTAI_CODEC_IS_AUDIO(sound->encoding)) return XIAOTAI_ERR_INVALID_ARG;
    if (sound->size > XIAOTAI_SOUND_BYTES_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!name_valid(sound->name)) return XIAOTAI_ERR_INVALID_ARG;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_sounds_register(const xiaotai_sound_pack_t *pack)
{
    if (!pack || !pack->name || !pack->sounds || !pack->count) return XIAOTAI_ERR_INVALID_ARG;
    /* Every entry is checked before the pack is added, so a refused pack
     * leaves the index as it was. A name twice in one pack would make the
     * answer depend on table order: refused. */
    for (size_t i = 0; i < pack->count; ++i) {
        xiaotai_err_t err = entry_valid(&pack->sounds[i]);
        if (err != XIAOTAI_OK) return err;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(pack->sounds[j].name, pack->sounds[i].name)) return XIAOTAI_ERR_INVALID_ARG;
    }
    if (s_pack_count >= SOUNDS_PACKS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_packs[s_pack_count++] = pack;
    return XIAOTAI_OK;
}

static const xiaotai_sound_t *find_in(size_t packs, const char *name)
{
    for (size_t p = packs; p-- > 0;) {
        const xiaotai_sound_pack_t *pack = s_packs[p];
        for (size_t i = 0; i < pack->count; ++i)
            if (!strcmp(pack->sounds[i].name, name)) return &pack->sounds[i];
    }
    return NULL;
}

const xiaotai_sound_t *xiaotai_sounds_find(const char *name)
{
    if (!name) return NULL;
    return find_in(s_pack_count, name);
}

const char *xiaotai_sounds_digit(unsigned digit)
{
    static const char *const names[10] = {
        "digit.0", "digit.1", "digit.2", "digit.3", "digit.4",
        "digit.5", "digit.6", "digit.7", "digit.8", "digit.9",
    };
    return digit < 10U ? names[digit] : NULL;
}

/* Distinct names are listed in the order they first appear (registration
 * order, then table order); each is answered by the sound a lookup returns,
 * that is, by the last pack that has the name. Stores the sound of the
 * index-th distinct name in *found; returns the number of distinct names
 * walked (all of them when index is past the end). */
static size_t distinct_walk(size_t index, const xiaotai_sound_t **found)
{
    size_t seen = 0;
    for (size_t p = 0; p < s_pack_count; ++p) {
        const xiaotai_sound_pack_t *pack = s_packs[p];
        for (size_t i = 0; i < pack->count; ++i) {
            const char *name = pack->sounds[i].name;
            /* Names are unique inside a pack, so only earlier packs can
             * already have listed this one. */
            if (find_in(p, name)) continue;
            if (seen == index) {
                *found = xiaotai_sounds_find(name);
                return seen;
            }
            ++seen;
        }
    }
    return seen;
}

size_t xiaotai_sounds_count(void)
{
    const xiaotai_sound_t *unused = NULL;
    return distinct_walk(SIZE_MAX, &unused);
}

const xiaotai_sound_t *xiaotai_sounds_at(size_t index)
{
    const xiaotai_sound_t *found = NULL;
    if (index == SIZE_MAX) return NULL;
    distinct_walk(index, &found);
    return found;
}

void xiaotai_sounds_reset(void)
{
    memset(s_packs, 0, sizeof s_packs);
    s_pack_count = 0;
}
