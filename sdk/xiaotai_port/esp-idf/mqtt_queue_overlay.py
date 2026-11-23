# SPDX-License-Identifier: MIT
"""Hash-pinned native single-outbox short transactions; never edits the vendor tree."""

from hashlib import sha256
from pathlib import Path
import re

from mqtt_overlay_common import CLIENT_SHA256, native_function, replace_once
from mqtt_id_overlay import PINS

HERE = Path(__file__).resolve().parent


def checked(original, digest, transformed):
    if sha256(original).hexdigest() != digest:
        raise ValueError("native MQTT queue SHA-256 mismatch")
    return (original if transformed is None else transformed).decode("utf-8")


def patch_outbox_queue_header(original, transformed=None):
    source = checked(original, PINS["outbox_header"], transformed)
    declarations = """
/* Failure phases are readable without logging while a short transaction is held. */
enum { XT_MQTT_QUEUE_ALLOC, XT_MQTT_QUEUE_QUOTA, XT_MQTT_QUEUE_IDS, XT_MQTT_QUEUE_CODEC };
void xt_mqtt_outbox_lock(outbox_handle_t outbox);
void xt_mqtt_outbox_unlock(outbox_handle_t outbox);
void xt_mqtt_outbox_note_failure(outbox_handle_t outbox, unsigned phase);
void xt_mqtt_outbox_failures(outbox_handle_t outbox, uint32_t counts[4]);
outbox_item_handle_t xt_mqtt_outbox_adopt(outbox_handle_t outbox, uint8_t *buffer,
                                        int length, int id, int type, int qos, outbox_tick_t tick);
"""
    anchor = "void outbox_delete_all_items(outbox_handle_t outbox);\n"
    return replace_once(source, anchor, anchor + declarations).encode("utf-8")


def patch_outbox_queue(original, transformed=None):
    source = checked(original, PINS["outbox_source"], transformed)
    source = replace_once(source, '#include "mqtt_outbox.h"\n',
                          '#include "mqtt_outbox.h"\n'
                          '#include "freertos/FreeRTOS.h"\n#include "freertos/semphr.h"\n')
    source = replace_once(source, "    struct outbox_list_t *list;\n",
                          "    struct outbox_list_t *list;\n    SemaphoreHandle_t admission;\n"
                          "    uint32_t failures[4];\n")
    source = replace_once(source, "    STAILQ_INIT(outbox->list);\n",
                          "    outbox->admission = xSemaphoreCreateRecursiveMutex();\n"
                          "    if (!outbox->admission) { free(outbox->list); free(outbox); return NULL; }\n"
                          "    STAILQ_INIT(outbox->list);\n")
    source = replace_once(source, "    free(outbox->list);\n    free(outbox);\n",
                          "    vSemaphoreDelete(outbox->admission);\n    free(outbox->list);\n    free(outbox);\n")
    # The native DEBUG calls would run under the outer transaction's recursive
    # lock even after an inner operation unlocks. Report failures as counters.
    source = re.sub(r"    +ESP_LOGD\(TAG,.*?;\n", "", source, flags=re.DOTALL)
    source = replace_once(source, "    ESP_MEM_CHECK(TAG, item, return NULL);\n",
                          "    if (!item) { xt_mqtt_outbox_note_failure(outbox, XT_MQTT_QUEUE_ALLOC); return NULL; }\n")
    source = replace_once(source, "    ESP_MEM_CHECK(TAG, item->buffer, {\n        free(item);\n        return NULL;\n    });\n",
                          "    if (!item->buffer) {\n        free(item);\n"
                          "        xt_mqtt_outbox_note_failure(outbox, XT_MQTT_QUEUE_ALLOC);\n        return NULL;\n    }\n")
    operations = {
        "outbox_enqueue": ("outbox_item_handle_t", "outbox_handle_t outbox, outbox_message_handle_t message, outbox_tick_t tick", "outbox, message, tick"),
        "outbox_get": ("outbox_item_handle_t", "outbox_handle_t outbox, int msg_id", "outbox, msg_id"),
        "outbox_dequeue": ("outbox_item_handle_t", "outbox_handle_t outbox, pending_state_t pending, outbox_tick_t *tick", "outbox, pending, tick"),
        "outbox_delete_item": ("esp_err_t", "outbox_handle_t outbox, outbox_item_handle_t item_to_delete", "outbox, item_to_delete"),
        "outbox_delete": ("esp_err_t", "outbox_handle_t outbox, int msg_id, int msg_type", "outbox, msg_id, msg_type"),
        "outbox_set_pending": ("esp_err_t", "outbox_handle_t outbox, int msg_id, pending_state_t pending", "outbox, msg_id, pending"),
        "outbox_set_tick": ("esp_err_t", "outbox_handle_t outbox, int msg_id, outbox_tick_t tick", "outbox, msg_id, tick"),
        "outbox_delete_single_expired": ("int", "outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout", "outbox, current_tick, timeout"),
        "outbox_delete_expired": ("int", "outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout", "outbox, current_tick, timeout"),
        "outbox_delete_all_items": ("void", "outbox_handle_t outbox", "outbox"),
    }
    for name, (kind, params, args) in operations.items():
        old = native_function(source, kind + " " + name + "(")
        renamed = replace_once(old, kind + " " + name + "(", "static " + kind + " xt_native_" + name + "(")
        invoke = ("    xt_native_" + name + "(" + args + ");\n") if kind == "void" else (
            "    " + kind + " result = xt_native_" + name + "(" + args + ");\n")
        wrapper = "\n" + kind + " " + name + "(" + params + ")\n{\n    xt_mqtt_outbox_lock(outbox);\n" + invoke + (
            "    xt_mqtt_outbox_unlock(outbox);\n" + ("" if kind == "void" else "    return result;\n") + "}\n")
        source = replace_once(source, old, renamed + wrapper)
    anchor = "outbox_handle_t outbox_init(void)\n"
    source = replace_once(source, anchor, '#include "mqtt_outbox_queue.inc"\n\n' + anchor)
    return source.encode("utf-8")


