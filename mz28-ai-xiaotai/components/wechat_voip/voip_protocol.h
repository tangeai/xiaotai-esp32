#pragma once
#include <stdbool.h>
#include "cJSON.h"

typedef enum { ROOM_INBOUND, ROOM_OUTBOUND, ROOM_STALE_OUTBOUND, ROOM_UNRELATED } room_route_t;
room_route_t voip_route_room(const cJSON *payload, const char *device_id, bool dialing,
                             const char *openid, const char *app_id, const char *call_id);
bool voip_parse_hangup(const char *json, unsigned *reason);
