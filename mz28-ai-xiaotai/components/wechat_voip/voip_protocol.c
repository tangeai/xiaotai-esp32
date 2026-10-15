#include "voip_protocol.h"
#include <string.h>

static const char *field(const cJSON *json, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

room_route_t voip_route_room(const cJSON *payload, const char *device_id, bool dialing,
                             const char *openid, const char *app_id, const char *call_id)
{
    const char *from = field(payload, "wx_from");
    const char *received_id = field(payload, "wx_call_id");
    const char *received_openid = field(payload, "wx_user_openid");
    const char *received_app = field(payload, "wx_app_id");
    bool own = from[0] && strcmp(from, device_id) == 0;
    if (!dialing)
        return own ? ROOM_STALE_OUTBOUND : ROOM_INBOUND;
    bool matches = (!received_openid[0] || strcmp(received_openid, openid) == 0) &&
                   (!received_app[0] || strcmp(received_app, app_id) == 0);
    if (call_id[0])
        matches = matches && received_id[0] && strcmp(received_id, call_id) == 0;
    else if (received_id[0])
        matches = matches && own;
    return matches ? ROOM_OUTBOUND : ROOM_UNRELATED;
}

bool voip_parse_hangup(const char *json, unsigned *reason)
{
    if (!json || !reason)
        return false;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "reason");
    bool ok = cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 20 &&
              value->valuedouble == value->valueint;
    if (ok)
        *reason = (unsigned)value->valueint;
    cJSON_Delete(root);
    return ok;
}
