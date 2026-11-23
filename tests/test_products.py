# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_native_profiles.py
# Replaces legacy: tests/test_product_contracts.py
# Replaces legacy: tests/regression/test_voice_format.py
"""Check V3 declarations, then strictly select and compile products with a board package."""

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection as selection  # noqa: E402
import xiaotai_build_native as native  # noqa: E402
import product_scope  # noqa: E402

COMPATIBILITY_PRODUCTS = frozenset({
    "xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-waveshare-p4-rev3",
    "xiaotai-wt99p461-s1-1v1", "xiaotai-xmini-c3-v3",
})
RETIRED_PRODUCTS = frozenset({"xiaotai-mz28-ai-ppd"})
BOARD_IDENTITY_REFS = {
    "board_model": "@board.identity.profile_model", "chip_model": "@board.identity.profile_chip",
}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def optional_cases(root, product, result, plan):
    """Remove independently selectable modules without changing the board."""
    apps, modules = result["units"][:2]
    optional = {name for app in apps for name in app.optional}
    for name in sorted(optional.intersection(product.get("modules", []))):
        explicit = [unit for unit in product["modules"] if unit != name]
        # Some app-optional modules are still required by another selected
        # module or another app. Their grouped removal is a product variant.
        dependencies = {dep for unit in [*apps, *(unit for unit in modules if unit.name != name)]
                        for dep in unit.depends}
        if name in dependencies or any(name in group and not (set(group) - {name}).intersection(result["modules"])
                                       for unit in modules if unit.name != name for group in unit.one_of):
            continue
        probed = plan(dict(product, modules=explicit, parameters={}, options={},
                           priorities={}, preemptions=[]))
        selected = set(probed["modules"])
        if name in selected:
            raise AssertionError(f"{product['name']}: removal of {name} unexpectedly retained it")
        removed_sections = {entry.get("section", unit.name) for unit in modules if unit.name not in selected
                            for entry in unit.configure}
        removed_sections -= {entry.get("section", unit.name) for unit in [*apps, *probed["units"][1]]
                             for entry in unit.configure}
        options = selection.component_options(root, probed["components"])
        activities = probed["activities"]
        reduced = dict(product, modules=explicit,
                       options={option: value for option, value in product.get("options", {}).items()
                                if option in options},
                       parameters={section: values for section, values in product.get("parameters", {}).items()
                                   if section not in removed_sections},
                       priorities={activity: rank for activity, rank in product.get("priorities", {}).items()
                                   if activity in activities},
                       preemptions=[rule for rule in product.get("preemptions", [])
                                    if rule["activity"] in activities and rule["over"] in activities])
        yield name, reduced


def configuration_cases(root, product, result, plan):
    yield "default", product
    for variant in sorted(product.get("variants", {})):
        yield f"variant-{variant}", selection.apply_variant(product, variant)
    for module, reduced in optional_cases(root, product, result, plan):
        yield f"without-{module}", reduced


def board_manifest(root, product):
    return root / "boards" / product["board"] / "board.json"


def board_packages(root, products):
    ready, unresolved = [], []
    for product in products:
        path = board_manifest(root, product)
        try:
            path.lstat()
        except FileNotFoundError:
            unresolved.append(product)
        else:
            # An invalid manifest must go through the strict path and fail.
            ready.append(product)
    return ready, unresolved


