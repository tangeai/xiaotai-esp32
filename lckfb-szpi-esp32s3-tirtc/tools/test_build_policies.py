"""Host-only checks for the portable gate runner; no compiler/device required."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import check_build_policies as policies


class PortablePoliciesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="build policy ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "中文工程"
        self.root.mkdir()
        self.elf = self.root / "firmware image.elf"
        self.elf.write_bytes(b"fixture")
        self.args = argparse.Namespace(root=self.root, elf=self.elf,
            nm="C:/IDF tools/bin/xtensa-esp32s3-elf-nm.exe",
            objdump="C:/IDF tools/bin/xtensa-esp32s3-elf-objdump.exe", product=None)
        self.context = policies.Context(self.args)

    def test_every_policy_has_a_direct_python_entry(self):
        names = {
            "i2c_driver_family", "product_touch_i2c", "backlight_polarity", "binding_prompt",
            "wifi_provisioning", "product_ui", "time_sync", "tirtc_startup_order",
            "realtime_connect_memory", "tirtc_thread_stack", "voice_intent", "wake_image",
            "session_priority", "audio_only", "media_cpu_fairness", "i2s_resource",
            "audio_capture", "aec", "downlink_audio", "memory_placement", "ai_stack",
        }
        self.assertEqual(names, set(policies.POLICIES))
        for name in names:
            with self.subTest(policy=name):
                checkers = {key: Mock() for key in names}
                with patch.dict(policies.POLICIES, checkers, clear=True), \
                     patch.object(policies.sys, "argv", ["check_build_policies.py", "--policy", name]), \
                     patch.object(policies, "Context", return_value=self.context), \
                     patch("builtins.print"):
                    self.assertEqual(policies.main(), 0)
                checkers[name].assert_called_once_with(self.context)
                for other in names - {name}:
                    checkers[other].assert_not_called()

    def test_utf8_bom_crlf_and_non_ascii_paths(self):
        (self.root / "source.c").write_bytes("\ufeff第一行\r\n第二行\r\n".encode("utf-8"))
        self.assertEqual(self.context.text("source.c"), "第一行\n第二行\n")
        self.assertEqual(self.context.compact("source.c"), "第一行第二行")
        with self.assertRaises(OSError):
            self.context.text("missing.c")

    def test_binutils_paths_are_not_split_or_passed_to_shell(self):
        completed = subprocess.CompletedProcess([], 0, "symbols", "")
        with patch.object(policies.subprocess, "run", return_value=completed) as run:
            self.assertEqual(self.context.symbols("-C"), "symbols")
            self.assertEqual(self.context.symbols("-C"), "symbols")
        run.assert_called_once()
        args, kwargs = run.call_args
        self.assertEqual(args[0], [self.args.nm, "-C", str(self.elf.resolve())])
        self.assertFalse(kwargs.get("shell", False))
        self.assertEqual(kwargs["encoding"], "utf-8")

    def test_tool_errors_are_failures_not_empty_success(self):
        failed = subprocess.CompletedProcess([], 2, "", "bad image")
        with patch.object(policies.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(policies.PolicyFailure, "exited 2"):
                self.context.symbols("-C")
        with patch.object(policies.subprocess, "run", side_effect=FileNotFoundError):
            with self.assertRaises(FileNotFoundError):
                self.context.symbols("-C")
        self.elf.unlink()
        with self.assertRaisesRegex(policies.PolicyFailure, "ELF is missing"):
            self.context.symbols("-C")

    def test_i2c_conflict_and_windows_map_paths(self):
        with patch.object(self.context, "symbols", return_value="1 T i2c_driver_install\n2 T i2c_new_master_bus\n"):
            with self.assertRaisesRegex(policies.PolicyFailure, "both legacy I2C"):
                policies.i2c_driver_family(self.context)
        self.elf.with_suffix(".map").write_text(r"esp-idf\driver\libdriver.a(i2c.c.obj)", encoding="utf-8")
        with patch.object(self.context, "symbols", return_value="2 T i2c_new_master_bus\n"):
            with self.assertRaisesRegex(policies.PolicyFailure, "legacy I2C object"):
                policies.i2c_driver_family(self.context)

    def test_elf_camera_and_removed_wake_symbols_still_fail(self):
        with patch.object(self.context, "symbols", return_value="1 T esp_camera_init\n"):
            with self.assertRaisesRegex(policies.PolicyFailure, "camera/video"):
                policies.audio_only(self.context)
        with patch.object(self.context, "symbols", return_value="1 T esp_mn_init\n"):
            with self.assertRaisesRegex(policies.PolicyFailure, "MultiNet"):
                policies.wake_image(self.context)

    def test_xtensa_stack_frame_decode_keeps_byte_budget(self):
        for raw, size in (("010136", 128), ("00e136", 112), ("c00136", 24576)):
            disassembly = f"42100000 <runtime_task>:\r\n42100000: {raw} entry a1, {size}\r\n"
            self.assertEqual(policies.entry_frame_bytes(disassembly, "runtime_task"), size)
        with self.assertRaises(policies.PolicyFailure):
            policies.entry_frame_bytes("42100000 <runtime_task>:\n42100000: 010135 other", "runtime_task")
        with self.assertRaises(policies.PolicyFailure):
            policies.entry_frame_bytes("42100000 <different>:\n42100000: 010136 entry", "runtime_task")

    def test_source_requirements_still_fail_when_missing(self):
        source = self.root / "product.c"
        source.write_text("#define LCD_BACKLIGHT_ON_LEVEL 1\n", encoding="utf-8")
        self.args.product = source
        with self.assertRaisesRegex(policies.PolicyFailure, "LCD_BACKLIGHT_ON_LEVEL 0"):
            policies.backlight_polarity(policies.Context(self.args))
        self.assertFalse(policies.has_line("token\nother", r"token\s+other"))
        self.assertEqual(policies.line_range("before\nstart\nbody\n}\nafter", "start", "^}"), "start\nbody\n}")

    def source_context(self):
        args = argparse.Namespace(root=Path(__file__).resolve().parents[1],
                                  elf=None, nm="", objdump="", product=None)
        return policies.Context(args)

    def test_lvgl_fixed_pool_must_be_in_psram_with_configured_size(self):
        (self.root / "sdkconfig").write_text("CONFIG_LV_MEM_SIZE_KILOBYTES=32\n", encoding="utf-8")
        for symbol in ("work_mem_int", "work_mem_int$0", "work_mem_int.1"):
            with self.subTest(symbol=symbol), patch.object(self.context, "symbols",
                    return_value=f"3c020000 00008000 b {symbol}\n"):
                policies.lvgl_pool_placement(self.context)
        for symbols in ("3fcaf3b8 00008000 b work_mem_int$0\n",
                        "3c020000 00004000 b work_mem_int\n", "",
                        "3c020000 00008000 b work_mem_int\n3c030000 00008000 b work_mem_int.1\n"):
            with self.subTest(symbols=symbols), patch.object(self.context, "symbols", return_value=symbols):
                with self.assertRaises(policies.PolicyFailure):
                    policies.lvgl_pool_placement(self.context)

    def test_memory_policy_rejects_internal_payload_or_per_click_stack(self):
        context = self.source_context()
        policies.memory_placement(context)
        for name, old, new in (
                ("runtime", "event->text = heap_caps_malloc(length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)",
                 "event->text = malloc(length + 1U)"),
                ("runtime", "static EXT_RAM_BSS_ATTR char s_call_connect_token", "static char s_call_connect_token"),
                ("runtime", "vSemaphoreDelete(mutex);", "/* missing cleanup */"),
                ("product", "nvs_worker_submit_latest(preferences_write_job,", "xTaskCreate(task)"),
                ("product", "err = nvs_commit(nvs);", "(void)nvs_commit(nvs);")):
            with self.subTest(name=name, old=old):
                original = context.text(name)
                self.assertIn(old, original)
                context.text_cache[context.path(name)] = original.replace(old, new)
                with self.assertRaises(policies.PolicyFailure):
                    policies.memory_placement(context)
                context.text_cache[context.path(name)] = original

    def test_time_sync_keeps_reviewed_primary_and_fallback(self):
        context = self.source_context()
        policies.time_sync(context)
        path = context.path("platform")
        original = context.text("platform")
        for server in ("ntp.aliyun.com", "pool.ntp.org"):
            with self.subTest(server=server):
                context.text_cache[path] = original.replace(server, "REMOVED")
                with self.assertRaises(policies.PolicyFailure):
                    policies.time_sync(context)

    def test_startup_order_ignores_rebind_but_rejects_early_mqtt(self):
        context = self.source_context()
        policies.tirtc_startup_order(context)
        path = context.path("main")
        original = context.text("main")
        start = original.index("static void starter_start_task(void *argument)")
        self.assertIn("platform_client_start(&platform)", original[:start])
        # A platform start inside the bootstrap before TiRTC must still fail.
        early = original[start:].replace("starter_tirtc_start(&tirtc)",
                                         "platform_client_start(&platform); starter_tirtc_start(&tirtc)", 1)
        context.text_cache[path] = original[:start] + early
        with self.assertRaisesRegex(policies.PolicyFailure, "before platform MQTT/TLS"):
            policies.tirtc_startup_order(context)
        context.text_cache[path] = original[:start] + original[start:].replace(
            "starter_tirtc_start(&tirtc)", "REMOVED", 1)
        with self.assertRaisesRegex(policies.PolicyFailure, "cannot locate"):
            policies.tirtc_startup_order(context)

    def test_startup_console_must_remain_opt_in(self):
        context = self.source_context()
        context.text_cache[context.path("main")] = context.text("main").replace(
            "#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE", "#if 1")
        with self.assertRaisesRegex(policies.PolicyFailure, "opt-in config"):
            policies.tirtc_startup_order(context)

    def test_both_receive_queues_must_remain_in_psram(self):
        context = self.source_context()
        # Host-only source checks do not require a generated sdkconfig.
        context.text_cache[context.path("resolved")] = context.text("defaults")
        policies.memory_placement(context)
        path = context.path("media")
        original = context.compact("media")
        for queue in ("s_audio_rx_ready_queue", "s_audio_rx_free_queue"):
            expected = (queue + "=xQueueCreateWithCaps(AUDIO_RX_QUEUE_DEPTH,sizeof(uint8_t),"
                        "MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)")
            for replacement in (
                queue + "=xQueueCreate(AUDIO_RX_QUEUE_DEPTH,sizeof(uint8_t))",
                expected.replace("MALLOC_CAP_SPIRAM", "MALLOC_CAP_INTERNAL"),
            ):
                with self.subTest(queue=queue, replacement=replacement):
                    context.text_cache[path] = original.replace(expected, replacement, 1)
                    with self.assertRaises(policies.PolicyFailure):
                        policies.memory_placement(context)

    def test_aec_capture_timestamp_and_epochs(self):
        context = self.source_context()
        policies.aec(context)
        path = context.path("media")
        original = context.text("media")
        changes = (
            ("read_end / 1000,", "read_end,"),
            ("read_end / 1000,", "0,"),
            ("read_end / 1000,", "read_begin / 1000,"),
            ("capture_epoch, mute_epoch);", "mute_epoch, capture_epoch);"),
            ("capture_epoch, mute_epoch);", "0, mute_epoch);"),
            ("if (read_ok) {", "if (true) {"),
            ("int64_t read_end = esp_timer_get_time();",
             "int64_t read_end = read_begin;"),
            ("bool read_ok = s_microphone_dev &&",
             "int64_t read_end = esp_timer_get_time();\n"
             "        bool read_ok = s_microphone_dev &&"),
        )
        for before, after in changes:
            with self.subTest(before=before, after=after):
                self.assertIn(before, original)
                context.text_cache[path] = original.replace(before, after, 1)
                with self.assertRaisesRegex(policies.PolicyFailure, "AEC input must submit"):
                    policies.aec(context)
        context.text_cache[path] = original

    def test_s3_checks_actual_includes_not_legacy_strings(self):
        root = Path(__file__).resolve().parents[1]
        args = argparse.Namespace(root=root, elf=None, nm="", objdump="", product=None)
        context = policies.Context(args)
        policies.s3_product_ui(context)
        ui_path = context.path("s3_ui")
        original = context.text("s3_ui")
        for token in ("lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP)",
                      "starter_runtime_call_contact(selected)",
                      "s3_render_network(screen)"):
            context.text_cache[ui_path] = original.replace(token, "REMOVED")
            with self.assertRaises(policies.PolicyFailure):
                policies.s3_product_ui(context)
        context.text_cache[ui_path] = original

    def test_s3_home_latest_two_lines_and_face_wake(self):
        context = self.source_context()
        policies.s3_product_ui(context)
        path = context.path("s3_ui")
        original = context.text("s3_ui")
        changes = (
            ("2 * lv_font_get_line_height(", "3 * lv_font_get_line_height("),
            ("const lv_coord_t bottom_inset = 2;", "const lv_coord_t bottom_inset = 10;"),
            ("lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, 0)",
             "lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 0)"),
            ("create_expression_face(screen, (320 - FACE_CANVAS_W) / 2,",
             "create_expression_face(screen, 50,"),
            ('make_button(screen, "", 276, 150, 44, 44,',
             'make_button(screen, "", 272, 48, 44, 44,'),
            ("&s3_phone_shortcut_image", "NULL"),
            ("bool changed = starter_runtime_caption_read(&s3_ui.caption);",
             "if (lv_obj_has_state(s3_ui.caption_panel, LV_STATE_SCROLLED)) return;\n"
             "            bool changed = starter_runtime_caption_read(&s3_ui.caption);"),
        )
        for before, after in changes:
            with self.subTest(before=before):
                self.assertIn(before, original)
                context.text_cache[path] = original.replace(before, after, 1)
                with self.assertRaises(policies.PolicyFailure):
                    policies.s3_product_ui(context)
        context.text_cache[path] = original


    def test_s3_home_header_has_small_menu_and_no_session_button(self):
        context = self.source_context()
        policies.s3_product_ui(context)
        path = context.path("s3_ui")
        original = context.text("s3_ui")
        changes = (
            ("lv_obj_set_pos(s_wifi_signal, 238, 7)",
             "lv_obj_set_pos(s_wifi_signal, 182, 7)"),
            ("lv_obj_set_pos(s_wifi_signal, 238, 7)",
             "lv_obj_set_pos(s_wifi_signal, 238, 10)"),
            ('LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET', "LV_SYMBOL_LIST"),
            ("280, 6, 32, ACTION_MENU)", "272, 2, 44, ACTION_MENU)"),
            ("lv_obj_set_height(more, 24)", "lv_obj_set_height(more, 44)"),
            ("lv_obj_set_ext_click_area(more, 20)", "lv_obj_set_ext_click_area(more, 0)"),
            ("lv_obj_set_style_bg_opa(more, LV_OPA_20, 0)",
             "lv_obj_set_style_bg_opa(more, LV_OPA_COVER, 0)"),
            ("lv_obj_move_foreground(more)", "REMOVED"),
        )
        for before, after in changes:
            with self.subTest(before=before):
                self.assertIn(before, original)
                context.text_cache[path] = original.replace(before, after, 1)
                with self.assertRaises(policies.PolicyFailure):
                    policies.s3_product_ui(context)
        for label in ("唤醒", "休眠"):
            with self.subTest(button=label):
                context.text_cache[path] = original.replace(
                    "lv_obj_set_width(s_header_title, 36);",
                    'lv_obj_set_width(s_header_title, 36);\n'
                    f'    make_button(screen, "{label}", 208, 2, 60, 44, ACTION_AI);', 1)
                with self.assertRaises(policies.PolicyFailure):
                    policies.s3_product_ui(context)
        context.text_cache[path] = original


if __name__ == "__main__":
    unittest.main()
