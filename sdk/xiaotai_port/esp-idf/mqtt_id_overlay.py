# SPDX-License-Identifier: MIT
"""Packet-ID reservation for the exact supported native MQTT codec and outbox."""

from hashlib import sha256
from pathlib import Path


HERE = Path(__file__).resolve().parent
PINS = {
    "msg_header": "c3dbd6b37f168662c995406af2f4119fe2cb869a376667cf11949b9fc3089009",
    "msg_source": "4f030f744316677de0d852968b5c6e8d99dfa9f3e8601c6800385779c05096cc",
    "outbox_source": "5e17d0b15f699e241a89b28301d17507003813100e2f61cc4d0906362d51b5e3",
    "outbox_header": "12c5e19841bd0a21e9445d9ffe480103d206255c37272ce43c26d5428783c34f",
}


def checked(data, kind):
    if sha256(data).hexdigest() != PINS[kind]:
        raise ValueError(f"native {kind} SHA-256 mismatch")
    return data


def replace_one(data, anchor, replacement):
    if data.count(anchor) != 1:
        raise ValueError(f"non-unique native anchor: {anchor!r}")
    return data.replace(anchor, replacement, 1)


def overlay_msg_header(data):
    checked(data, "msg_header")
    tail = b"    mqtt_connect_info_t information;\n"
    return replace_one(data, tail, tail +
                       b"    uint16_t (*message_id_available)(void *ctx, uint16_t first);\n"
                       b"    void *message_id_ctx;\n")


def overlay_msg_source(data):
    checked(data, "msg_source")
    start = data.index(b"static uint16_t append_message_id(")
    end = data.index(b"\n}\n", start) + 3
    return data[:start] + (HERE / "mqtt_codec_ids.inc").read_bytes() + data[end:]


def overlay_outbox_source(data):
    checked(data, "outbox_source")
    return data + b'\n#line 1 "mqtt_outbox_ids.inc"\n' + (HERE / "mqtt_outbox_ids.inc").read_bytes()


def overlay_outbox_header(data):
    checked(data, "outbox_header")
    anchor = b"void outbox_delete_all_items(outbox_handle_t outbox);\n"
    return replace_one(data, anchor, anchor + b"uint16_t xt_mqtt_outbox_available_id(void *ctx, uint16_t first);\n")