def validate_product(root, product, scratch):
    name = product["name"]
    board_path = board_manifest(root, product)
    if not board_path.is_file():
        raise AssertionError(f"{name}: board manifest is not a readable file: {board_path}")
    board = read_json(board_path)
    if not isinstance(board, dict) or not isinstance(board.get("parts"), dict):
        raise AssertionError(f"{name}: missing board package with parts: {board_path}")
    def plan(config):
        return selection.plan(root, config)

    cases = list(configuration_cases(root, product, plan(product), plan))
    for label, configuration in cases:
        generated = scratch / name / label
        result = selection.write(root, configuration, generated)
        key = "CONFIG_XIAOTAI_FRAMEWORK_MAX_MODULES"
        options = dict(line.split("=", 1) for line in result["options"])
        if name == "xiaotai-xmini-c3-v3" and options.get("CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS") != "0":
            raise AssertionError(f"{name}/{label}: change Wi-Fi must wait for settings and HTTP without a timeout")
        capacity = configuration.get("options", {}).get("FRAMEWORK_MAX_MODULES")
        if capacity is None and board.get("memory", {}).get("external_ram") is False:
            capacity = max(8, len(result["modules"]))
        expected = None if capacity is None else str(capacity)
        if options.get(key) != expected:
            raise AssertionError(f"{name}/{label}: {key} expected {expected}, got {options.get(key)}")
        if name in COMPATIBILITY_PRODUCTS:
            if label == "default" and "room" in result["modules"]:
                legacy_alaw = name != "xiaotai-xmini-c3-v3"
                glue = (generated / "xiaotai_selected.c").read_text(encoding="utf-8")
                expected = "true" if legacy_alaw else "false"
                if f".accept_alaw_join_reply = {expected}" not in glue:
                    raise AssertionError(f"{name}: generated room audio alias policy differs from deployed behavior")
            lcd = name != "xiaotai-xmini-c3-v3"
            defaults = dict(line.split("=", 1) for line in result["sdkconfig"])
            for option in ("LV_COLOR_DEPTH_16", "LV_FONT_MONTSERRAT_48", "LV_USE_CUSTOM_MALLOC"):
                if defaults.get(f"CONFIG_{option}") != ("y" if lcd else None):
                    raise AssertionError(f"{name}/{label}: effective {option} no longer follows the LCD kit")
            if lcd and defaults.get("CONFIG_LV_USE_CLIB_MALLOC") == "y":
                raise AssertionError(f"{name}/{label}: libc allocator conflicts with LVGL BULK memory")
        includes = [generated, board_path.parent / "include", root / "sdk/xiaotai_port/host/include",
                    *(root / directory / "include" for directory in result["components"].values())]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only"]
        for path in includes:
            command.extend(("-I", str(path)))
        subprocess.run([*command, str(generated / "xiaotai_selected.c")], check=True)
    return len(cases)


def load_inventory(root, expected=None):
    expected = product_scope.expected(COMPATIBILITY_PRODUCTS, root) if expected is None else expected
    paths = sorted((root / "products").glob("*/product.json"))
    if not paths:
        raise AssertionError("no V3 product.json files; BI-2 product declarations are required")
    products = [selection.load_product(path) for path in paths]
    names = [product["name"] for product in products]
    if retired := RETIRED_PRODUCTS.intersection(names):
        raise AssertionError(f"retired SDK products must not have declarations: {sorted(retired)}")
    if len(names) != len(set(names)) or not expected.issubset(names):
        raise AssertionError(f"expected every existing product once; missing={sorted(expected - set(names))}, "
                             f"manifests={len(names)}")
    for path, product in zip(paths, products):
        if path.parent.name != product["name"]:
            raise AssertionError(f"{path}: directory must match product name {product['name']}")
    return products


def declaration_plan(root, catalog, product):
    """Check only facts known from the product and selected component manifests."""
    name = product["name"]
    apps, modules, _, selected = selection.resolve(catalog, product.get("applications", []),
                                                   product.get("modules", []))
    units = modules + apps
    components = {unit.component: unit.path.relative_to(root).as_posix() for unit in units}
    components.update({name: f"sdk/{name}" for name in selection.BASE_COMPONENTS})
    selection._options(root, product, components, units)
    selection.check_theme_parameters(units, product, {})
    selection.check_absent_with(units, product)
    selection.vocabulary(units)
    activities = selection.activity_defaults(units)
    declared = set(product.get("priorities", {}))
    for rule in product.get("preemptions", []):
        declared.update((rule["activity"], rule["over"]))
    if declared - activities.keys():
        raise AssertionError(f"{name}: unknown activity: {sorted(declared - activities.keys())}")

    return {"units": (apps, modules), "modules": sorted(selected), "activities": activities,
            "components": components}


