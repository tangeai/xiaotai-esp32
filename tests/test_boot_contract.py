"""Static startup contracts; scheduling and failure injection remain separate tests."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootContract(unittest.TestCase):
    def test_application_owns_stage_order(self):
        source = (ROOT / "main/xiaotai/application/application.cc").read_text()
        stages = ("xiaotai_startup_initialize_platform()", "starter_media_init()",
                  "starter_product_start()", "wifi_manager_start()", "xiaotai_startup_launch_services()")
        self.assertEqual(sorted(source.index(stage) for stage in stages), [source.index(stage) for stage in stages])
        self.assertEqual(source.count("if (error_ != ESP_OK) return error_;"), len(stages) + 1)

    def test_native_profile_failure_is_not_reported_started(self):
        source = (ROOT / "main/xiaotai/application/application.cc").read_text()
        native = source.split("phase_ = BootPhase::Services;", 1)[1].split("#else", 1)[0]
        self.assertLess(native.index("error_ = xiaotai_profile_main()"), native.index("if (error_ != ESP_OK) return error_;"))
        self.assertLess(native.index("if (error_ != ESP_OK) return error_;"), native.index("BootPhase::Started"))

    def test_cores3_reserve_precedes_worker(self):
        source = (ROOT / "main/xiaotai/application/cores3/startup/app_main.c").read_text()
        launch = source.split("esp_err_t xiaotai_profile_main(void)", 1)[1]
        self.assertLess(launch.index("s_tirtc_internal_reserve = heap_caps_malloc("),
                        launch.index("xTaskCreateStaticPinnedToCore("))

    def test_worker_cannot_start_before_reserve_is_published(self):
        source = (ROOT / "main/xiaotai/application/startup_services.c").read_text()
        launch = source.split("esp_err_t xiaotai_startup_launch_services(void)", 1)[1]
        self.assertLess(launch.index("s_tirtc_internal_reserve = heap_caps_malloc("),
                        launch.index("xTaskCreateStaticPinnedToCore("))
        self.assertIn("heap_caps_free(s_tirtc_internal_reserve);", launch)
        self.assertIn("return ESP_ERR_NO_MEM;", launch)


if __name__ == "__main__":
    unittest.main()
