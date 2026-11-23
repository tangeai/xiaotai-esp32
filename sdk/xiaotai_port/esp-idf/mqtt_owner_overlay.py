# SPDX-License-Identifier: MIT
"""Per-client owner bridge hooks for the exact locally pinned MQTT source."""

from mqtt_overlay_common import native_function, replace_once, CLIENT_SHA256, PRIVATE_HEADER_SHA256
from hashlib import sha256


def checked(data, digest):
    if sha256(data).hexdigest() != digest:
        raise ValueError("unreviewed MQTT source for owner integration")
    return data.decode("utf-8")


def owner_header(data):
    source = checked(data, PRIVATE_HEADER_SHA256)
    return replace_once(source, "    bool wait_for_ping_resp;\n",
                        "    bool wait_for_ping_resp;\n    void *xiaotai_owner_bridge;\n").encode("utf-8")


def owner_source(data):
    source = checked(data, CLIENT_SHA256)
    source = replace_once(source, '#include "mqtt_client_priv.h"\n',
                          '#include "mqtt_client_priv.h"\n#define XIAOTAI_MQTT_OWNER_BRIDGE 1\n'
                          '#include "mqtt_owner_bridge.h"\n')
    # Only a selected client's path bypasses the single-slot native event loop.
    anchor = "static esp_err_t esp_mqtt_dispatch_event(esp_mqtt_client_handle_t client)\n{\n"
    source = replace_once(source, anchor, anchor +
                          "    if (xt_mqtt_owner_bridge(client)) return xt_mqtt_owner_dispatch(client);\n")
    source = replace_once(source, "    xEventGroupClearBits(client->status_bits, STOPPED_BIT);\n",
                          "    xEventGroupClearBits(client->status_bits, STOPPED_BIT);\n"
                          "    xt_mqtt_owner_begin(client);\n")
    source = replace_once(source, "        run_event_loop(client);\n",
                          "        if (xt_mqtt_owner_bridge(client)) xt_mqtt_owner_safe_point(client);\n"
                          "        else run_event_loop(client);\n")
    source = replace_once(source,
                          "            xEventGroupWaitBits(client->status_bits, RECONNECT_BIT, false, true,\n",
                          "            xEventGroupWaitBits(client->status_bits, RECONNECT_BIT |\n"
                          "                (xt_mqtt_owner_bridge(client) ? XT_MQTT_OWNER_WORK : 0), false, false,\n")
    source = replace_once(source, "    xEventGroupSetBits(client->status_bits, STOPPED_BIT);\n",
                          "    xt_mqtt_owner_end(client);\n    xEventGroupSetBits(client->status_bits, STOPPED_BIT);\n")
    source = replace_once(source,
                          "        if (MQTT_STATE_CONNECTED == client->state) {\n"
                          "            if (esp_transport_poll_read",
                          "        if (MQTT_STATE_CONNECTED == client->state &&\n"
                          "            !(xt_mqtt_owner_bridge(client) &&\n"
                          "              (xEventGroupGetBits(client->status_bits) & XT_MQTT_OWNER_WORK))) {\n"
                          "            if (esp_transport_poll_read")
    # Each of these native entry points first holds the recursive lock once.
    for name in ("int esp_mqtt_client_publish(", "int esp_mqtt_client_subscribe_multiple(",
                 "esp_err_t esp_mqtt_client_stop("):
        body = native_function(source, name)
        changed = replace_once(body, "    MQTT_API_LOCK(client);\n",
                               "    if (!xt_mqtt_owner_sync_lock(client)) return -1;\n")
        if name != "esp_err_t esp_mqtt_client_stop(":
            changed = replace_once(changed, "    if (!xt_mqtt_owner_sync_lock(client)) return -1;\n",
                                   "    if (!xt_mqtt_owner_sync_lock(client)) return -1;\n"
                                   "    if (xt_mqtt_owner_bridge(client) && client->state != MQTT_STATE_CONNECTED) {\n"
                                   "        MQTT_API_UNLOCK(client);\n        return -1;\n    }\n")
        source = replace_once(source, body, changed)
    return (source + '\n#include "mqtt_owner_api.inc"\n').encode("utf-8")
