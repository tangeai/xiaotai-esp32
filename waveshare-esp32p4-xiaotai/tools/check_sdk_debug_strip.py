"""Verify a debug-only GNU ar/ELF transformation without extracting members.

Requires pyelftools (included in the ESP-IDF Python environment). Duplicate
archive member names are intentionally compared by position, not by filename.
"""

import argparse
from collections import Counter
import hashlib
from io import BytesIO
import json
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


def read_archive(path):
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected a regular GNU archive")
    members = []
    names = b""
    symbol_index = None
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise ValueError(f"Invalid archive header at {offset}")
        name = header[:16].decode("ascii").strip()
        size = int(header[48:58])
        payload = data[offset + 60:offset + 60 + size]
        if len(payload) != size:
            raise ValueError("Truncated archive member")
        if name == "//":
            names = payload
        elif name == "/":
            symbol_index = payload
        else:
            if name.startswith("/"):
                start = int(name[1:])
                end = names.index(b"/\n", start)
                name = names[start:end].decode("utf-8")
            else:
                name = name.removesuffix("/")
            members.append((offset, name, payload))
        offset += 60 + size + size % 2
    if offset != len(data) or symbol_index is None:
        raise ValueError("Archive length/index invalid")

    # Archive offsets change after stripping; compare index targets by member.
    count = struct.unpack_from(">I", symbol_index)[0]
    offsets = struct.unpack_from(f">{count}I", symbol_index, 4)
    symbols = symbol_index[4 + 4 * count:].split(b"\0")
    if len(symbols) < count + 1 or symbols[count] != b"":
        raise ValueError("Truncated archive symbol index")
    symbols = symbols[:count]
    by_offset = {entry[0]: i for i, entry in enumerate(members)}
    index = [(symbol, by_offset[target]) for symbol, target in zip(symbols, offsets)]
    return data, members, index


def section_name(elf, index):
    return elf.get_section(index).name if isinstance(index, int) else index


def symbol_key(elf, symbol):
    item = symbol.entry
    return (symbol.name, item.st_value, item.st_size, item.st_info.type,
            item.st_info.bind, item.st_other.visibility,
            section_name(elf, item.st_shndx))


def linked_content(payload):
    elf = ELFFile(BytesIO(payload))
    sections = list(elf.iter_sections())
    allocated = [(s.name, s["sh_type"], s["sh_flags"], s["sh_addralign"],
                  s["sh_size"], s["sh_entsize"], s["sh_addr"], s.data())
                 for s in sections if s["sh_flags"] & 2]
    symbols = elf.get_section_by_name(".symtab")
    if symbols is None and any(s[4] for s in allocated):
        raise ValueError("Link symbols must not be stripped")
    kept_symbols = []
    for number, symbol in enumerate(symbols.iter_symbols() if symbols else []):
        if number == 0:
            continue
        index = symbol["st_shndx"]
        is_allocated = isinstance(index, int) and sections[index]["sh_flags"] & 2
        # Debug-only references can be the sole users of local section symbols.
        # Live relocation targets are compared separately below.
        debug_only_kind = symbol["st_info"]["type"] in ("STT_FILE", "STT_SECTION")
        if not debug_only_kind and (
                is_allocated or not isinstance(index, int) or
                symbol["st_info"]["bind"] in ("STB_GLOBAL", "STB_WEAK")):
            kept_symbols.append(symbol_key(elf, symbol))
    relocations = []
    for section in sections:
        if not isinstance(section, RelocationSection):
            continue
        target = sections[section["sh_info"]]
        if not target["sh_flags"] & 2:
            continue
        table = sections[section["sh_link"]]
        entries = [(r["r_offset"], r["r_info_type"],
                    r["r_addend"] if r.is_RELA() else None,
                    symbol_key(elf, table.get_symbol(r["r_info_sym"])))
                   for r in section.iter_relocations()]
        relocations.append((section.name, target.name, entries))
    attributes = [(s.name, s.data()) for s in sections
                  if s.name == ".riscv.attributes"]
    identity = (elf.elfclass, elf.little_endian, elf["e_machine"],
                elf["e_type"], elf["e_flags"])
    return identity, allocated, Counter(kept_symbols), relocations, attributes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("stripped", type=Path)
    args = parser.parse_args()
    old_data, original, old_index = read_archive(args.original)
    new_data, stripped, new_index = read_archive(args.stripped)
    if [m[1] for m in original] != [m[1] for m in stripped]:
        raise ValueError("Archive member order/names changed")
    if old_index != new_index:
        raise ValueError("Archive symbol lookup changed")
    alloc_count = 0
    relocation_count = 0
    for number, (old, new) in enumerate(zip(original, stripped)):
        before, after = linked_content(old[2]), linked_content(new[2])
        if before != after:
            raise ValueError(f"Linked content changed in member {number}: {old[1]}")
        alloc_count += len(before[1])
        relocation_count += sum(len(r[2]) for r in before[3])
        elf = ELFFile(BytesIO(new[2]))
        if any(s.name.startswith((".debug", ".zdebug")) for s in elf.iter_sections()):
            raise ValueError(f"Debug sections remain in {new[1]}")
    personal_path = rb"(?:/(?:home|Users)/[^/\s\x00]+/|[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/])"
    if re.search(personal_path, new_data):
        raise ValueError("Personal build paths remain after debug stripping")
    print(json.dumps({
        "result": "PASS",
        "members": len(original),
        "allocated_sections_compared": alloc_count,
        "relocations_compared": relocation_count,
        "archive_symbol_index_entries": len(old_index),
        "original_personal_path_occurrences": len(re.findall(personal_path, old_data)),
        "stripped_personal_path_occurrences": 0,
        "original_size": len(old_data),
        "stripped_size": len(new_data),
        "original_sha256": hashlib.sha256(old_data).hexdigest(),
        "stripped_sha256": hashlib.sha256(new_data).hexdigest(),
    }, indent=2))


if __name__ == "__main__":
    main()