def compile_parameters(root, product, result, scratch, label):
    name = product["name"]
    apps, modules = result["units"]
    # Compiling field types and array/pointer initializers needs no board.
    # The board owns the strings. In this syntax-only probe, use unresolved
    # typed symbols instead of inventing an identity or supplying a board.
    params = dict(product.get("parameters", {}))
    platform = dict(params.get("platform", {}))
    symbols = []
    for field, reference in BOARD_IDENTITY_REFS.items():
        if platform.get(field) == reference:
            platform[field] = f"XIAOTAI_UNRESOLVED_{field.upper()}"
            symbols.append(f"extern const char {platform[field]}[];\n")
    if "platform" in params:
        params["platform"] = platform
    glue, headers, calls = selection._glue(apps, modules, dict(product, parameters=params))
    source = scratch / f"{name}-{label}.c"
    include = "".join(f'#include "{header}"\n' for header in headers)
    body = "".join(call + "    if (err != XIAOTAI_OK) return err;\n" for call in calls)
    source.write_text('#include <stddef.h>\n#include <string.h>\n#include "xiaotai/xiaotai.h"\n' + include +
                      "".join(symbols + glue.objects) +
                      "xiaotai_err_t declaration_probe(const xiaotai_config_t *config) {\n"
                      "    (void)config;\n    xiaotai_err_t err = XIAOTAI_OK;\n" + body +
                      "    return err;\n}\n", encoding="utf-8")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only"]
    for path in [root / "sdk/xiaotai_port/host/include",
                 *(root / directory / "include" for directory in result["components"].values())]:
        command.extend(("-I", str(path)))
    subprocess.run([*command, str(source)], check=True)
    return {unit.name for unit in [*apps, *modules]}


def validate_declaration(root, catalog, product, scratch):
    def plan(config):
        return declaration_plan(root, catalog, config)

    baseline = plan(product)
    cases = list(configuration_cases(root, product, baseline, plan))
    for label, config in cases:
        selected = compile_parameters(root, config, plan(config), scratch, label)
        if label == "variant-no-video" and selected & {"ai_vision", "stream_video", "video"}:
            raise AssertionError("S3 no-video must not keep the video engine")
        if label == "variant-locked" and any(config["options"][option] for option in
                ("UI_CAPTURE", "BINDING_DEV_COMMANDS", "NET_CONSOLE_FORGET")):
            raise AssertionError("CoreS3 locked must close all three development controls")
    return len(cases)


