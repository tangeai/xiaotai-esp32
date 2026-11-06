import hashlib
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HardwareBoundaries(unittest.TestCase):
    def test_board_interface_without_idf_lvgl_or_network_headers(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "board.cc"
            source.write_text('#include "board.h"\nint main() { return 0; }\n')
            subprocess.run(["c++", "-std=c++17", "-fsyntax-only", "-I", str(ROOT / "main/boards/common"), str(source)], check=True)

    def test_media_and_ui_do_not_create_shared_peripherals(self):
        for path in (ROOT / "main/xiaotai/media/s3/src/starter_media.c", ROOT / "main/xiaotai/ui/lcd320/src/starter_product.c"):
            source = path.read_text()
            for initializer in ("i2c_new_master_bus(", "i2s_new_channel(", "esp_codec_dev_new(", "esp_lcd_new_panel_st7789(", "spi_bus_initialize("):
                self.assertNotIn(initializer, source)
        hardware = (ROOT / "main/boards/lckfb/szpi-esp32s3/xiaotai_hardware.c").read_text()
        self.assertIn("xSemaphoreTake(s_output_mutex", hardware)
        self.assertIn("if (err == ESP_OK) s_pca9557_output = value;", hardware)

    def test_phone_pixels_preserved_across_lvgl_layout_change(self):
        source = (ROOT / "main/xiaotai/ui/lcd320/src/starter_product_s3_phone.inc").read_text()
        body = source.split("s3_phone_shortcut_pixels[] = {", 1)[1].split("};", 1)[0]
        pixels = bytes(int(s, 16) for s in re.findall(r"0x[0-9a-fA-F]{2}", body))
        count = 44*44
        self.assertEqual(len(pixels), 3*count)
        original = bytes(value for i in range(count) for value in (pixels[2*i+1], pixels[2*i], pixels[2*count+i]))
        self.assertEqual(hashlib.sha256(original).hexdigest(), "c56b37a767fe8a0b4eeb1389568701b6d3f626315de38354ce73592ecd4f590d")

    def test_firmware_uses_owned_application_and_transport(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        for source in ("protocols/websocket_protocol.cc", "protocols/mqtt_protocol.cc", '"application.cc"', '"ota.cc"'):
            self.assertNotIn(source, cmake)
        self.assertIn("xiaotai/transport/tirtc", cmake)
        self.assertIn("xiaotai/application/application.cc", cmake)
        board = (ROOT / "main/boards/common/board.h").read_text()
        self.assertNotIn("GetNetwork", board)
        self.assertNotIn("GenerateUuid", board)
        self.assertIn("Board& SelectedBoard()", board)


if __name__ == "__main__":
    unittest.main()
