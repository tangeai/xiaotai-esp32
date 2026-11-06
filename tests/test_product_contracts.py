"""Positive, incompatible and future-board cases for the actual build gate."""
from copy import deepcopy
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import product_contract as contracts
import xiaotai_build as build


class ProductContracts(unittest.TestCase):
    BASELINE = {"xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-waveshare-p4-rev3",
                "xiaotai-wt99p461-s1-1v1", "xiaotai-xmini-c3-v3", "xiaotai-mz28-ai-ppd"}
    @classmethod
    def setUpClass(cls):
        cls.variants = build.variants()
        cls.adapters = contracts.read(ROOT / "config/adapter-contracts.json")
        cls.products = contracts.read(ROOT / "config/product-contracts.json")["products"]

    def hardware(self, variant):
        return contracts.read(ROOT / "main/boards" / variant["board"] / "capabilities.json")

    def combination(self, variant):
        product = self.products[variant["name"]]
        return (self.hardware(variant), self.adapters["media"][product["media"]],
                self.adapters["interaction"][product["interaction"]], product["features"])

    def test_six_defaults_use_real_build_gate(self):
        self.assertEqual(set(self.products), {item["name"] for item in self.variants})
        for item in self.variants:
            with self.subTest(board=item["board"]), patch.dict(os.environ, {}, clear=True):
                report = build.plan(item, "firmware")["compatibility"]
                self.assertTrue(report["hardware_contract_checked"])
                self.assertFalse(report["hardware_tested"])
                self.assertFalse(report["runtime_portable"])
                self.assertEqual(len(report["inputs"]), 4)
                self.assertIn("scripts/product_contract.py", report["inputs"])

    def test_36_hardware_product_pairs(self):
        matched = []
        baseline = [item for item in self.variants if item["name"] in self.BASELINE]
        self.assertEqual(len(baseline), 6)
        for board in baseline:
            for product in baseline:
                _, media, interaction, features = self.combination(product)
                try:
                    contracts.check_combination(self.hardware(board), media, interaction, features)
                except ValueError:
                    continue
                matched.append((board["name"], product["name"]))
        expected = {(v["name"], v["name"]) for v in baseline}
        expected |= {("xiaotai-szpi-s3", "xiaotai-cores3-lite"),
                     ("xiaotai-cores3-lite", "xiaotai-szpi-s3")}
        self.assertEqual(set(matched), expected)

    def test_every_required_port_is_checked(self):
        for item in self.variants:
            original, media, interaction, features = self.combination(item)
            required = set(media["requires"] + interaction["requires"] + ["network.ip", "storage.nvs"])
            if {"h5_video", "ai_vision"} & set(features):
                required.add("camera.capture")
            for port in required:
                with self.subTest(board=item["board"], missing=port):
                    hardware = deepcopy(original)
                    hardware["capabilities"].remove(port)
                    with self.assertRaises(ValueError):
                        contracts.check_combination(hardware, media, interaction, features)
                    self.assertEqual(original, self.hardware(item))

    def test_pcma_not_coupled_to_oled_or_chip_name(self):
        hardware = self.hardware(build.select("mz28-ai-ppd"))
        hardware["target"] = "future-chip-fixture"
        hardware["capabilities"].remove("audio.opus16k20.capture")
        hardware["capabilities"].append("audio.pcm.capture")
        contracts.check_combination(hardware, self.adapters["media"]["pcm-pcma-mini"],
                                    self.adapters["interaction"]["headless-key-led-voice"], ["wechat_voip"])
        # This proves requirements composition only, not Mini SDK support for a new chip.

    def test_encoder_and_remote_input_are_explicit_adapter_choices(self):
        original = self.hardware(build.select("mz28-ai-ppd"))
        interaction = {"requires": ["feedback.audio"],
                       "one_of": [["input.key", "input.encoder", "input.remote"]],
                       "display": {"kind": "none", "width": 0, "height": 0}}
        for source in ("input.key", "input.encoder", "input.remote"):
            hardware = deepcopy(original)
            hardware["capabilities"].remove("input.key")
            hardware["capabilities"].append(source)
            contracts.check_combination(hardware, self.adapters["media"]["uart-opus-pcm"], interaction, ["wechat_voip"])
        hardware["capabilities"].remove(source)
        with self.assertRaisesRegex(ValueError, "needs one of"):
            contracts.check_combination(hardware, self.adapters["media"]["uart-opus-pcm"], interaction, ["wechat_voip"])

    def test_headless_is_not_null_interaction(self):
        hardware, media, interaction, features = self.combination(build.select("mz28-ai-ppd"))
        for capability in ("input.key", "feedback.rgb", "feedback.audio"):
            changed = deepcopy(hardware)
            changed["capabilities"].remove(capability)
            with self.assertRaisesRegex(ValueError, "interaction"):
                contracts.check_combination(changed, media, interaction, features)

    def test_uart_encoded_capture_cannot_pretend_to_be_pcm(self):
        hardware = self.hardware(build.select("mz28-ai-ppd"))
        with self.assertRaisesRegex(ValueError, "audio.pcm.capture"):
            contracts.check_adapter(hardware, self.adapters["media"]["pcm-pcma-mini"], "media")

    def test_display_geometry_pixel_format_and_types(self):
        original = self.hardware(build.select("xmini/c3-v3"))
        for field, value in (("width", 64), ("width", True), ("height", -1), ("kind", "rgb565")):
            hardware = deepcopy(original)
            hardware["display"][field] = value
            with self.assertRaises(ValueError):
                contracts.check_adapter(hardware, self.adapters["interaction"]["oled-keys"], "interaction")
        hardware = self.hardware(build.select("mz28-ai-ppd"))
        hardware["capabilities"].append("input.pointer")
        with self.assertRaises(ValueError):
            contracts.check_hardware(hardware)

    def test_unknown_duplicate_and_schema_errors_fail_closed(self):
        original = self.hardware(build.select("mz28-ai-ppd"))
        for caps in (["network.typo"], ["network.ip", "network.ip"], [None], "network.ip"):
            hardware = deepcopy(original)
            hardware["capabilities"] = caps
            with self.assertRaises(ValueError):
                contracts.check_hardware(hardware)
        for value in (True, 2, "1"):
            hardware = deepcopy(original)
            hardware["schema"] = value
            with self.assertRaises(ValueError):
                contracts.check_hardware(hardware)
        adapter = deepcopy(self.adapters["interaction"]["oled-keys"])
        adapter["require"] = adapter.pop("requires")
        with self.assertRaises(ValueError):
            contracts.check_adapter(original, adapter, "interaction")

    def test_feature_dependencies_and_camera_requirements(self):
        hardware, media, interaction, _ = self.combination(build.select("xmini/c3-v3"))
        for features in (["h5_video"], ["ai_vision"], ["h5_video", "h5_stream"],
                         ["ai_vision", "ai"], ["unregistered-feature"]):
            with self.assertRaises(ValueError):
                contracts.check_combination(hardware, media, interaction, features)

    def test_native_graph_cannot_be_reused_by_renaming_board(self):
        variant = deepcopy(build.select("m5stack/core-s3"))
        variant["board"] = "lckfb/szpi-esp32s3"
        with self.assertRaisesRegex(ValueError, "board-bound"):
            build.configuration(variant, "firmware")

    def test_product_configuration_cannot_silently_drift(self):
        original = build.select("xmini/c3-v3")
        for field, value in (("ui", "none"), ("audio", "opus16k"),
                             ("profile", "mz28"), ("features", ["wechat_voip"])):
            variant = deepcopy(original)
            variant["xiaotai"][field] = value
            with self.assertRaises(ValueError):
                build.configuration(variant, "firmware")
        variant = deepcopy(original)
        variant["target"] = "esp32s3"
        with self.assertRaisesRegex(ValueError, "target"):
            build.configuration(variant, "firmware")

    def test_contract_paths_cannot_escape_repository(self):
        variant = deepcopy(build.select("xmini/c3-v3"))
        variant["board"] = "../../../../other-project"
        with self.assertRaises(ValueError):
            contracts.validate_variant(variant, ROOT)


if __name__ == "__main__":
    unittest.main()