class InventoryTests(unittest.TestCase):
    def fixture(self):
        directory = tempfile.TemporaryDirectory(prefix="xiaotai-product-inventory-")
        self.addCleanup(directory.cleanup)
        return Path(directory.name)

    def populate(self, root, path, value):
        path = root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def test_missing_inventory_fails_the_declaration_check(self):
        root = self.fixture()
        with self.assertRaisesRegex(AssertionError, "BI-2"):
            load_inventory(root)

    def test_compact_module_order_size_follows_kconfig_bounds(self):
        product = {"name": "test"}
        framework = {"xiaotai_framework": "sdk/xiaotai_framework"}
        board = {"memory": {"external_ram": False}}
        module = selection.Unit("engine", "test", "test", ROOT)
        app = selection.Unit("application", "app", "app", ROOT)
        key = "CONFIG_XIAOTAI_FRAMEWORK_MAX_MODULES"
        self.assertIn(f"{key}=8", selection._options(ROOT, product, framework, [app], board))
        self.assertIn(f"{key}=255", selection._options(ROOT, product, framework, [module] * 255 + [app], board))
        with self.assertRaisesRegex(selection.SelectionError, "256 selected units"):
            selection._options(ROOT, product, framework, [module] * 256, board)
        product["options"] = {"FRAMEWORK_MAX_MODULES": 32}
        self.assertIn(f"{key}=32", selection._options(ROOT, product, framework, [module] * 10, board))

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_compact_development_console_and_diagnostics_override(self):
        scratch = self.fixture()
        name = "xiaotai-xmini-c3-v3"
        product = selection.load_product(ROOT / "products" / name / "product.json")
        diagnostic = selection.apply_variant(product, "diagnostics")
        compact = selection.write(ROOT, product, scratch / "default")
        full = selection.write(ROOT, diagnostic, scratch / "diagnostics")
        self.assertEqual(compact["selected"], full["selected"])
        self.assertEqual(compact["components"], full["components"])
        self.assertEqual(compact["activities"], full["activities"])
        self.assertEqual(compact["commands"], full["commands"])  # Includes option-tagged spellings.
        self.assertEqual(compact["tirtc"], full["tirtc"])
        self.assertIn(".no_platform_console = true",
                      (scratch / "default" / "xiaotai_selected.c").read_text(encoding="utf-8"))
        self.assertIn(".omit_command_tables = true",
                      (scratch / "default" / "xiaotai_selected.c").read_text(encoding="utf-8"))
        self.assertIn(".no_platform_console = false",
                      (scratch / "diagnostics" / "xiaotai_selected.c").read_text(encoding="utf-8"))
        self.assertNotIn(".omit_command_tables = true",
                         (scratch / "diagnostics" / "xiaotai_selected.c").read_text(encoding="utf-8"))
        a = product["parameters"]["at"]
        a["omit_command_tables"] = False
        selection.write(ROOT, product, scratch / "local")
        self.assertIn(".omit_command_tables = false",
                      (scratch / "local" / "xiaotai_selected.c").read_text(encoding="utf-8"))
        a.update(omit_command_tables=True, no_platform_console=False)
        with self.assertRaisesRegex(selection.SelectionError, "omit_command_tables"):
            selection.write(ROOT, product, scratch / "invalid")
        for option in ("UI_CAPTURE", "MEDIA_TRACE", "MEDIA_TEST_COMMANDS", "MEDIA_SOUND_COMMANDS"):
            self.assertIn(f"CONFIG_XIAOTAI_{option}=n", compact["options"])
            self.assertIn(f"CONFIG_XIAOTAI_{option}=y", full["options"])

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_binding_ignores_unusable_grants_only_in_xmini(self):
        scratch = self.fixture()
        for product in load_inventory(ROOT):
            name = product["name"]
            if name not in COMPATIBILITY_PRODUCTS:
                continue
            with self.subTest(product=name):
                output = scratch / name
                selection.write(ROOT, product, output)
                generated = (output / "xiaotai_selected.c").read_text(encoding="utf-8")
                configs = re.findall(r"^static xiaotai_binding_config_t const .*;$", generated, re.M)
                if name == "xiaotai-xmini-c3-v3":
                    self.assertEqual(len(configs), 1)
                    self.assertIn(".ignore_unusable_grants = true", configs[0])
                else:
                    self.assertFalse(any(".ignore_unusable_grants = true" in config for config in configs))

    def test_vendor_idle_stack_default_follows_board_memory(self):
        scratch = self.fixture()
        sized = set()
        for product in load_inventory(ROOT):
            name = product["name"]
            with self.subTest(product=name):
                output = scratch / name
                selection.write(ROOT, product, output)
                generated = (output / "xiaotai_selected.c").read_text(encoding="utf-8")
                sdkconfig = (output / "sdkconfig.selection").read_text(encoding="utf-8").splitlines()
                board = read_json(board_manifest(ROOT, product))
                if board["memory"]["external_ram"]:
                    self.assertNotIn(".vendor_idle_stack_bytes =", generated)
                    self.assertNotIn("CONFIG_XIAOTAI_TIRTC_SESSION_STACK_9216=y", sdkconfig)
                else:
                    self.assertIn(".vendor_idle_stack_bytes = 4096", generated)
                    self.assertIn("CONFIG_XIAOTAI_TIRTC_SESSION_STACK_9216=y", sdkconfig)
                    sized.add(name)
        self.assertEqual(sized, set(product_scope.present(["xiaotai-xmini-c3-v3"])))

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_vendor_session_stack_override(self):
        name = "xiaotai-xmini-c3-v3"
        product = read_json(ROOT / "products" / name / "product.json")
        output = self.fixture() / name
        option = "TIRTC_SESSION_STACK_9216"
        options = {**product.get("options", {}), option: False}
        selection.write(ROOT, dict(product, options=options), output)
        sdkconfig = (output / "sdkconfig.selection").read_text(encoding="utf-8").splitlines()
        self.assertIn(f"CONFIG_XIAOTAI_{option}=n", sdkconfig)
        options[option] = 1
        with self.assertRaisesRegex(selection.SelectionError, f"{option}: expected a boolean"):
            selection.write(ROOT, dict(product, options=options), output)

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_vendor_idle_stack_override_and_validation(self):
        name = "xiaotai-xmini-c3-v3"
        product = read_json(ROOT / "products" / name / "product.json")
        original = deepcopy(product)
        apps, modules, board = selection.plan(ROOT, product)["units"]
        for capability in (True, None):
            alternate = dict(board, memory={"external_ram": capability} if capability is not None else {})
            with self.subTest(external_ram=capability):
                generated = selection.generate(apps, modules, product, alternate)
                self.assertNotIn(".vendor_idle_stack_bytes =", generated)
        self.assertEqual(product, original)

        def configured(size):
            parameters = deepcopy(product["parameters"])
            parameters["transport"]["vendor_idle_stack_bytes"] = size
            return selection.generate(apps, modules, dict(product, parameters=parameters), board)

        for size in (0, 4096, 6144, 8192):
            with self.subTest(valid=size):
                generated = configured(size)
                self.assertRegex(generated, rf"\.vendor_idle_stack_bytes = {size}(?:[, }}])")
        for size in (-1, 4095, 4097, 8208, "4096", 4096.0, True, None):
            with self.subTest(invalid=size):
                with self.assertRaisesRegex(selection.SelectionError, r"parameters\.transport\.vendor_idle_stack_bytes"):
                    configured(size)

    def test_product_inventory_validates_ids(self):
        root = self.fixture()
        expected = {"a", "b"}
        product = {"schema": 1, "name": "a", "board": "vendor/board", "applications": ["clock"]}
        self.populate(root, "products/a/product.json", product)
        with self.assertRaisesRegex(AssertionError, "missing=.*b"):
            load_inventory(root, expected=expected)
        self.populate(root, "products/b/product.json", dict(product, name="b"))
        self.assertEqual([p["name"] for p in load_inventory(root, expected)], ["a", "b"])
        self.populate(root, "products/c/product.json", dict(product, name="c"))
        self.assertEqual([p["name"] for p in load_inventory(root, expected)], ["a", "b", "c"])
        self.populate(root, "products/b/product.json", product)
        with self.assertRaisesRegex(AssertionError, "expected every existing product once"):
            load_inventory(root, expected=expected)

    def test_retired_products_are_gone(self):
        active = {product["name"] for product in load_inventory(ROOT)}
        self.assertTrue(RETIRED_PRODUCTS.isdisjoint(active))
        # Their sources stay at the commit recorded in tests/migration_inventory.json.
        for name in RETIRED_PRODUCTS:
            with self.subTest(product=name):
                self.assertFalse(native.profile_path(ROOT, name).exists())
                self.assertFalse(native.dependency_lock_path(ROOT, name).exists())

    def test_hardware_product_matrix_uses_selected_board_parts_and_product_sources(self):
        products = {p["name"]: p for p in load_inventory(ROOT) if p["name"] in COMPATIBILITY_PRODUCTS}
        original = {name: selection.plan(ROOT, product) for name, product in products.items()}
        lcd320 = {"xiaotai-szpi-s3", "xiaotai-cores3-lite"}
        lcd480 = {"xiaotai-waveshare-p4-rev3", "xiaotai-wt99p461-s1-1v1"}
        oled = "xiaotai-xmini-c3-v3"
        scratch = self.fixture()
        accepted = set()

        # The LCD themes accept either board in their display family. Firmware
        # entry remains locked to the shipped board, even when assembly works.
        for board_name, board_product in products.items():
            for name, product in products.items():
                with self.subTest(board=board_name, product=name):
                    board_id = board_product["board"]
                    output = scratch / board_name / name
                    candidate = dict(product, board=board_id)
                    compatible = (name == board_name or
                                  {name, board_name} <= lcd320 or {name, board_name} <= lcd480)
                    if not compatible:
                        # Applications are checked before engines: a product with the door
                        # application fails on a board without touch for that reason first.
                        if "door" in product.get("applications", []) and board_name == oled:
                            failure = r"application door needs .*touch"
                        elif name in lcd320 | lcd480 and board_name == oled:
                            failure = r"engine video needs .*camera"
                        else:
                            failure = r"theme .* draws for none of the board's displays"
                        with self.assertRaisesRegex(selection.SelectionError, failure):
                            selection.write(ROOT, candidate, output)
                        self.assertFalse(output.exists())
                        continue

                    result = selection.write(ROOT, candidate, output)
                    accepted.add((board_name, name))
                    self.assertEqual((result["board"], result["product"], result["target"]),
                                     (board_id, name, original[board_name]["target"]))
                    self.assertEqual(result["needs"], original[board_name]["needs"])
                    for key in ("applications", "modules", "activities", "commands", "tirtc"):
                        self.assertEqual(result[key], original[name][key], key)
                    apps, modules, _ = original[name]["units"]
                    hardware = original[board_name]["units"][2]
                    sources = {unit.component: unit.path.relative_to(ROOT).as_posix() for unit in apps + modules}
                    for component in set(selection.BASE_COMPONENTS) | set(hardware.get("components", [])):
                        sources.setdefault(component, original[board_name]["components"][component])
                    self.assertEqual(result["components"], sources)
                    cmake = (output / "selection.cmake").read_text(encoding="utf-8")
                    self.assertIn(f'set(XIAOTAI_BOARD "{board_id}")', cmake)
                    dirs = ";".join(result["components"].values())
                    self.assertIn(f'set(XIAOTAI_SELECTED_COMPONENT_DIRS "{dirs}")', cmake)

        # Each product on its own board, plus both pairs of the two LCD families.
        self.assertEqual(len(accepted), len(products) + 4)

    def test_native_profiles_reject_even_compatible_board_substitutions(self):
        products = {p["name"]: p for p in load_inventory(ROOT) if p["name"] in COMPATIBILITY_PRODUCTS}
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        for name, product in products.items():
            profile = native.load_profile(name, ROOT)
            cli_board = native.common.select(profile.get("cli_board", profile["board"]), name, ROOT)
            with patch.dict(os.environ, {}, clear=True):
                args.idf = profile["idf_line"]
                _, _, _, info = native.native_plan(args, cli_board, {}, ROOT)
                self.assertEqual((info["board"], info["name"], info["target"]),
                                 (product["board"], name, profile["target"]))
                self.assertTrue((ROOT / info["dependency_lock"]).is_file())
                for other in products.values():
                    if other["board"] == product["board"]:
                        continue
                    candidate = dict(product, board=other["board"])
                    try:
                        selection.plan(ROOT, candidate)
                    except selection.SelectionError:
                        continue  # Already rejected by the independent assembly matrix above.
                    with self.subTest(product=name, board=other["board"]), \
                            patch.object(selection, "load_product", return_value=candidate):
                        with self.assertRaisesRegex(ValueError, "do not match the supported entry"):
                            native.native_plan(args, cli_board, {}, ROOT)

    def test_headless_composition_with_explicit_input_and_feedback(self):
        product = {"name": "headless-fixture", "board": "fixture/future", "applications": ["companion"],
                   "modules": ["wechat"], "needs_all": ["pcm_in", "light"],
                   "needs_any": ["key", "encoder"]}
        parts = {"audio_input": [{"encoding": "pcm16"}], "audio_output": [{}],
                 "output": [{"kind": "rgb_light"}], "network": [{"kind": "wifi"}]}
        board = {"schema": 3, "target": "fixture-chip", "parts": parts}
        for kind, need in (("button", "key"), ("encoder", "encoder")):
            with self.subTest(input=kind):
                with_input = dict(board, parts=dict(parts, input=[{"kind": kind}]))
                result = selection.plan(ROOT, product, with_input)
                self.assertEqual(result["target"], "fixture-chip")
                self.assertIn("wechat", result["modules"])
                self.assertIn(need, result["needs"])
                self.assertNotIn("display", result["needs"])
        with self.assertRaisesRegex(selection.SelectionError, "needs one of"):
            selection.plan(ROOT, product, board)
        for port, missing in (("output", "light"), ("audio_output", "audio_out")):
            with self.subTest(port=port):
                changed = dict(parts, input=[{"kind": "button"}])
                del changed[port]
                with self.assertRaisesRegex(selection.SelectionError, rf"needs .*'{missing}'"):
                    selection.plan(ROOT, product, dict(board, parts=changed))
        encoded = dict(parts, input=[{"kind": "button"}], audio_input=[{"encoding": "opus"}])
        with self.assertRaisesRegex(selection.SelectionError, r"needs .*'pcm_in'"):
            selection.plan(ROOT, product, dict(board, parts=encoded))

    def test_missing_board_parts_is_a_failure_before_generating(self):
        root = self.fixture()
        product = {"name": "demo", "board": "vendor/board"}
        generated = root / "generated"
        self.assertEqual(board_packages(root, [product]), ([], [product]))
        with self.assertRaisesRegex(selection.SelectionError, "missing manifest"):
            selection.write(root, product, generated)
        for malformed in ({}, {"parts": []}, []):
            self.populate(root, "boards/vendor/board/board.json", malformed)
            self.assertEqual(board_packages(root, [product]), ([product], []))
            with self.assertRaisesRegex(AssertionError, "missing board package with parts"):
                validate_product(root, product, generated)
        board_manifest(root, product).write_text("{", encoding="utf-8")
        self.assertEqual(board_packages(root, [product]), ([product], []))
        with self.assertRaises(json.JSONDecodeError):
            validate_product(root, product, generated)
        self.assertFalse(generated.exists())

    def test_manifest_types_and_paths_fail_before_generating(self):
        root = self.fixture()
        valid = {"schema": 1, "name": "demo", "board": "vendor/board", "applications": ["clock"]}
        path = "products/demo/product.json"
        for field, value, reason in (
            ("schema", True, "schema"), ("schema", 1.0, "schema"), ("schema", "1", "schema"),
            ("name", 1, "a name"), ("board", "../../../../other-project", "a board"),
            ("board", "vendor/board\n", "a board"),
        ):
            with self.subTest(field=field, value=value):
                self.populate(root, path, dict(valid, **{field: value}))
                with self.assertRaisesRegex(selection.SelectionError, reason):
                    selection.load_product(root / path)
        generated = root / "generated"
        with self.assertRaisesRegex(selection.SelectionError, "a board"):
            selection.write(root, dict(valid, board="../../outside"), generated)
        self.assertFalse(generated.exists())

    @unittest.skipUnless(os.name == "posix", "the fixture needs unprivileged symlinks")
    def test_selected_board_file_stays_in_repository(self):
        root = self.fixture()
        outside = self.fixture()
        self.populate(outside, "board.json", {"schema": 3, "parts": {}})
        manifest = root / "boards/vendor/board/board.json"
        manifest.parent.mkdir(parents=True)
        manifest.symlink_to(outside / "board.json")
        generated = root / "generated"
        product = {"name": "demo", "board": "vendor/board"}
        with self.assertRaisesRegex(selection.SelectionError, "manifest is outside the repository"):
            selection.write(root, product, generated)
        self.assertFalse(generated.exists())

    def test_board_schema_and_geometry_fail_before_generating(self):
        root = self.fixture()
        for name in selection.BASE_COMPONENTS:
            (root / "sdk" / name).mkdir(parents=True)
        product = {"name": "demo", "board": "vendor/board"}
        path = "boards/vendor/board/board.json"
        generated = root / "generated"
        panel = {"pixel": "mono1_vlsb", "width": 128, "height": 64}
        identity = {"model": "demo-board", "revision": "V1"}
        self.populate(root, path, {"schema": 3, "identity": identity, "parts": {"display": [panel]}})
        self.assertEqual(selection.write(root, product, root / "valid")["needs"], ["display"])
        for bad in (True, 2, 3.0, "3"):
            with self.subTest(schema=bad):
                self.populate(root, path, {"schema": bad, "identity": identity, "parts": {"display": [panel]}})
                with self.assertRaisesRegex(selection.SelectionError, "schema 3"):
                    selection.write(root, product, generated)
                with self.assertRaisesRegex(selection.SelectionError, "schema 3"):
                    selection.board_header(root / path)
        for key, value in (("width", "128"), ("width", True), ("height", -1), ("height", 0), ("touch", 1)):
            with self.subTest(field=key, value=value):
                changed = dict(panel, **{key: value})
                self.populate(root, path, {"schema": 3, "identity": identity, "parts": {"display": [changed]}})
                expected = rf"parts\.display\[0\]\.{key}"
                with self.assertRaisesRegex(selection.SelectionError, expected):
                    selection.write(root, product, generated)
                with self.assertRaisesRegex(selection.SelectionError, expected):
                    selection.board_header(root / path)
        self.assertFalse(generated.exists())

    def test_optional_cases_keep_required_edges_and_remove_inactive_ranks(self):
        root = self.fixture()
        for name in selection.BASE_COMPONENTS:
            (root / "sdk" / name).mkdir(parents=True)
        self.populate(root, "boards/vendor/board/board.json", {"schema": 3, "parts": {}})
        self.populate(root, "apps/one/application.json",
                      {"name": "one", "modules": ["parent"], "optional_modules": ["call", "extra"]})
        self.populate(root, "sdk/modules/parent/module.json", {"name": "parent", "depends": ["call"]})
        self.populate(root, "sdk/modules/call/module.json", {"name": "call", "activities": {"call": 3}})
        self.populate(root, "sdk/modules/extra/module.json", {"name": "extra", "activities": {"extra": 2}})
        (root / "sdk/modules/call/Kconfig").write_text("config XIAOTAI_CALL_DEBUG\n    bool\n", encoding="utf-8")
        (root / "sdk/modules/extra/Kconfig").write_text("config XIAOTAI_EXTRA_LOG\n    bool\n", encoding="utf-8")
        product = {"name": "demo", "board": "vendor/board", "applications": ["one"], "modules": ["call", "extra"],
                   "priorities": {"call": 4, "extra": 1},
                   "options": {"CALL_DEBUG": True, "EXTRA_LOG": False},
                   "preemptions": [{"activity": "call", "over": "extra", "preempts": True}]}
        def plan(config):
            return selection.plan(root, config)
        [(name, reduced)] = optional_cases(root, product, plan(product), plan)
        self.assertEqual(name, "extra")
        self.assertEqual(reduced["priorities"], {"call": 4})
        self.assertEqual(reduced["preemptions"], [])
        self.assertEqual(reduced["options"], {"CALL_DEBUG": True})
        self.assertEqual(selection.plan(root, reduced)["modules"], ["call", "parent"])


def check_products(root, require_boards):
    products = load_inventory(root)
    ready, unresolved = board_packages(root, products)
    catalog = selection.catalog(root)
    with tempfile.TemporaryDirectory(prefix="xiaotai-product-check-") as directory:
        scratch = Path(directory)
        resolved_cases = sum(validate_product(root, product, scratch) for product in ready)
        declaration_cases = sum(validate_declaration(root, catalog, product, scratch) for product in unresolved)
    if ready:
        print(f"PASS: generated glue for {len(ready)} products and {resolved_cases} selections compiled")
    if unresolved:
        print(f"PASS: {len(unresolved)} product declarations and {declaration_cases} typed configurations")
        for product in unresolved:
            print(f"UNRESOLVED_BOARD: {product['name']} -> {board_manifest(root, product).relative_to(root).as_posix()}")
        if require_boards:
            raise AssertionError(f"strict product gate needs all boards ({len(unresolved)} unresolved)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-products", action="store_true",
                        help="require a board package and full selection/glue for every product")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InventoryTests)
    if not unittest.TextTestRunner().run(suite).wasSuccessful():
        return 1
    try:
        check_products(ROOT, args.require_products)
        return 0
    except (selection.SelectionError, AssertionError, OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
