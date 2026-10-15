import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from opus_packets import unpack_packets, packet_samples


class AssetContractTests(unittest.TestCase):
    def test_supplied_sdk_matches_provenance(self):
        expected = {
            "lib/libTiRTC.a": "05d37bb0466d262aee634e9a8c0da7e0088410d7a109a840cb854cc2bbb3d73c",
            "include/tiRTC.h": "b1315121abf843c43c669f754382cab302c3b0b35bdfe41f4c4756ef95636293",
            "include/basedef.h": "23883b8040838fcda10927523536def667cfe7be7fcd8b6f4a1c9410d3e03c04",
        }
        lock = ROOT / "components/tirtc_sdk/release-lock.json"
        if lock.exists():
            profile = json.loads(lock.read_text(encoding="utf-8"))
            expected["lib/libTiRTC.a"] = profile["sdk_sha256"]
            self.assertEqual(profile["target"], "esp32c3")
            self.assertEqual(profile["idf_version"], "5.5.5")
        for name, digest in expected.items():
            with self.subTest(file=name):
                actual = hashlib.sha256((ROOT / "components/tirtc_sdk" / name).read_bytes()).hexdigest()
                self.assertEqual(actual, digest)

    def test_opus_asset(self):
        asset = ROOT / "components/wechat_voip/assets/audio.opuspkt"
        data = asset.read_bytes()
        manifest = json.loads(asset.with_suffix(".opuspkt.json").read_text(encoding="utf-8"))
        packets = unpack_packets(data)
        self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["sha256"])
        self.assertEqual(manifest["source_sha256"],
                         "807305ac8e76212b271c5de21d4fc082e08fa369e42b20dd44363a895a3e2d4f")
        self.assertEqual(manifest["codec"], "opus")
        self.assertEqual(manifest["sample_rate"], 16000)
        self.assertEqual(manifest["packet_count"], len(packets))
        self.assertEqual(len(packets), 501)
        for packet in packets:
            self.assertEqual(len(packet), 60)
            self.assertEqual(packet_samples(packet), 960)

    def test_local_binding_prompts(self):
        prompt_dir = ROOT / "components/product_audio/assets/prompts"
        generated = {
            "ring_incoming": ("original-bright-incoming-ring", 67),
            "ring_outgoing": ("original-telephone-outgoing-beep", 13),
            "popup": ("original-crisp-tap", 11),
            "success": ("original-rising-success-chime", 21),
            "exclamation": ("original-warning-chime", 26),
            "call_ended": ("original-descending-call-end", 19),
        }
        names = [
            "activation",
            "wifi_provisioning",
            "success",
            "popup",
            "exclamation",
            "call_ended",
            "ring_incoming",
            "ring_outgoing",
        ] + [
            f"digit_{value}" for value in range(10)
        ]
        for name in names:
            with self.subTest(prompt=name):
                asset = prompt_dir / f"{name}.opuspkt"
                data = asset.read_bytes()
                manifest = json.loads(
                    asset.with_suffix(".opuspkt.json").read_text(encoding="utf-8")
                )
                packets = unpack_packets(data)
                self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["sha256"])
                self.assertEqual(manifest["codec"], "opus")
                self.assertEqual(manifest["sample_rate"], 16000)
                self.assertEqual(manifest["channels"], 1)
                self.assertEqual(manifest["packet_ms"], 20)
                if name in ("activation", "wifi_provisioning"):
                    self.assertEqual(
                        manifest["source_license"],
                        "Microsoft Windows SAPI generated voice",
                    )
                    expected_text = (
                        "登录微信小程序搜索小钛进行绑定，钛是金属钛的钛"
                        if name == "activation" else "请打开WiFi来为设备配置无线网络。"
                    )
                    self.assertEqual(manifest["source_text"], expected_text)
                    self.assertTrue(manifest["source_voice"].startswith("Microsoft Huihui Desktop"))
                elif name in generated:
                    self.assertEqual(
                        manifest["source_license"],
                        "Original project-generated sound",
                    )
                    self.assertEqual(manifest["source_kind"], generated[name][0])
                    self.assertEqual(manifest["packet_count"], generated[name][1])
                else:
                    self.assertEqual(manifest["source_license"], "MIT")
                self.assertEqual(manifest["packet_count"], len(packets))
                for packet in packets:
                    self.assertEqual(len(packet), 60)
                    self.assertEqual(packet_samples(packet), 960)

        incoming = json.loads(
            (prompt_dir / "ring_incoming.opuspkt.json").read_text(encoding="utf-8")
        )
        outgoing = json.loads(
            (prompt_dir / "ring_outgoing.opuspkt.json").read_text(encoding="utf-8")
        )
        ended = json.loads(
            (prompt_dir / "call_ended.opuspkt.json").read_text(encoding="utf-8")
        )
        self.assertEqual(incoming["source_kind"], "original-bright-incoming-ring")
        self.assertEqual(outgoing["source_kind"], "original-telephone-outgoing-beep")
        self.assertEqual(incoming["packet_count"], 67)
        self.assertEqual(outgoing["packet_count"], 13)
        self.assertNotEqual(incoming["sha256"], outgoing["sha256"])
        self.assertEqual(ended["source_kind"], "original-descending-call-end")
        self.assertEqual(ended["packet_count"], 19)
