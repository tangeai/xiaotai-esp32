#include <assert.h>
#include <stdio.h>
#include "../components/wechat_voip/voip_protocol.h"

int main(void)
{
    cJSON *p = cJSON_Parse("{\"wx_from\":\"device\",\"wx_call_id\":\"call-1\","
                           "\"wx_user_openid\":\"person\",\"wx_app_id\":\"app\"}");
    assert(p);
    /* MQTT may beat the HTTP response; direction is already reserved. */
    assert(voip_route_room(p, "device", true, "person", "app", "") == ROOM_OUTBOUND);
    assert(voip_route_room(p, "device", true, "person", "app", "call-1") == ROOM_OUTBOUND);
    assert(voip_route_room(p, "device", true, "person", "app", "call-2") == ROOM_UNRELATED);
    assert(voip_route_room(p, "device", true, "other", "app", "") == ROOM_UNRELATED);
    assert(voip_route_room(p, "device", true, "person", "other", "") == ROOM_UNRELATED);
    assert(voip_route_room(p, "device", false, "", "", "") == ROOM_STALE_OUTBOUND);
    cJSON_ReplaceItemInObjectCaseSensitive(p, "wx_from", cJSON_CreateString("phone"));
    assert(voip_route_room(p, "device", false, "", "", "") == ROOM_INBOUND);
    assert(voip_route_room(p, "device", true, "person", "app", "") == ROOM_UNRELATED);
    cJSON_Delete(p);

    unsigned reason = 99;
    assert(voip_parse_hangup("{\"reason\":1}", &reason) && reason == 1);
    assert(voip_parse_hangup("{\"reason\":0}", &reason) && reason == 0);
    assert(!voip_parse_hangup("{}", &reason));
    assert(!voip_parse_hangup("{\"reason\":-1}", &reason));
    assert(!voip_parse_hangup("{\"reason\":21}", &reason));
    assert(!voip_parse_hangup("{\"reason\":1.5}", &reason));
    assert(!voip_parse_hangup("{\"reason\":\"1\"}", &reason));
    assert(!voip_parse_hangup("{\"reason\":true}", &reason));
    assert(!voip_parse_hangup("{\"reason\":1}trailing", &reason));
    puts("protocol tests passed");
}
