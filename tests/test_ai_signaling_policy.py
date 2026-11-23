"""Verify the default signaling policy on the generated product graphs."""

from pathlib import Path
import json
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection as selection  # noqa: E402
import product_scope  # noqa: E402


class SignalingPolicy(unittest.TestCase):
    def test_only_xmini_default_ai_and_call_pause_signaling(self):
        xmini = "xiaotai-xmini-c3-v3"
        enabled = ".pause_platform_signaling_while_active = true"
        wait_for_release = ".resume_signaling_after_link_release = true"
        products = sorted((ROOT / "products").glob("*/product.json"))
        for path in products:
            product = selection.load_product(path)
            with self.subTest(product=product["name"]):
                graph = selection.plan(ROOT, product)
                apps, modules, board = graph["units"]
                source = selection.generate(apps, modules, product, board)
                ai = re.search(r"^static xiaotai_ai_talk_config_t const (\w+) = (.*);$", source, re.M)
                call = re.search(r"^static xiaotai_call_config_t const (\w+) = (.*);$", source, re.M)
                if product["name"] == xmini:
                    self.assertIsNotNone(ai)
                    self.assertIsNotNone(call)
                    self.assertEqual(ai.group(2).count(enabled), 1)
                    self.assertEqual(call.group(2).count(enabled), 1)
                    self.assertEqual(source.count(enabled), 2)
                    self.assertIn(wait_for_release, ai.group(2))
                    self.assertIn(f"xiaotai_ai_talk_configure(&{ai.group(1)})", source)
                    self.assertIn(f"xiaotai_call_configure(&{call.group(1)})", source)
                    self.assertTrue({"call", "wechat", "room"} <= set(graph["modules"]))
                else:
                    self.assertNotIn(enabled, source)
                    self.assertNotIn(wait_for_release, source)

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_xmini_sdk_memory_profile_keeps_board_defaults(self):
        name = "xiaotai-xmini-c3-v3"
        product = selection.load_product(ROOT / "products" / name / "product.json")
        board_defaults = (ROOT / "boards/xmini/c3-v3/sdkconfig.board").read_text(encoding="utf-8").splitlines()
        self.assertIn("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6", board_defaults)
        rom_flash = "CONFIG_SPI_FLASH_ROM_IMPL=y"
        udp_mailbox = "CONFIG_LWIP_UDP_RECVMBOX_SIZE=16"
        heap_flash = "CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y"
        self.assertNotIn(rom_flash, board_defaults)
        self.assertNotIn(udp_mailbox, board_defaults)
        self.assertNotIn(heap_flash, board_defaults)
        for path in sorted((ROOT / "products").glob("*/product.json")):
            if path.parent.name != name:
                config = selection.plan(ROOT, selection.load_product(path))["sdkconfig"]
                self.assertNotIn(rom_flash, config)
                self.assertNotIn(udp_mailbox, config)
                self.assertNotIn(heap_flash, config)
        for variant in (None, "diagnostics"):
            with self.subTest(variant=variant):
                selected = selection.apply_variant(product, variant) if variant else product
                graph = selection.plan(ROOT, selected)
                self.assertIn("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4", graph["sdkconfig"])
                self.assertIn("CONFIG_ESP_WIFI_RX_BA_WIN=4", graph["sdkconfig"])
                self.assertIn(rom_flash, graph["sdkconfig"])
                self.assertIn(udp_mailbox, graph["sdkconfig"])
                self.assertEqual(graph["sdkconfig"].count(heap_flash), 1)
                self.assertEqual(selected["parameters"]["media"]["downlink.capacity_packets"], 10)
                apps, modules, board = graph["units"]
                source = selection.generate(apps, modules, selected, board)
                self.assertIn('{.name = "downlink.capacity_packets", .value = 10}', source)
                self.assertEqual(source.count(".resume_signaling_after_link_release = true"), 1)
                platform = re.search(r"^static xiaotai_platform_config_t const (\w+) = (.*);$", source, re.M)
                self.assertIsNotNone(platform)
                self.assertIn(".signaling_stack_bytes = 3072", platform.group(2))
                self.assertIn(f"value = {platform.group(1)};", source)
                self.assertIn("xiaotai_platform_configure(&value)", source)


if __name__ == "__main__":
    unittest.main()