def patch_client_queue(original, transformed=None):
    source = checked(original, CLIENT_SHA256, transformed)
    source = replace_once(source, '#include "mqtt_outbox.h"\n',
                          '#include "mqtt_outbox.h"\n#define XIAOTAI_MQTT_NATIVE_QUEUE 1\n'
                          '#include "mqtt_queue_client.inc"\n')
    # Preserve the same atomic native state, but serialize its publication with
    # admission. Invalidation precedes transport close and callbacks.
    source, count = re.subn(r"client->state = (MQTT_STATE_[A-Z_]+);",
                           r"xt_mqtt_queue_state(client, \1);", source)
    if count != 8:
        raise ValueError(f"native MQTT state assignment count changed: {count}")
    abort = native_function(source, "static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t client)\n{")
    changed = replace_once(abort, "    xt_mqtt_queue_state(client, MQTT_STATE_WAIT_RECONNECT);\n", "")
    changed = replace_once(changed, "    esp_transport_close(client->transport);\n",
                           "    xt_mqtt_queue_state(client, MQTT_STATE_WAIT_RECONNECT);\n"
                           "    esp_transport_close(client->transport);\n")
    source = replace_once(source, abort, changed)
    cleanup = "    esp_transport_close(client->transport);\n    outbox_delete_all_items(client->outbox);\n    xt_mqtt_queue_state(client, MQTT_STATE_DISCONNECTED);\n"
    source = replace_once(source, cleanup,
                          "    xt_mqtt_queue_state(client, MQTT_STATE_DISCONNECTED);\n"
                          "    esp_transport_close(client->transport);\n    outbox_delete_all_items(client->outbox);\n")
    stop = native_function(source, "esp_err_t esp_mqtt_client_stop(")
    changed = replace_once(stop, "        xt_mqtt_queue_state(client, MQTT_STATE_DISCONNECTED);\n", "")
    changed = replace_once(changed, "        if (client->state == MQTT_STATE_CONNECTED) {\n",
                           "        const bool xt_was_connected = client->state == MQTT_STATE_CONNECTED;\n"
                           "        xt_mqtt_queue_state(client, MQTT_STATE_DISCONNECTED);\n"
                           "        if (xt_was_connected) {\n")
    source = replace_once(source, stop, changed)
    source = replace_once(source, "        ESP_LOGE(TAG, \"Publish message cannot be created\");\n", "")
    source = replace_once(source, '    ESP_LOGD(TAG, "mqtt_enqueue id: %d, type=%d successful",\n'
                          '             client->mqtt_state.pending_msg_id, client->mqtt_state.pending_msg_type);\n', "")
    subscribe = native_function(source, "int esp_mqtt_client_subscribe_multiple(")
    changed = replace_once(subscribe, "    // Reset pending state to avoid inheriting previous PUBLISH QoS or type\n",
                           "    xt_mqtt_outbox_lock(client->outbox);\n"
                           "    if (client->state != MQTT_STATE_CONNECTED) {\n"
                           "        xt_mqtt_outbox_unlock(client->outbox);\n        MQTT_API_UNLOCK(client);\n        return -1;\n    }\n"
                           "    // Reset pending state to avoid inheriting previous PUBLISH QoS or type\n")
    # Only the port's MQTT 3.1.1 path is selected. Avoid logging or MQTT5
    # callbacks under the short lock even if a caller uses the native API.
    begin = changed.index("    if (client->mqtt_state.connection.information.protocol_ver == MQTT_PROTOCOL_V_5) {")
    end = changed.index("    if (client->mqtt_state.connection.outbound_message.length == 0) {", begin)
    changed = changed[:begin] + """    if (client->mqtt_state.connection.information.protocol_ver != MQTT_PROTOCOL_V_3_1_1) {
        xt_mqtt_outbox_unlock(client->outbox);
        MQTT_API_UNLOCK(client);
        return -1;
    }
    mqtt_msg_subscribe(&client->mqtt_state.connection, topic_list, size,
                       &client->mqtt_state.pending_msg_id);

""" + changed[end:]
    changed = replace_once(changed, '        ESP_LOGE(TAG, "Subscribe message cannot be created");\n',
                           "        xt_mqtt_outbox_note_failure(client->outbox, XT_MQTT_QUEUE_CODEC);\n"
                           "        xt_mqtt_outbox_unlock(client->outbox);\n")
    changed = replace_once(changed, "    if (!mqtt_enqueue(client, NULL, 0)) {\n",
                           "    if (!mqtt_enqueue(client, NULL, 0)) {\n        xt_mqtt_outbox_unlock(client->outbox);\n")
    changed = replace_once(changed, "    client->mqtt_state.pending_msg_type = mqtt_get_type(client->mqtt_state.connection.outbound_message.data);\n",
                           "    if (!xt_mqtt_queue_fits(client, client->mqtt_state.connection.outbound_message.length)) {\n"
                           "        xt_mqtt_outbox_unlock(client->outbox);\n        MQTT_API_UNLOCK(client);\n        return -2;\n    }\n"
                           "    client->mqtt_state.pending_msg_type = mqtt_get_type(client->mqtt_state.connection.outbound_message.data);\n")
    anchor = "    outbox_set_pending(client->outbox, client->mqtt_state.pending_msg_id, TRANSMITTED);// handle error\n"
    changed = replace_once(changed, anchor, anchor +
                           "    outbox_item_handle_t xt_accepted = outbox_get(client->outbox, client->mqtt_state.pending_msg_id);\n"
                           "    xt_mqtt_outbox_unlock(client->outbox);\n")
    changed = replace_once(changed, "    if (esp_mqtt_write(client) != ESP_OK) {\n",
                           "    if (esp_mqtt_write(client) != ESP_OK) {\n"
                           "        if (xt_accepted) outbox_delete_item(client->outbox, xt_accepted);\n")
    source = replace_once(source, subscribe, changed)
    publish = native_function(source, "int esp_mqtt_client_publish(")
    before = """    if (client->config->outbox_limit > 0 && qos > 0) {
        if (len + outbox_get_size(client->outbox) > client->config->outbox_limit) {
            MQTT_API_UNLOCK(client);
            return -2;
        }
    }

    int pending_msg_id = mqtt_client_enqueue_publish(client, topic, data, len, qos, retain, false);

    if (pending_msg_id < 0) {
        MQTT_API_UNLOCK(client);
        return -1;
    }
"""
    after = """    size_t xt_wire;
    if (!xt_mqtt_queue_wire_size(topic, len, qos, &xt_wire)) {
        MQTT_API_UNLOCK(client);
        return -1;
    }
    xt_mqtt_outbox_lock(client->outbox);
    if (client->state != MQTT_STATE_CONNECTED) {
        xt_mqtt_outbox_unlock(client->outbox);
        MQTT_API_UNLOCK(client);
        return -1;
    }
    if (qos && !xt_mqtt_queue_fits(client, xt_wire)) {
        xt_mqtt_outbox_unlock(client->outbox);
        MQTT_API_UNLOCK(client);
        return -2;
    }
    int pending_msg_id = mqtt_client_enqueue_publish(client, topic, data, len, qos, retain, false);
    outbox_item_handle_t xt_accepted = pending_msg_id > 0 ? outbox_get(client->outbox, pending_msg_id) : NULL;
    if (pending_msg_id < 0) xt_mqtt_outbox_note_failure(client->outbox, XT_MQTT_QUEUE_CODEC);
    xt_mqtt_outbox_unlock(client->outbox);
    if (pending_msg_id < 0) {
        MQTT_API_UNLOCK(client);
        return -1;
    }
"""
    changed = replace_once(publish, before, after)
    changed = replace_once(changed, "        if (esp_mqtt_write(client) != ESP_OK) {\n",
                           "        if (esp_mqtt_write(client) != ESP_OK) {\n"
                           "            if (xt_accepted) { outbox_delete_item(client->outbox, xt_accepted); xt_accepted = NULL; }\n")
    source = replace_once(source, publish, changed)
    return source.encode("utf-8")


def queue_publish_transaction(source):
    """Extract only after queue and owner transforms; never clone the raw path."""
    if isinstance(source, bytes):
        source = source.decode("utf-8")
    body = native_function(source, "int esp_mqtt_client_publish(")
    if "xt_mqtt_outbox_lock(client->outbox)" not in body or "xt_accepted" not in body:
        raise ValueError("publish transaction lacks the verified queue boundary")
    return replace_once(body, "int esp_mqtt_client_publish(", "static int xt_mqtt_publish_transaction(").encode("utf-8")
