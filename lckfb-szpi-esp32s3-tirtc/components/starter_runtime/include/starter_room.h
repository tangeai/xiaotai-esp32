#ifndef STARTER_ROOM_H
#define STARTER_ROOM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STARTER_ROOM_MEMBERS_MAX 100U
#define STARTER_ROOM_PAGE_SIZE 3U

typedef enum {
    STARTER_ROOM_EMPTY = 0,
    STARTER_ROOM_SYNCING,
    STARTER_ROOM_CONNECTING,
    STARTER_ROOM_LISTENING,
    STARTER_ROOM_SPEAKING,
    STARTER_ROOM_SUSPENDED,
    STARTER_ROOM_ERROR,
    STARTER_ROOM_OFFLINE,
} starter_room_phase_t;

typedef struct {
    char participant_id[97];
    char device_id[65];
    bool speaking;
    bool self;
} starter_room_member_t;

/* Small paged copy, not a 100-member array on the LVGL task stack. */
typedef struct {
    uint32_t revision;
    uint32_t generation;
    starter_room_phase_t phase;
    bool assigned;
    bool assignment_known;
    bool app_open;
    bool busy;
    bool snapshot_ready;
    uint16_t member_count;
    uint16_t offset;
    uint8_t count;
    int error;
    char room_code[7];
    char self_name[65];
    starter_room_member_t members[STARTER_ROOM_PAGE_SIZE];
} starter_room_view_t;

/* All operations enqueue intent. Success is reported only by the room view. */
/* Foreground lifetime, separate from the persistent server assignment.
 * Closing immediately mutes PTT; the runtime owner disconnects Room, releases
 * its lease and stops Room polling. Shared MQTT/AI/call services stay alive.
 * Closed at boot. Reopening always re-reads assignment before connecting. */
void starter_runtime_room_set_open(bool open);
esp_err_t starter_runtime_room_create(const char *password);
esp_err_t starter_runtime_room_join(const char *room_code, const char *password);
esp_err_t starter_runtime_room_leave(void);
esp_err_t starter_runtime_room_refresh(void);
bool starter_runtime_room_read(uint16_t offset, starter_room_view_t *out);
/* Generation-scoped press. Release immediately closes the media gate, even
 * when the runtime queue is full; it never changes the user's mute setting. */
esp_err_t starter_runtime_room_ptt(uint32_t generation, bool pressed);

#ifdef __cplusplus
}
#endif

#endif
