"""Compare embedded framing checks to the official libopus parser on the host."""
import ctypes
import ctypes.util
from pathlib import Path
import random
import unittest


class OpusFramingTests(unittest.TestCase):
    def test_against_libopus(self):
        path = Path(__file__).parents[1] / "build-host/libpacket_test.so"
        library = ctypes.util.find_library("opus")
        if not path.exists() or not library:
            self.skipTest("Run tools/test_host.sh with host libopus installed")
        ours = ctypes.CDLL(str(path))
        opus = ctypes.CDLL(library)
        ours.opus_packet_samples.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        ours.opus_packet_samples.restype = ctypes.c_uint
        opus.opus_packet_parse.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        opus.opus_packet_get_nb_samples.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        randomizer = random.Random(6716)
        cases = [bytes([toc]) for toc in range(256)]
        cases += [bytes(randomizer.randrange(256) for _ in range(randomizer.randrange(1, 128)))
                  for _ in range(10000)]
        for packet in cases:
            buffer = ctypes.create_string_buffer(packet)
            toc = ctypes.c_ubyte()
            frames = (ctypes.c_void_p * 48)()
            sizes = (ctypes.c_short * 48)()
            offset = ctypes.c_int()
            result = opus.opus_packet_parse(buffer, len(packet), ctypes.byref(toc),
                                            frames, sizes, ctypes.byref(offset))
            expected = opus.opus_packet_get_nb_samples(buffer, len(packet), 48000) if result > 0 else 0
            self.assertEqual(ours.opus_packet_samples(buffer, len(packet)), max(0, expected),
                             packet.hex())
