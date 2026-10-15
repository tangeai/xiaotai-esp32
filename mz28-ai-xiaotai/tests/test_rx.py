from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import rx_to_wav as rx
from opus_packets import unpack_packets, pack_packets, packet_samples, to_ogg


class RxTests(unittest.TestCase):
    def log(self, packets):
        data = pack_packets(packets)
        return f"RX_OPUS_BEGIN {len(data)} rate=16000 channels=1\nRX {data.hex()}\nRX_OPUS_END"

    def test_duration(self):
        self.assertEqual(packet_samples(b"\xf8\xff\xfe"), 960)
        self.assertEqual(packet_samples(b"\x80"), 120)
        with self.assertRaises(ValueError):
            packet_samples(b"\xfb\x07")
        with self.assertRaises(ValueError):
            packet_samples(b"\xfc\xff\xfe")

    def test_dump(self):
        packets = [b"\xf8\xff\xfe", b"\xf8\x00"]
        self.assertEqual(unpack_packets(rx.extract(self.log(packets))), packets)

    def test_incomplete(self):
        for text in (
            "RX_OPUS_BEGIN 6 rate=16000 channels=1\nRX 0300f8fffe\nRX_OPUS_END",
            "RX_OPUS_BEGIN 2 rate=16000 channels=1\nRX 0300\nRX_OPUS_END",
            "RX_OPUS_BEGIN 1 rate=16000 channels=1\nRX ff\nRX_OPUS_END",
            "RX_OPUS_BEGIN 2 rate=16000 channels=1\nRX 0000\nRX_OPUS_END"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                rx.extract(text)

    def test_timestamped_dump(self):
        text = "\n".join("[13:00:00] " + line for line in self.log([b"\xf8\xff\xfe"]).splitlines())
        self.assertEqual(unpack_packets(rx.extract(text)), [b"\xf8\xff\xfe"])

    def test_multiline_serial_dump(self):
        packets = [b"\xf8\xff\xfe"] * 50
        data = pack_packets(packets)
        lines = [f"RX_OPUS_BEGIN {len(data)} rate=16000 channels=1"]
        lines += ["RX " + data[i:i + 32].hex() for i in range(0, len(data), 32)]
        lines += ["RX_OPUS_END"]
        for newline in ("\n", "\r\n", "\n\n"):
            for prefix in ("", "[18:00:00.123] "):
                with self.subTest(newline=newline, prefix=prefix):
                    text = newline.join(prefix + line for line in lines)
                    self.assertEqual(unpack_packets(rx.extract(text)), packets)

    def test_empty_and_old_format(self):
        for text in ("RX_OPUS_BEGIN 0 rate=16000 channels=1\nRX_OPUS_END",
                     "RX_ALAW_BEGIN 2\nRX d555\nRX_ALAW_END"):
            with self.assertRaises(ValueError):
                rx.extract(text)

    def test_last_complete_and_nested(self):
        first = self.log([b"\xf8\xff\xfe"])
        last = self.log([b"\xf8\x00"])
        self.assertEqual(rx.extract(first + "\n" + last), pack_packets([b"\xf8\x00"]))
        with self.assertRaises(ValueError):
            rx.extract("RX_OPUS_BEGIN 5 rate=16000 channels=1\n" + first)

    @unittest.skipUnless(shutil.which("ffmpeg"), "FFmpeg required for real Opus decoding")
    def test_real_decode_silence_and_asset(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "silence.wav"
            samples = rx.decode_wav([b"\xf8\xff\xfe"] * 50, output)
            self.assertEqual(samples, 16000)
            with wave.open(str(output), "rb") as wave_file:
                self.assertEqual(wave_file.getframerate(), 16000)
                self.assertEqual(wave_file.getnchannels(), 1)
                self.assertEqual(wave_file.getsampwidth(), 2)
            asset = Path(__file__).parents[1] / "components/wechat_voip/assets/audio.opuspkt"
            packets = unpack_packets(asset.read_bytes())
            output = Path(folder) / "asset.wav"
            self.assertEqual(rx.decode_wav(packets, output), len(packets) * 320)
            with wave.open(str(output), "rb") as wave_file:
                pcm = wave_file.readframes(wave_file.getnframes())
                self.assertGreater(len(set(pcm)), 100)

    def test_corrupt_bitstream_rejected(self):
        if not shutil.which("ffmpeg"):
            self.skipTest("FFmpeg required")
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(subprocess.CalledProcessError):
                rx.decode_wav([b"\xfa\xff"], Path(folder) / "bad.wav")

    def test_ogg_page_crc_and_packet_boundaries(self):
        from opus_packets import _crc
        import struct
        data = to_ogg([b"\xf8" + b"\x00" * 254])
        count = 0
        while data:
            self.assertEqual(data[:4], b"OggS")
            segments = data[26]
            size = 27 + segments + sum(data[27:27 + segments])
            page = bytearray(data[:size])
            expected, = struct.unpack_from("<I", page, 22)
            page[22:26] = b"\x00" * 4
            self.assertEqual(_crc(page), expected)
            count += 1
            data = data[size:]
        self.assertEqual(count, 3)


if __name__ == "__main__":
    unittest.main()
