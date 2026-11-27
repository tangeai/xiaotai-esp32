// SPDX-License-Identifier: MIT
#include "door_rooms.h"

#include <stdlib.h>
#include <string.h>

/* The digit at p, and how many bytes it takes; -1: none. */
static int digit(const char *p, size_t *bytes)
{
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] >= '0' && u[0] <= '9') {
        *bytes = 1;
        return u[0] - '0';
    }
    /* U+FF10..U+FF19 */
    if (u[0] == 0xefU && u[1] == 0xbcU && u[2] >= 0x90U && u[2] <= 0x99U) {
        *bytes = 3;
        return u[2] - 0x90;
    }
    return -1;
}

bool door_room_of(const char *name, char room[DOOR_ROOM_SIZE])
{
    char groups[DOOR_ROOM_GROUPS][DOOR_ROOM_DIGITS + 1U];
    size_t count = 0;
    room[0] = '\0';
    for (const char *p = name; p && *p;) {
        char group[DOOR_ROOM_DIGITS + 1U];
        size_t bytes, length = 0, run = 0;
        for (int d; (d = digit(p, &bytes)) >= 0; p += bytes, ++run) {
            if (!d && !length) continue;        /* a zero in front */
            if (length < DOOR_ROOM_DIGITS) group[length] = (char)('0' + d);
            ++length;
        }
        if (!run) {
            ++p;
            continue;
        }
        if (length > DOOR_ROOM_DIGITS || run > DOOR_ROOM_DIGITS) continue;
        if (!length) group[length++] = '0';
        group[length] = '\0';
        if (count == DOOR_ROOM_GROUPS) {
            memmove(groups[0], groups[1], (DOOR_ROOM_GROUPS - 1U) * sizeof(groups[0]));
            --count;
        }
        memcpy(groups[count++], group, sizeof(group));
    }
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i) room[used++] = '-';
        const size_t length = strlen(groups[i]);
        memcpy(room + used, groups[i], length);
        used += length;
    }
    room[used] = '\0';
    return count != 0;
}

void door_room_plate(const char *name, char *out, size_t size)
{
    size_t end = 0;
    for (const char *p = name; p && *p;) {
        size_t bytes, run = 0;
        while (digit(p, &bytes) >= 0) {
            p += bytes;
            ++run;
        }
        if (!run) ++p;      /* a byte of something else: never the start of a digit */
        else if (run <= DOOR_ROOM_DIGITS) end = (size_t)(p - name);
    }
    if (!size) return;
    if (end >= size) {
        end = size - 1U;
        while (end && ((unsigned char)name[end] & 0xc0U) == 0x80U) --end;      /* whole characters only */
    }
    memcpy(out, name ? name : "", end);
    out[end] = '\0';
}

static const char *skip_zeros(const char *typed)
{
    while (typed[0] == '0' && typed[1]) ++typed;
    return typed;
}

int door_room_matches(const char *room, const char *typed)
{
    if (!room[0] || !typed[0]) return 0;
    const char *whole = skip_zeros(typed);
    int rank = 2;
    const char *last = strrchr(room, '-');
    last = last ? last + 1 : room;
    if (!strcmp(last, whole)) return 2;
    typed = whole;
    for (const char *group = room; *group;) {
        const size_t length = strcspn(group, "-");
        for (; *typed == '0' && *group != '0'; ++typed) rank = 1;
        if (strncmp(typed, group, length)) return 0;
        typed += length;
        group += length;
        if (*group) ++group;
    }
    return *typed ? 0 : rank;
}

int door_room_compare(const char *a, const char *b)
{
    while (*a && *b) {
        const long x = strtol(a, NULL, 10), y = strtol(b, NULL, 10);
        if (x != y) return x < y ? -1 : 1;
        a += strcspn(a, "-");
        b += strcspn(b, "-");
        if (*a) ++a;
        if (*b) ++b;
    }
    return (*a != '\0') - (*b != '\0');
}
