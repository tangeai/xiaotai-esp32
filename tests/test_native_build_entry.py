"""Guard SDK and legacy firmware entry routing."""
# Replaces legacy: tests/test_build_profiles.py
# Replaces legacy: tests/test_product_contracts.py

import argparse
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
BOARD = "lckfb/szpi-esp32s3"
CORE = "m5stack/core-s3"
WAVESHARE = "waveshare/esp32-p4-wifi6-touch-lcd-3.5"
WT99 = "wt99p461-s1"
XMINI = "xmini/c3-v3"
sys.path.insert(0, str(ROOT / "scripts"))
import xiaotai_build_native as native
import product_scope  # noqa: E402

HAS_XMINI = product_scope.has_product(product_scope.XMINI)


def invoke(*args):
    return subprocess.run([sys.executable, "-B", str(ROOT / "scripts/build.py"), *args],
                          capture_output=True, text=True, check=False)


class NativeBuildEntryTest(unittest.TestCase):
    def test_only_validated_defaults_are_routed_native(self):
        items = json.loads(invoke("--list-boards", "--json").stdout)
        self.assertEqual(len(items), 4 + HAS_XMINI)
        for item in items:
            with self.subTest(board=item["board"]):
                args = [item["board"], "--name", item["name"], "--dry-run"]
                default = invoke(*args)
                explicit = invoke(*args, "--entry", "legacy")
                self.assertEqual(default.returncode, 0, default.stderr)
                # The legacy entry is gone; nothing falls back to it.
                self.assertNotEqual(explicit.returncode, 0)
                self.assertIn("no legacy entry", explicit.stderr)
                sdk = invoke(*args, "--entry", "sdk")
                self.assertEqual(sdk.returncode, 0, sdk.stderr)
                self.assertEqual(json.loads(default.stdout), json.loads(sdk.stdout))
                self.assertEqual(Path(json.loads(sdk.stdout)["build_dir"]).name, "sdk-firmware")

    def test_native_stages_only_board_owned_project_json(self):
        with TemporaryDirectory() as folder:
            root = Path(folder)
            board = root / "boards/vendor/test"
            board.mkdir(parents=True)
            source = board / "sensor.json"
            source.write_text('{"gain": 2}', encoding="ascii")
            outsider = root / "outside.json"
            outsider.write_text('{}', encoding="ascii")
            project = root / "project"
            project.mkdir()
            relative = source.relative_to(root).as_posix()
            paths = native.stage_project_assets(root, "vendor/test", {
                "project_assets": {"camera.json": relative},
            }, project)
            output = project / "camera.json"
            self.assertEqual(paths, {source.resolve(), output})
            self.assertEqual(output.read_bytes(), source.read_bytes())
            self.assertIn(project / relative, native.stage_project_assets(root, "vendor/test", {
                "project_assets": {relative: relative},
            }, project))
            self.assertEqual((project / relative).read_bytes(), source.read_bytes())
            for unsafe in ({"../escape.json": relative}, {"CMakeLists.txt": relative},
                           {"camera.json": outsider.name}, {"nested/camera.json": relative}):
                with self.assertRaisesRegex(ValueError, "Invalid project JSON asset"):
                    native.stage_project_assets(root, "vendor/test", {"project_assets": unsafe}, project)

    def test_native_network_defaults_apply_to_no_external_ram_only(self):
        keys = ("CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM", "CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM")
        ipv6 = "CONFIG_LWIP_IPV6"
        layer = ROOT / "config/capabilities/no-external-ram.defaults"
        for item in json.loads(invoke("--list-boards", "--json").stdout):
            if not native.profile_path(ROOT, item["name"]).is_file():
                continue  # Legacy board inventory also contains retired products.
            with self.subTest(board=item["board"]):
                args = [item["board"], "--name", item["name"], "--dry-run"]
                sdk = invoke(*args, "--entry", "sdk")
                self.assertEqual(sdk.returncode, 0, sdk.stderr)
                plan = json.loads(sdk.stdout)
                board = ROOT / "boards" / plan["selection"]["board"]
                manifest = json.loads((board / "board.json").read_text(encoding="utf-8"))
                board_values = native.common.read_config(board / "sdkconfig.board")
                self.assertEqual(layer in native.native_capability_layers(ROOT, board, board_values),
                                 not manifest["memory"]["external_ram"])
                if not manifest["memory"]["external_ram"]:
                    self.assertEqual([plan["config"].get(k) for k in keys], ["16", "16"])
                    self.assertEqual(plan["config"].get(ipv6), "n")
                else:
                    self.assertNotIn(ipv6, plan["config"])

    def test_local_locks_keep_relative_vendor_base_without_moving_registry_only_lock(self):
        out = ROOT / "build/test-sdk"
        self.assertEqual(native.resolved_lock_path(out, {}), out / "project/dependencies.lock")
        self.assertEqual(native.resolved_lock_path(out, {"vendor_hashes": {"some/vendor": "pinned"}}),
                         out / "dependencies.lock")

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_xmini_sdk_uses_pinned_60(self):
        args = (XMINI, "--entry", "sdk", "--dry-run")
        result = invoke(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        explicit = invoke(*args, "--idf", "6.0")
        self.assertEqual((explicit.returncode, explicit.stderr, explicit.stdout), (0, "", result.stdout))
        plan = json.loads(result.stdout)
        self.assertEqual((plan["idf_environment"]["version"], plan["tirtc_package"]),
                         ("6.0.2", "6.0/xmini-mini"))
        self.assertEqual(Path(plan["build_dir"]).name, "sdk-firmware")
        pin = ROOT / plan["dependency_lock"]
        self.assertEqual(pin.name, "xiaotai-xmini-c3-v3-native.lock")
        self.assertTrue(pin.is_file())
        yaml = native.common.load_yaml()
        manifest = ROOT / "profiles/xiaotai-xmini-c3-v3-native.yml"
        self.assertEqual(yaml.safe_load(manifest.read_text(encoding="utf-8"))["dependencies"]["idf"],
                         "==" + plan["idf_environment"]["version"])
        self.assertEqual(yaml.safe_load(pin.read_text(encoding="utf-8"))["dependencies"]["idf"]["version"],
                         plan["idf_environment"]["version"])
        diagnostics = invoke(*args, "--variant", "diagnostics")
        self.assertEqual(diagnostics.returncode, 0, diagnostics.stderr)
        diagnosed = json.loads(diagnostics.stdout)
        self.assertEqual(diagnosed["dependency_lock"], plan["dependency_lock"])
        self.assertEqual(Path(diagnosed["build_dir"]).name, "sdk-firmware--diagnostics")
        self.assertEqual(invoke(*args, "--idf", "5.5").returncode, 1)
        self.assertEqual(json.loads(invoke(XMINI, "--dry-run").stdout), plan)
        self.assertNotEqual(invoke(XMINI, "--entry", "legacy", "--dry-run").returncode, 0)
        self.assertNotIn("xmini-mini-v263", invoke("--list-idf").stdout)

    def test_native_only_uses_selected_szpi_graph_and_separate_output(self):
        result = invoke(BOARD, "--entry", "sdk", "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["entry"], "sdk")
        self.assertEqual(data["selection"]["board"], BOARD)
        self.assertEqual(data["idf_environment"]["version"], "6.0.3")
        self.assertTrue((ROOT / data["dependency_lock"]).is_file())
        self.assertEqual(Path(data["build_dir"]).name, "sdk-firmware")
        self.assertTrue(all(not path.startswith("main/") for path in data["selection"]["components"].values()))
        self.assertEqual(data["config"]["CONFIG_COMPILER_OPTIMIZATION_SIZE"], "y")
        self.assertEqual(data["config"]["CONFIG_XIAOTAI_PORT_HTTP_DEADLINE_TRANSPORT"], "y")
        self.assertEqual(invoke(BOARD, "--dry-run").stdout, result.stdout)

    def test_core_native_profile_uses_its_pinned_idf(self):
        result = invoke(CORE, "--entry", "sdk", "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["target"], "esp32s3")
        self.assertEqual(data["tirtc_package"], "6.0/generic-esp32s3")
        self.assertEqual(data["idf_environment"]["version"], "6.0.3")
        self.assertEqual(data["config"]["CONFIG_SPIRAM_XIP_FROM_PSRAM"], "n")
        self.assertNotIn("CONFIG_FREERTOS_IDLE_TASK_STACKSIZE", data["config"])
        self.assertNotIn("CONFIG_ESP_IPC_TASK_STACK_SIZE", data["config"])
        self.assertTrue((ROOT / data["dependency_lock"]).is_file())
        line55 = ("--idf", "5.5", "--dry-run")
        self.assertEqual(invoke(CORE, "--entry", "sdk", *line55).returncode, 0)
        self.assertEqual(invoke(CORE, *line55).stdout, invoke(CORE, "--entry", "sdk", *line55).stdout)
        self.assertNotEqual(invoke(CORE, "--entry", "legacy", *line55).returncode, 0)

    def test_retired_mz28_is_no_longer_a_board(self):
        listed = {item["board"] for item in json.loads(invoke("--list-boards", "--json").stdout)}
        self.assertNotIn("mz28-ai-ppd", listed)
        self.assertNotEqual(invoke("mz28-ai-ppd", "--dry-run").returncode, 0)

    def test_native_package_identity_must_match_the_locked_profile(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        other_package = native.common.sdk_package(native.common.select(WAVESHARE), "6.0")
        with patch.object(native.common, "sdk_package", return_value=other_package):
            with self.assertRaisesRegex(ValueError, "does not match the locked native profile"):
                native.native_plan(args, native.common.select(BOARD), {})

    def test_native_rejects_selected_board_with_wrong_target(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        board = native.common.select(BOARD)
        manifest = ROOT / "boards" / BOARD / "board.json"
        read_text = Path.read_text

        def wrong_target(path, *args, **kwargs):
            contents = read_text(path, *args, **kwargs)
            return json.dumps(dict(json.loads(contents), target="esp32c3")) if path == manifest else contents

        with patch.object(Path, "read_text", wrong_target):
            with self.assertRaisesRegex(ValueError, "selected board target esp32c3.*locked target esp32s3"):
                native.native_plan(args, board, {})

    def test_native_rejects_unvalidated_profiles_without_fallback(self):
        for flags in (("--stage", "sdk-check"), ("--write-lock",),
                      ("--fresh-config",), ("--build-options-json", '{"development_console":true}')):
            with self.subTest(flags=flags):
                result = invoke(BOARD, "--entry", "sdk", "--dry-run", *flags)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
        # No legacy fallback: these options are refused instead of silently building the old entry.
        self.assertNotEqual(invoke(BOARD, "--idf", "5.5", "--entry", "legacy", "--dry-run").returncode, 0)
        for flags in (("--stage", "sdk-check"), ("--write-lock",), ("--fresh-config",),
                      ("--build-options-json", '{"development_console":true}')):
            result = invoke(BOARD, "--dry-run", *flags)
            self.assertEqual((result.returncode, result.stdout), (1, ""))

    def test_explicit_diagnostics_needs_supported_primary_console(self):
        prefix = "CONFIG_ESP_CONSOLE_"
        supported = ("USB_SERIAL_JTAG", "UART_DEFAULT", "UART_CUSTOM")
        unsupported = ("NONE", "USB_CDC")
        primary = supported + unsupported
        read_config = native.common.read_config
        for board in (BOARD, *([XMINI] if HAS_XMINI else [])):
            selected = native.common.select(board)
            config_path = ROOT / "boards" / board / "sdkconfig.board"
            args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                      component_index=None, component_cache=None)
            modes = [(backend,) for backend in primary] + [
                ("SECONDARY_USB_SERIAL_JTAG",), ("UART_DEFAULT", "USB_SERIAL_JTAG"),
            ]
            for enabled in modes:
                with self.subTest(board=board, backends=enabled):
                    def override(path):
                        values = read_config(path)
                        if path == config_path:
                            values.update({prefix + key: "y" if key in enabled else "n" for key in primary})
                            values[prefix + "SECONDARY_USB_SERIAL_JTAG"] = (
                                "y" if "SECONDARY_USB_SERIAL_JTAG" in enabled else "n")
                        return values

                    with patch.object(native.common, "read_config", side_effect=override):
                        _, _, _, ordinary = native.native_plan(args, selected, {})
                        for backend in enabled:
                            self.assertEqual(ordinary["config"][prefix + backend], "y")
                        diagnostics = argparse.Namespace(**vars(args), variant="diagnostics")
                        if len(enabled) == 1 and enabled[0] in supported:
                            product, _, _, plan = native.native_plan(diagnostics, selected, {})
                            self.assertEqual(plan["variant"], "diagnostics")
                            self.assertIs(product["parameters"]["at"]["no_platform_console"], False)
                            self.assertEqual(plan["config"][prefix + enabled[0]], "y")
                        else:
                            with self.assertRaisesRegex(ValueError, r"diagnostics.*primary USB Serial/JTAG or UART"):
                                native.native_plan(diagnostics, selected, {})

    def test_diagnostics_rechecks_the_effective_primary_and_leaves_headless_variants_alone(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None, variant="diagnostics")
        product, _, _, plan = native.native_plan(args, native.common.select(BOARD), {})
        graph = plan["selection"]
        for config in ({}, {"CONFIG_ESP_CONSOLE_NONE": "n"}, {"CONFIG_ESP_CONSOLE_USB_CDC": "n"}):
            native.verify_diagnostics_console(product, args.variant, graph, config)
        for config in ({}, {"CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": "y"},
                       {"CONFIG_ESP_CONSOLE_USB_CDC": "y"}, {"CONFIG_ESP_CONSOLE_NONE": "y"}):
            with self.subTest(effective=config):
                with self.assertRaisesRegex(ValueError, r"diagnostics.*primary USB Serial/JTAG or UART"):
                    native.verify_diagnostics_console(product, args.variant, graph, config, effective=True)
        for backend in ("USB_SERIAL_JTAG", "UART_DEFAULT", "UART_CUSTOM"):
            native.verify_diagnostics_console(product, args.variant, graph, {
                f"CONFIG_ESP_CONSOLE_{backend}": "y", "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": "y",
            }, effective=True)

        with self.assertRaisesRegex(ValueError, "does not select the AT engine"):
            native.verify_diagnostics_console(product, args.variant, {"components": {}}, plan["config"])
        headless = {"name": "headless-fixture", "variants": {"diagnostics": {
            "parameters": {"at": {"no_platform_console": True}},
        }}}
        native.verify_diagnostics_console(headless, "diagnostics", {"components": {}}, {}, effective=True)

    def test_optional_offline_inputs_are_paired_and_fail_closed(self):
        for flag in ("--component-index", "--component-cache"):
            result = invoke(BOARD, flag, str(ROOT), "--dry-run")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must be supplied together", result.stderr)
        result = invoke(BOARD, "--component-index", str(ROOT), "--component-cache", str(ROOT), "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=ROOT, component_cache=ROOT)
        inputs = native.native_plan(args, native.common.select(BOARD), {})
        with patch.object(native.sys, "platform", "win32"):
            with self.assertRaisesRegex(ValueError, "offline builds inside a Linux network namespace"):
                native.build(args, *inputs)

    def test_installed_idf_is_checked_without_offline_flags_on_windows(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        inputs = native.native_plan(args, native.common.select(BOARD), {})
        with patch.object(native.sys, "platform", "win32"), \
                patch.object(native, "verify_idf", side_effect=ValueError("pinned IDF validation reached")):
            with self.assertRaisesRegex(ValueError, "pinned IDF validation reached"):
                native.build(args, *inputs)


class NativeIdf55EntryTest(unittest.TestCase):
    def test_color_sdk_entry_selects_the_separate_official_55_inputs(self):
        products = (
            (BOARD, "xiaotai-szpi-s3", "esp32s3", "6.0.3"),
            (CORE, "xiaotai-cores3-lite", "esp32s3", "6.0.3"),
            (WAVESHARE, "xiaotai-waveshare-p4-rev3", "esp32p4", "6.0.3"),
            (WT99, "xiaotai-wt99p461-s1-1v1", "esp32p4", "6.0.3"),
        )
        yaml = native.common.load_yaml()
        for board, product, target, version in products:
            with self.subTest(board=board):
                args = (board, "--dry-run")
                sdk55 = invoke(*args, "--idf", "5.5", "--entry", "sdk")
                sdk60 = invoke(*args, "--idf", "6.0", "--entry", "sdk")
                legacy55 = invoke(*args, "--idf", "5.5", "--entry", "legacy")
                for result in (sdk55, sdk60):
                    self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotEqual(legacy55.returncode, 0)
                self.assertEqual(invoke(*args).stdout, sdk60.stdout)
                self.assertEqual(invoke(*args, "--idf", "5.5").stdout, sdk55.stdout)

                plan, default = json.loads(sdk55.stdout), json.loads(sdk60.stdout)
                self.assertEqual((plan["entry"], plan["idf_line"], plan["target"], plan["name"]),
                                 ("sdk", "5.5", target, product))
                self.assertEqual(plan["idf_environment"]["version"], "5.5.5")
                self.assertEqual(default["idf_environment"]["version"], version)
                self.assertEqual(plan["tirtc_package"], f"5.5/generic-{target}")
                contract_path = ROOT / f"components/tirtc_sdk/contracts/5.5/{target}.json"
                contract = json.loads(contract_path.read_text(encoding="utf-8"))
                self.assertEqual((contract["origin"], contract["sdk_version"], contract["idf_version"]),
                                 ("official", "2.6.3", "5.5.5"))
                required_config = ("CONFIG_MBEDTLS_DES_C", "CONFIG_MBEDTLS_HARDWARE_SHA", "CONFIG_LIBC_NEWLIB")
                self.assertEqual([plan["config"].get(key) for key in required_config], ["y"] * len(required_config))
                self.assertEqual(plan["selection"], default["selection"])
                self.assertEqual(Path(plan["build_dir"]).name, "sdk-firmware-idf5.5")
                self.assertEqual(Path(default["build_dir"]).name, "sdk-firmware")

                lock = f"profiles/dependency-locks/idf5.5/{product}-native.lock"
                self.assertEqual(plan["dependency_lock"], lock)
                self.assertEqual(default["dependency_lock"], f"profiles/dependency-locks/{product}-native.lock")
                pinned = yaml.safe_load((ROOT / lock).read_text(encoding="utf-8"))
                self.assertEqual((pinned["target"], pinned["dependencies"]["idf"]["version"]), (target, "5.5.5"))
                manifest = ROOT / f"profiles/{product}-idf5.5-native.yml"
                direct = yaml.safe_load(manifest.read_text(encoding="utf-8"))["dependencies"]
                self.assertIn("espressif/mqtt", direct)
                for dependency, declaration in direct.items():
                    version = declaration["version"] if isinstance(declaration, dict) else declaration
                    self.assertEqual(version, "==" + pinned["dependencies"][dependency]["version"], dependency)
                profile = ROOT / f"profiles/native-firmware/idf5.5/{product}.json"
                self.assertEqual(native.profile_path(ROOT, product, line="5.5"), profile)
                self.assertEqual(json.loads(profile.read_text(encoding="utf-8"))["tirtc_package"], plan["tirtc_package"])

    def test_55_sdk_uses_only_locks_on_the_selected_line(self):
        flags = ("--idf", "5.5", "--entry", "sdk", "--dry-run")
        default = invoke(BOARD, *flags)
        diagnosed = invoke(BOARD, *flags, "--variant", "diagnostics")
        for result in (default, diagnosed):
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(diagnosed.stdout)["dependency_lock"], json.loads(default.stdout)["dependency_lock"])
        self.assertEqual(Path(json.loads(diagnosed.stdout)["build_dir"]).name, "sdk-firmware-idf5.5--diagnostics")
        for args in ((BOARD, "--variant", "no-video"), *([(XMINI,)] if HAS_XMINI else [])):
            with self.subTest(args=args):
                result = invoke(*args, *flags)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertEqual(result.stdout, "")
                self.assertIn("5.5", result.stderr)


if __name__ == "__main__":
    unittest.main()
