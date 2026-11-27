#!/usr/bin/env python3
"""Data that rides behind an application image, in the same file.

    python3 scripts/image_trailer.py attach <application.bin> <data> [--record <image_trailer.json>]
    python3 scripts/image_trailer.py show <application.bin>

A chip that runs from external RAM copies all the code and constants an image
maps into that RAM when it starts. Data of several megabytes among the
constants takes as much from the heap, and stays there. So it is not linked
into the image: it follows it, on the next 64 KB boundary of the same file,
and the firmware reads what it needs of it from the partition it runs from.

The loader reads an image by its own headers and never looks past its end, so
what follows does not concern it. Whoever writes the application file writes
the data with it: `idf.py flash`, flash_args, the merged release image and the
partition's size check all see one file. Attaching again replaces what was
attached before.

The firmware finds the data the same way this script places it: from the
image's headers. What the data is, how long it is and whether it is whole is
for its own format to say.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

PAGE = 0x10000
HEADER = 24             # esp_image_header_t


def image_length(data):
    """The length of an ESP application image, from its own headers."""
    if len(data) < HEADER or data[0] != 0xE9:
        raise ValueError("Not an ESP application image")
    segments, hash_appended = data[1], data[23]
    at = HEADER
    for _ in range(segments):
        if at + 8 > len(data):
            raise ValueError("The application image ends inside its segment table")
        _, size = struct.unpack_from("<II", data, at)
        at += 8 + size
    at = (at + 16) & ~15            # padding up to a checksum in the last byte of sixteen
    if hash_appended:
        at += 32
    if at > len(data):
        raise ValueError("The application image is shorter than its headers say")
    return at


def trailer_offset(length):
    return (length + PAGE - 1) // PAGE * PAGE


def attached(image, data):
    """The image with the data behind it, whatever was behind it before."""
    if not data:
        raise ValueError("Nothing to attach")
    length = image_length(image)
    return image[:length] + b"\xff" * (trailer_offset(length) - length) + data


def describe(image, source):
    """What a build record says about the data behind an image."""
    length = image_length(image)
    offset = trailer_offset(length)
    data = image[offset:]
    return {"source": source, "image_bytes": length, "offset": hex(offset), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def verify(image, record):
    """An image carries exactly the data its record describes; without a record, none."""
    length = image_length(image)
    if record is None:
        if len(image) != length:
            raise ValueError(f"The application file has {len(image) - length} unrecorded bytes behind its image")
        return
    offset = trailer_offset(length)
    if set(image[length:offset]) - {0xFF}:
        raise ValueError("The gap behind the application image is not blank")
    if describe(image, record.get("source")) != record or not record["bytes"]:
        raise ValueError("The data behind the application image does not match its record")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    attach = commands.add_parser("attach")
    attach.add_argument("image", type=Path)
    attach.add_argument("data", type=Path)
    attach.add_argument("--record", type=Path)
    show = commands.add_parser("show")
    show.add_argument("image", type=Path)
    args = parser.parse_args(argv)
    try:
        image = args.image.read_bytes()
        if args.command == "show":
            print(json.dumps(describe(image, None), indent=2))
            return
        out = attached(image, args.data.read_bytes())
        if out != image:
            args.image.write_bytes(out)
        record = describe(out, args.data.name)
        if args.record:
            args.record.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError) as error:
        parser.exit(1, f"ERROR: {error}\n")
    print(f"{args.image.name}: image {record['image_bytes']} bytes, {record['source']} {record['bytes']} bytes "
          f"at {record['offset']}, {len(out)} bytes in all")


if __name__ == "__main__":
    main()
