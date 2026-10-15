"""Length-prefixed Opus packets and Ogg export (RFC 6716 / RFC 7845).

Storage format: repeated uint16 little-endian size + one raw Opus packet.
The length prefix and Ogg headers are storage-only and never sent to TiRTC.
"""
import struct

MAX_PACKET = 1275


def packet_samples(packet):
    """Read duration on the 48 kHz Opus clock; decoding validates the bitstream."""
    if not packet or len(packet) > MAX_PACKET:
        raise ValueError("Invalid Opus packet size")
    toc = packet[0]
    if toc & 128:
        samples = 120 << ((toc >> 3) & 3)
    elif toc & 96 == 96:
        samples = 960 if toc & 8 else 480
    else:
        index = (toc >> 3) & 3
        samples = 2880 if index == 3 else 480 << index
    code = toc & 3
    if code == 3 and len(packet) < 2:
        raise ValueError("Truncated Opus frame count")
    count = 1 if code == 0 else (packet[1] & 63 if code == 3 else 2)
    if not count or samples * count > 5760:
        raise ValueError("Invalid Opus packet duration")
    if toc & 4:
        raise ValueError("Expected mono Opus")
    return samples * count


def unpack_packets(data):
    packets = []
    offset = 0
    while offset < len(data):
        if offset + 2 > len(data):
            raise ValueError("Truncated packet length")
        size, = struct.unpack_from("<H", data, offset)
        offset += 2
        if not 0 < size <= MAX_PACKET or offset + size > len(data):
            raise ValueError("Invalid or truncated packet payload")
        packet = data[offset:offset + size]
        packet_samples(packet)
        packets.append(packet)
        offset += size
    if not packets:
        raise ValueError("Empty Opus recording")
    return packets


def pack_packets(packets):
    return b"".join(struct.pack("<H", len(packet)) + packet for packet in packets)


def _crc(data):
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc


def _page(packet, sequence, granule, flags=0):
    lacing = bytes([255] * (len(packet) // 255) + [len(packet) % 255])
    page = bytearray(struct.pack("<4sBBQIIIB", b"OggS", 0, flags, granule,
                                 1, sequence, 0, len(lacing)) + lacing + packet)
    struct.pack_into("<I", page, 22, _crc(page))
    return bytes(page)


def to_ogg(packets, rate=16000):
    if not packets or rate != 16000:
        raise ValueError("Expected nonempty 16 kHz mono recording")
    # A capture starts midstream: no known encoder pre-skip; never invent one.
    head = struct.pack("<8sBBHIhB", b"OpusHead", 1, 1, 0, rate, 0, 0)
    vendor = b"TiRTC C3 capture"
    tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
    pages = [_page(head, 0, 0, 2), _page(tags, 1, 0)]
    granule = 0
    for index, packet in enumerate(packets):
        granule += packet_samples(packet)
        pages.append(_page(packet, index + 2, granule, 4 if index == len(packets) - 1 else 0))
    return b"".join(pages)
