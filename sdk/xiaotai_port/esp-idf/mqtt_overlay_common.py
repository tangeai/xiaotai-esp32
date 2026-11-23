# SPDX-License-Identifier: MIT
"""Exact-input checks and function boundaries shared by the native MQTT adapter."""

from hashlib import sha256


CLIENT_SHA256 = "83e5a7065b8774dce83dd6b9be0163caed3a4fdbbd7de38b328e7e01911487dc"
PRIVATE_HEADER_SHA256 = "dc613a10765ad9d68648065400fb2c52bb081cca1951c64f7e6b7df574971c49"


def native_function(source, start):
    if source.count(start) != 1:
        raise ValueError(f"native function boundary changed: {start}")
    begin = source.index(start)
    end = source.index("\n}\n", begin) + len("\n}\n")
    return source[begin:end]


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"native transaction boundary changed: {before}")
    return source.replace(before, after)


def locked_bytes(path, digest):
    data = path.read_bytes()
    if sha256(data).hexdigest() != digest:
        raise ValueError(f"{path}: SHA-256 differs from locked esp-mqtt 1.1.0")
    return data
