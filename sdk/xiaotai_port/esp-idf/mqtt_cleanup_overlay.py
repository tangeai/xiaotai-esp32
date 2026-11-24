# SPDX-License-Identifier: MIT
"""Dormant native cleanup transforms for the exactly locked ESP-MQTT source."""

from hashlib import sha256


CLIENT_SHA256 = "83e5a7065b8774dce83dd6b9be0163caed3a4fdbbd7de38b328e7e01911487dc"
MSG_SHA256 = "4f030f744316677de0d852968b5c6e8d99dfa9f3e8601c6800385779c05096cc"


def _replace_one(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError(f"non-unique native cleanup anchor: {anchor!r}")
    return source.replace(anchor, replacement, 1)


def _function(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f"non-unique native cleanup function: {signature!r}")
    start = source.index(signature)
    end = source.index(b"\n}\n", start) + 3
    return source[start:end]


def patch_client_cleanup(source):
    """Patch bytes already authenticated by a composing generator, after its other transforms."""
    marker = b"static void xt_native_mqtt_wipe_string("
    if marker in source:
        raise ValueError("native credential cleanup already installed")
    body = _function(source, b"void esp_mqtt_destroy_config(esp_mqtt_client_handle_t client)\n{")
    patched = body
    for field in (b"client_id", b"username", b"password"):
        pointer = b"client->mqtt_state.connection.information." + field
        call = b"    free(" + pointer + b");"
        patched = _replace_one(patched, call, b"    xt_native_mqtt_wipe_string(" + pointer + b");\n" + call)
    helper = b"""static void xt_native_mqtt_wipe_string(char *text)
{
    if (!text) return;
    size_t remaining = strlen(text) + 1U;
    volatile unsigned char *bytes = (volatile unsigned char *)text;
    while (remaining--) *bytes++ = 0;
}

"""
    source = _replace_one(source, body, helper + patched)
    config = _function(source, b"esp_err_t esp_mqtt_set_config(esp_mqtt_client_handle_t client, const esp_mqtt_client_config_t *config)\n{")
    # TX/RX allocation can fail before the later assignment of NO_MEM. The
    # success exit returns config_has_conflict independently of this variable.
    failure = _replace_one(config, b"    esp_err_t err = ESP_OK;", b"    esp_err_t err = ESP_ERR_NO_MEM;")
    source = _replace_one(source, config, failure)
    # IDF 5.5/6.x enables the event loop. A failed queue/loop allocation must
    # use the same partial-construction cleanup as every other init failure.
    return _replace_one(source,
                        b"    esp_event_loop_create(&no_task_loop, &client->config->event_loop_handle);",
                        b"    if (esp_event_loop_create(&no_task_loop, &client->config->event_loop_handle) != ESP_OK) {\n"
                        b"        goto _mqtt_init_failed;\n"
                        b"    }")


def patch_msg_cleanup(source):
    """Clear this codec's complete TX allocation before its existing free; leave all other heaps alone."""
    body = _function(source, b"void mqtt_msg_buffer_destroy(mqtt_connection_t *connection)\n{")
    if b"volatile unsigned char *bytes" in body:
        raise ValueError("native TX cleanup already installed")
    call = b"        free(connection->buffer);"
    patched = _replace_one(body, call, b"""        if (connection->buffer) {
            volatile unsigned char *bytes = (volatile unsigned char *)connection->buffer;
            for (size_t i = 0; i < connection->buffer_length; ++i) bytes[i] = 0;
        }
""" + call)
    return _replace_one(source, body, patched)


def _checked(source, digest, name):
    if sha256(source).hexdigest() != digest:
        raise ValueError(f"native {name} cleanup input SHA-256 mismatch")


def overlay_client_cleanup(source):
    """Validate original bytes, then apply the standalone client transform."""
    _checked(source, CLIENT_SHA256, "client")
    return patch_client_cleanup(source)


def overlay_msg_cleanup(source):
    """Validate original bytes, then apply the standalone codec transform."""
    _checked(source, MSG_SHA256, "codec")
    return patch_msg_cleanup(source)
