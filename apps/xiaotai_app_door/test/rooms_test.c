// SPDX-License-Identifier: MIT
#include "door_rooms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

static bool is(const char *name, const char *expected)
{
    char room[DOOR_ROOM_SIZE];
    const bool found = door_room_of(name, room);
    if (strcmp(room, expected) || found != (expected[0] != '\0')) fprintf(stderr, "%s -> %s\n", name, room);
    return !strcmp(room, expected) && found == (expected[0] != '\0');
}

int main(void)
{
    CHECK(is("301", "301") && is("301 王先生", "301") && is("王先生 301室", "301"));
    CHECK(is("3栋2单元301 王先生", "3-2-301") && is("1-301", "1-301") && is("03栋02单元0301", "3-2-301"));
    CHECK(is("３栋１２０１", "3-1201"));                         /* full width */
    CHECK(is("301 王先生 13800000000", "301"));                  /* a telephone number is no group */
    CHECK(is("13800000000", "") && is("王先生", "") && is("", "") && is(NULL, "") && is("A座", ""));
    CHECK(is("0", "0") && is("000", "0") && is("A301", "301"));
    CHECK(is("1-2-3-4-5", "2-3-4-5"));                           /* the last groups */
    CHECK(is("1234567", "") && is("123456", "123456"));
    CHECK(is("\xef\xbc", "") && is("\xef", ""));                 /* cut in the middle of a character */

    char plate[64];
    door_room_plate("3栋2单元0301 王先生", plate, sizeof(plate));
    CHECK(!strcmp(plate, "3栋2单元0301"));
    door_room_plate("301 王先生 13800000000", plate, sizeof(plate));
    CHECK(!strcmp(plate, "301"));                                   /* the telephone number is no door */
    door_room_plate("王先生", plate, sizeof(plate));
    CHECK(!plate[0]);
    door_room_plate("4栋301", plate, 4);
    CHECK(!strcmp(plate, "4"));                                     /* cut to fit, between characters */

    CHECK(door_room_matches("301", "301") && door_room_matches("301", "0301") && !door_room_matches("301", "30"));
    CHECK(door_room_matches("3-2-301", "301") && door_room_matches("3-2-301", "32301"));
    CHECK(door_room_matches("3-2-301", "302301") && door_room_matches("3-2-301", "03020301"));
    CHECK(!door_room_matches("3-2-301", "2301") && !door_room_matches("3-2-301", "3230") && !door_room_matches("3-2-301", "323010"));
    CHECK(door_room_matches("1-301", "1301") == 2 && door_room_matches("10-301", "10301") == 2 && door_room_matches("1-301", "10301") == 1);
    CHECK(door_room_matches("301", "301") == 2 && door_room_matches("3-2-301", "0301") == 2 && door_room_matches("3-2-301", "032301") == 2);
    CHECK(door_room_matches("0", "0") && door_room_matches("0", "00") && !door_room_matches("301", "") && !door_room_matches("", "0"));
    CHECK(!door_room_matches("3-301", "1301") && !door_room_matches("301", "3011"));

    CHECK(door_room_compare("301", "301") == 0 && door_room_compare("301", "1201") < 0 && door_room_compare("2-101", "1-901") > 0);
    CHECK(door_room_compare("301", "301-2") < 0 && door_room_compare("3-2-301", "3-10-101") < 0);
    return 0;
}
