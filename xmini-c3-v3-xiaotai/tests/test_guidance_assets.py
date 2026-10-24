import hashlib
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


class GuidanceAssets(unittest.TestCase):
    def test_bundle_and_index(self):
        directory = ROOT / "components/xmini_audio/assets"
        manifest = json.loads((directory / "guidance_manifest.json").read_text(encoding="utf-8"))
        config = json.loads((ROOT / "tools/guidance.json").read_text(encoding="utf-8"))
        data = (directory / "guidance.alaw").read_bytes()
        self.assertEqual((manifest["sample_rate"], manifest["channels"], manifest["codec"]), (8000, 1, "pcm_alaw"))
        self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["bundle_sha256"])
        self.assertEqual(len(manifest["clips"]), 23)
        self.assertEqual([c["name"] for c in manifest["clips"]], [c["name"] for c in config["clips"]])
        table = re.findall(r"\{(\d+), (\d+)\}", (directory / "guidance_index.inc").read_text(encoding="ascii"))
        self.assertEqual(len(table), 23)
        offset = 0
        for clip, entry in zip(manifest["clips"], table):
            self.assertEqual((clip["offset"], clip["bytes"]), tuple(map(int, entry)))
            self.assertEqual(clip["offset"], offset)
            self.assertGreater(clip["bytes"], 0)
            self.assertEqual(clip["bytes"] % 160, 0)
            chunk = data[offset:offset + clip["bytes"]]
            self.assertEqual(hashlib.sha256(chunk).hexdigest(), clip["sha256"])
            offset += clip["bytes"]
        self.assertEqual(offset, len(data))
        digits = manifest["clips"][13:]
        self.assertEqual([c["name"] for c in digits], [f"digit_{i}" for i in range(10)])
        # A six-digit code must finish within the callback's bounded 30 s wait.
        self.assertLess(manifest["clips"][5]["bytes"] + 6 * max(c["bytes"] for c in digits), 30 * 8000)

    def test_chinese_coverage(self):
        source = (ROOT / "components/xmini_ui/xmini_cn_font.inc").read_text(encoding="ascii")
        codes = [int(code, 16) for code in re.findall(r"\{0x([0-9a-f]+), \{", source)]
        self.assertEqual(codes, list(range(32, 127)) + list(range(0x4E00, 0xA000)))
        for name in ("xmini_ui.c", "xmini_guide.c"):
            text = (ROOT / "components/xmini_ui" / name).read_text(encoding="utf-8")
            for char in set(re.findall(r"[\u4e00-\u9fff]", text)):
                self.assertIn(ord(char), codes, char)

    def test_regenerate_prompts_preserves_font(self):
        spec = importlib.util.spec_from_file_location("prepare_guidance", ROOT / "tools/prepare_guidance.py")
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        for legacy_font in (False, True):
            with self.subTest(legacy_font=legacy_font), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "tools").mkdir()
                (root / "components/xmini_audio").mkdir(parents=True)
                font_dir = root / "components/xmini_ui"
                font_dir.mkdir()
                original = {"xmini_cn_font.inc": b"full-CJK-font-sentinel\n",
                            "font_source.json": b'{"glyphs":21087}\n'}
                for name, content in original.items():
                    (font_dir / name).write_bytes(content)
                config = {"voice": "offline-test", "rate": "+0%", "clips": [{"name": "first"}, {"name": "second"}]}
                (root / "tools/guidance.json").write_text(json.dumps(config), encoding="utf-8")
                wav_dir = root / "wav"
                wav_dir.mkdir()
                for name in ("first", "second"):
                    (wav_dir / f"{name}.wav").write_bytes(name.encode("ascii"))
                argv = ["prepare_guidance.py", "--wav-dir", str(wav_dir), "--ffmpeg", "test-ffmpeg"]
                if legacy_font:
                    argv += ["--font", str(root / "not-read.ttf")]
                encoded = (b"\x31" * 161, b"\x87" * 160)
                with mock.patch.object(generator, "__file__", str(root / "tools/prepare_guidance.py")), \
                     mock.patch("sys.argv", argv), \
                     mock.patch.object(generator.subprocess, "check_output", side_effect=encoded) as convert, \
                     contextlib.redirect_stdout(io.StringIO()):
                    generator.main()
                self.assertEqual(convert.call_count, 2)
                for call in convert.call_args_list:
                    self.assertEqual(call.args[0][-9:],
                                     ["-ac", "1", "-ar", "8000", "-c:a", "pcm_alaw", "-f", "alaw", "pipe:1"])
                for name, content in original.items():
                    self.assertEqual((font_dir / name).read_bytes(), content)
                assets = root / "components/xmini_audio/assets"
                bundle = encoded[0] + b"\xd5" * 159 + encoded[1]
                self.assertEqual((assets / "guidance.alaw").read_bytes(), bundle)
                manifest = json.loads((assets / "guidance_manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["bundle_sha256"], hashlib.sha256(bundle).hexdigest())
                self.assertEqual([(clip["offset"], clip["bytes"]) for clip in manifest["clips"]], [(0, 320), (320, 160)])
                for clip in manifest["clips"]:
                    chunk = bundle[clip["offset"]:clip["offset"] + clip["bytes"]]
                    self.assertEqual(clip["sha256"], hashlib.sha256(chunk).hexdigest())
                    self.assertEqual(clip["wav_sha256"], hashlib.sha256(clip["name"].encode("ascii")).hexdigest())


if __name__ == "__main__":
    unittest.main()
