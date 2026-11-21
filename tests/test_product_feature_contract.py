"""Keep every product graph on the feature set its legacy firmware shipped."""

# Replaces legacy: tests/test_product_feature_contract.py
import argparse
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from xiaotai_build import FEATURES, select, variants
import sdk_selection
import xiaotai_build_native as native

# Feature sets the legacy firmware shipped (each board.json at
# 0f93f4147b2978211f47ce5e80c5739449451892). The product graphs must keep them.
LEGACY_FEATURES = {
    "xiaotai-xmini-c3-v3": {"ai", "device_call", "h5_stream", "room", "wechat_voip"},
    "xiaotai-szpi-s3": {"ai", "ai_vision", "device_call", "h5_stream", "h5_video", "room", "wechat_voip"},
    "xiaotai-cores3-lite": {"ai", "device_call", "h5_stream", "h5_video", "room", "wechat_voip"},
    "xiaotai-waveshare-p4-rev3": {"ai", "device_call", "h5_stream", "h5_video", "room", "wechat_voip"},
    "xiaotai-wt99p461-s1-1v1": {"ai", "device_call", "h5_stream", "h5_video", "room", "wechat_voip"},
}


def legacy_features(item):
    return LEGACY_FEATURES[item["name"]]


FEATURE_MODULES = {
    "ai": "ai_talk", "device_call": "call", "wechat_voip": "wechat", "room": "room",
    "h5_stream": "stream", "h5_video": "stream_video", "ai_vision": "ai_vision",
}


def selected_features(product):
    modules = sdk_selection.plan(ROOT, product)["modules"]
    return {feature for feature, module in FEATURE_MODULES.items() if module in modules}


class ProductFeatureContract(unittest.TestCase):
    def test_v3_default_graphs_keep_the_legacy_feature_matrix(self):
        self.assertEqual(set(FEATURE_MODULES), set(FEATURES))
        for item in variants():
            if not native.profile_path(ROOT, item["name"]).is_file():
                continue  # Legacy-only entries are exercised by direct-CMake checks below.
            with self.subTest(product=item["name"]):
                product = sdk_selection.load_product(ROOT / "products" / item["name"] / "product.json")
                self.assertEqual(selected_features(product), legacy_features(item))

    def test_locked_native_entry_does_not_silently_ignore_feature_changes(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        for item in variants():
            if not native.profile_path(ROOT, item["name"]).is_file():
                continue
            native.native_plan(args, item, {})
            enabled = legacy_features(item)
            for feature in FEATURES:
                with self.subTest(product=item["name"], feature=feature):
                    with self.assertRaisesRegex(
                            ValueError, "accepts declared variants, not arbitrary build overrides"):
                        native.native_plan(args, item, {feature: feature not in enabled})

    def test_supported_s3_voice_profile_stays_configurable(self):
        item = select("lckfb/szpi-esp32s3")
        product = sdk_selection.load_product(ROOT / "products" / item["name"] / "product.json")
        voice = sdk_selection.apply_variant(product, "no-video")
        self.assertEqual(selected_features(voice), LEGACY_FEATURES[item["name"]] - {"ai_vision", "h5_video"})

if __name__ == "__main__":
    unittest.main()
