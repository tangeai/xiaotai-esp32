// SPDX-License-Identifier: MIT
#pragma once
/* Room numbers in the names of contacts. Pure text; nothing else is known here.
 *
 * A room is the groups of digits of a name, in order, without leading zeros,
 * joined by '-': "3栋2单元0301 王先生" is "3-2-301", "1-301" is "1-301", "301" is
 * "301". Full-width digits count as digits. A longer run of digits than a room
 * has (a telephone number) is no group. A name without one has no room. */
#include <stdbool.h>
#include <stddef.h>

#define DOOR_ROOM_GROUPS 4U         /* the last ones of a name that has more */
#define DOOR_ROOM_DIGITS 6U
#define DOOR_ROOM_SIZE (DOOR_ROOM_GROUPS * (DOOR_ROOM_DIGITS + 1U))
/* What can be typed: every group of the longest room. */
#define DOOR_ROOM_TYPED_MAX 10U

bool door_room_of(const char *name, char room[DOOR_ROOM_SIZE]);
/* The door plate as the name writes it: the name up to the last digit of its
 * room, so "3栋2单元0301 王先生" is "3栋2单元0301" ("" without a room). */
void door_room_plate(const char *name, char *out, size_t size);
/* Whether digits typed at the door mean this room: 2 the last group, or every
 * group one after the other; 1 the same with zeros in front of a group; 0 not.
 * Where one room is meant without such zeros, one that needs them is not. */
int door_room_matches(const char *room, const char *typed);
/* Ascending by number, group by group; a room with fewer groups comes first. */
int door_room_compare(const char *a, const char *b);
