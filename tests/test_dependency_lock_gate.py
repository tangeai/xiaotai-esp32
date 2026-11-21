"""Check product dependency locks at the actual build-entry checkpoints."""
# Replaces legacy: tests/test_dependency_lock_gate.py
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import host_components as hc
import xiaotai_build as build
import xiaotai_build_native as native


class DependencyLockGate(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.out = self.root / "build" / "fixture"
        self.out.mkdir(parents=True)
        self.source = self.root / "source.lock"
        self.effective = self.out / "dependencies.lock"
        self.lock = {
            "version": "3.0.0",
            "target": "esp32s3",
            "manifest_hash": "a" * 64,
            "direct_dependencies": ["espressif/mqtt"],
            "dependencies": {
                "espressif/mqtt": {
                    "version": "1.1.0",
                    "source": {"type": "service", "registry_url": "https://components.espressif.com/"},
                    "component_hash": "b" * 64,
                    "dependencies": [{"name": "idf", "version": ">=6.0"}],
                },
                "idf": {"version": "6.0.3", "source": {"type": "idf"}},
            },
        }
        self.source.write_text(json.dumps(self.lock), encoding="utf-8")

    def verify(self, value):
        self.effective.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
        build.verify_dependency_lock(self.source, self.effective)

    def test_manifest_refresh_does_not_change_dependency_identity(self):
        self.verify(dict(self.lock, manifest_hash="c" * 64))

    def test_target_source_version_content_and_graph_changes_are_rejected(self):
        for field, value in (
            ("version", "1.1.1"),
            ("source", {"type": "local", "path": "../../../vendor/mqtt"}),
            ("component_hash", "c" * 64),
            ("dependencies", []),
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(self.lock)
                changed["dependencies"]["espressif/mqtt"][field] = value
                with self.assertRaisesRegex(ValueError, "component espressif/mqtt"):
                    self.verify(changed)
        for field, value in (("target", "esp32p4"), ("direct_dependencies", []), ("version", "4.0.0")):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, f"field {field}"):
                self.verify(dict(self.lock, **{field: value}))
        missing = copy.deepcopy(self.lock)
        del missing["dependencies"]["idf"]
        with self.assertRaisesRegex(ValueError, "component idf"):
            self.verify(missing)

    def test_invalid_or_missing_effective_lock_is_not_success(self):
        with self.assertRaisesRegex(ValueError, "Cannot read dependency lock"):
            build.verify_dependency_lock(self.source, self.effective)
        self.effective.write_text("dependencies: [", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Cannot read dependency lock"):
            build.verify_dependency_lock(self.source, self.effective)
        with self.assertRaisesRegex(ValueError, "Invalid dependency lock"):
            self.verify(dict(self.lock, manifest_hash="invalid"))

    def test_idf_lines_resolve_the_same_component_versions(self):
        # Only the IDF entry and components that one IDF line provides itself may differ.
        default = ROOT / "profiles/dependency-locks"
        for other in sorted(default.glob("idf*/*.lock")):
            with self.subTest(lock=other.relative_to(ROOT).as_posix()):
                yaml = build.load_yaml()
                base = yaml.safe_load((default / other.name).read_text(encoding="utf-8"))["dependencies"]
                line = yaml.safe_load(other.read_text(encoding="utf-8"))["dependencies"]
                for name in sorted(set(base) & set(line) - {"idf"}):
                    if base[name]["source"].get("type") == "service":
                        self.assertEqual(line[name]["version"], base[name]["version"], name)
                    if base[name]["source"].get("type") == "git":
                        # Same commit, same files: the hash must not depend on who wrote the lock.
                        self.assertEqual(line[name]["source"], base[name]["source"], name)
                        self.assertEqual(line[name]["component_hash"], base[name]["component_hash"], name)

    def test_products_lock_one_version_per_component(self):
        # With the cross-line test above this covers every lock: one version everywhere.
        yaml = build.load_yaml()
        seen = {}
        for path in sorted((ROOT / "profiles/dependency-locks").glob("*.lock")):
            for name, entry in yaml.safe_load(path.read_text(encoding="utf-8"))["dependencies"].items():
                if entry["source"].get("type") != "service":
                    continue
                identity = (entry["version"], entry["component_hash"])
                first = seen.setdefault(name, (identity, path.name))
                self.assertEqual(identity, first[0], f"{name}: {path.name} differs from {first[1]}")

    def test_seeded_lock_is_loadable_by_every_component_manager(self):
        yaml = build.load_yaml()
        for path in sorted((ROOT / "profiles/dependency-locks").rglob("*.lock")):
            for reads_relative in (True, False):
                with self.subTest(lock=path.relative_to(ROOT).as_posix(), reads_relative=reads_relative), \
                        tempfile.TemporaryDirectory() as temporary, \
                        patch.object(build, "resolver_reads_relative_paths", return_value=reads_relative):
                    out = Path(temporary) / "build/product/firmware"
                    out.mkdir(parents=True)
                    seed = out / "dependencies.lock"
                    build.seed_lock(path, seed)
                    lock = yaml.safe_load(seed.read_text(encoding="utf-8"))
                    paths = [Path(source["path"]) for source in build.local_sources(lock)]
                    if reads_relative:
                        self.assertEqual(seed.read_bytes(), path.read_bytes())
                    else:
                        # Component manager 2.x has no lock directory to resolve against.
                        self.assertTrue(all(item.is_absolute() for item in paths), paths)
                    # The seed and the committed lock still name the same components.
                    build.verify_dependency_lock(path, seed)

    def test_write_lock_on_another_line_forces_a_solve(self):
        source = next((ROOT / "profiles/dependency-locks/idf5.5").glob("*.lock"))
        with tempfile.TemporaryDirectory() as temporary:
            seed = Path(temporary) / "dependencies.lock"
            build.seed_lock(source, seed, force_solve=True)
            lock = build.load_yaml().safe_load(seed.read_text(encoding="utf-8"))
            self.assertEqual(lock["manifest_hash"], "0" * 64)
            original = build.load_yaml().safe_load(source.read_text(encoding="utf-8"))
            self.assertEqual(lock["dependencies"].keys(), original["dependencies"].keys())

    def test_resolver_checks_out_git_components_with_lf(self):
        with patch.dict(build.os.environ, {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "user.name",
                                           "GIT_CONFIG_VALUE_0": "kept"}):
            environment = build.resolver_environment()
        self.assertEqual(environment["GIT_CONFIG_COUNT"], "3")
        self.assertEqual(environment["GIT_CONFIG_KEY_0"], "user.name")
        self.assertEqual((environment["GIT_CONFIG_KEY_1"], environment["GIT_CONFIG_VALUE_1"]), ("core.autocrlf", "false"))
        self.assertEqual((environment["GIT_CONFIG_KEY_2"], environment["GIT_CONFIG_VALUE_2"]), ("core.eol", "lf"))

    def test_repository_product_locks_are_valid_inputs(self):
        for path in (ROOT / "profiles/dependency-locks").rglob("*.lock"):
            with self.subTest(lock=path.name):
                build.verify_dependency_lock(path, path)

    def test_native_variant_keeps_registry_subset_identity(self):
        reduced = copy.deepcopy(self.lock)
        # The same registry pin may be transitive in the reduced graph.
        reduced["direct_dependencies"].clear()
        mqtt = reduced["dependencies"]["espressif/mqtt"]
        mqtt["source"]["registry_url"] = "https://components.espressif.com"
        self.effective.write_text(json.dumps(reduced), encoding="utf-8")
        native.verify_variant_lock(self.source, self.effective)

        without_mqtt = copy.deepcopy(reduced)
        del without_mqtt["dependencies"]["espressif/mqtt"]
        self.effective.write_text(json.dumps(without_mqtt), encoding="utf-8")
        native.verify_variant_lock(self.source, self.effective)

        for key, value in (
            ("version", "1.1.1"), ("component_hash", "c" * 64),
            ("source", {"type": "service", "registry_url": "https://other.test/"}),
            ("source", {"type": "local", "path": "../vendored/mqtt"}),
        ):
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(reduced)
                changed["dependencies"]["espressif/mqtt"][key] = value
                self.effective.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Component espressif/mqtt"):
                    native.verify_variant_lock(self.source, self.effective)
        reduced["dependencies"]["other/extra"] = mqtt
        self.effective.write_text(json.dumps(reduced), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Component other/extra"):
            native.verify_variant_lock(self.source, self.effective)
        del reduced["dependencies"]["idf"]
        self.effective.write_text(json.dumps(reduced), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "pinned IDF"):
            native.verify_variant_lock(self.source, self.effective)

    def test_native_default_options_and_reduced_locks(self):
        args = dict(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                    component_index=None, component_cache=None, resolve_only=False)
        board = build.select("lckfb/szpi-esp32s3")
        package = "profiles/dependency-locks/xiaotai-szpi-s3"
        plans = {
            variant: native.native_plan(argparse.Namespace(variant=variant, **args), board, {})[3]
            for variant in (None, "no-video", "diagnostics")
        }
        default, reduced, diagnostic = (plans[key] for key in (None, "no-video", "diagnostics"))
        self.assertEqual(default["dependency_lock"], package + "-native.lock")
        self.assertEqual(diagnostic["dependency_lock"], default["dependency_lock"])
        self.assertEqual(reduced["dependency_lock"], package + "--no-video.lock")
        self.assertEqual(reduced["product_dependency_lock"], default["dependency_lock"])
        self.assertEqual(len({row["build_dir"] for row in plans.values()}), 3)
        self.assertEqual(default["selection"]["components"], diagnostic["selection"]["components"])
        self.assertNotEqual(default["selection"]["options"], diagnostic["selection"]["options"])
        self.assertEqual(set(default["selection"]["components"]) - set(reduced["selection"]["components"]),
                         {"xiaotai_video", "xiaotai_mod_ai_vision", "xiaotai_mod_stream_video"})
        native.verify_variant_lock(ROOT / default["dependency_lock"], ROOT / reduced["dependency_lock"])
        is_file = Path.is_file
        with patch.object(Path, "is_file", autospec=True,
                          side_effect=lambda path: not path.name.endswith("--no-video.lock") and is_file(path)):
            with self.assertRaisesRegex(ValueError, "Missing locked native variant.*component-index.*offline"):
                native.native_plan(argparse.Namespace(variant="no-video", **args), board, {})
        with patch.object(Path, "is_file", autospec=True,
                          side_effect=lambda path: not path.name.endswith("-native.lock") and is_file(path)):
            for variant in ("diagnostics", "no-video"):
                with self.subTest(variant=variant), self.assertRaisesRegex(ValueError, "Missing committed SDK product lock"):
                    native.native_plan(argparse.Namespace(variant=variant, **args), board, {})
        for variant in (None, "diagnostics", "no-video"):
            with self.subTest(variant=variant), self.assertRaisesRegex(ValueError, "requires.*local index/cache"):
                native.native_plan(argparse.Namespace(variant=variant, **dict(args, write_lock=True,
                                                                             resolve_only=True)), board, {})

    def test_native_variant_lock_checked_before_and_after_configure_and_build(self):
        for reduced in (True, False):
            for stage in ("cached", "reconfigure", "build"):
                with self.subTest(reduced=reduced, stage=stage), tempfile.TemporaryDirectory() as directory:
                    self.check_native_drift(Path(directory), reduced, stage)

    def test_native_lock_maintenance_will_not_follow_existing_output_links(self):
        for alias in ("symlink", "hardlink", "directory"):
            with self.subTest(alias=alias), tempfile.TemporaryDirectory() as directory:
                self.check_native_drift(Path(directory), True, alias)

    def test_native_pin_replaced_after_final_validation_is_not_recorded(self):
        self.check_native_drift(self.root / "racing", True, "replace")

    def check_native_drift(self, root, reduced, stage):
        root.mkdir(exist_ok=True)
        out = root / "build"
        resolved = out / "project/dependencies.lock"
        base = root / "product-native.lock"
        base.write_bytes(self.source.read_bytes())
        pinned = root / "product--no-video.lock" if reduced else base
        if reduced:
            fixture = dict(self.lock, direct_dependencies=[])
            pinned.write_text(json.dumps(fixture), encoding="utf-8")
        original = pinned.read_bytes()
        changed = json.loads(original)
        changed["dependencies"]["espressif/mqtt"]["component_hash"] = "c" * 64
        drifted = json.dumps(changed).encode("utf-8")
        for path in (root / "profiles", root / "boards/fixture", root / "cmake/esp-idf-product/main",
                     resolved.parent, out / "generated"):
            path.mkdir(parents=True, exist_ok=True)
        for path in ("boards/fixture/partitions.csv", "profiles/fixture-native.yml"):
            (root / path).touch()
        archive = root / "libTiRTC.a"
        archive.write_bytes(b"fixture")
        contract = dict(sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), toolchain_release="test-release")
        info = dict(name="fixture", board="fixture", build_dir=str(out), dependency_lock=pinned.name,
                    product_dependency_lock=base.name, target="esp32s3", sdkconfig=str(out / "sdkconfig"),
                    idf_line="6.0", variant="no-video" if reduced else "diagnostics")
        actions = []
        updating = stage in {"symlink", "hardlink", "directory"}
        product_pin = base.read_bytes()
        if stage == "cached":
            resolved.write_bytes(drifted)
        elif updating:
            try:
                if stage == "hardlink":
                    os.link(base, resolved)
                elif stage == "symlink":
                    resolved.symlink_to(base)
                else:
                    resolved.parent.rmdir()
                    resolved.parent.symlink_to(root, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"Host cannot create the fixture link: {error}")
        elif stage == "replace":
            # Prepare the input paths needed after the final lock barrier.
            for path in ("boards/fixture/board.json", "boards/fixture/sdkconfig.board",
                         "products/fixture/product.json", "profiles/native-firmware/fixture.json",
                         "profiles/firmware-entry-defaults.json", "config/xiaotai.defaults", "fixture-contract.json",
                         *(f"scripts/{name}" for name in ("build.py", "xiaotai_build.py", "xiaotai_build_native.py",
                             "sdk_selection.py", "idf_environment.py", "flash_layout.py", "elf_static_memory.py"))):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).touch()
            (root / "boards/fixture/board.json").write_text('{"memory":{"external_ram":true}}', encoding="utf-8")
            for path in ("build.ninja", "compile_commands.json", "CMakeCache.txt", "config/sdkconfig.h",
                         "toolchain/cflags"):
                (out / path).parent.mkdir(parents=True, exist_ok=True)
                (out / path).touch()

        # The native builder now resolves the manifest and source paths by line.
        packages = root / "components/tirtc_sdk/packages.json"
        packages.parent.mkdir(parents=True, exist_ok=True)
        packages.write_text(json.dumps(build.sdk_lines()), encoding="utf-8")

        def run_tool(lock, argv, env):
            actions.append(argv[-1])
            (out / "project_description.json").write_text('{"c_compiler":"fixture"}', encoding="utf-8")
            Path(info["sdkconfig"]).touch()
            if actions[-1] == stage:
                resolved.write_bytes(drifted)

        verify_dependency_lock = build.verify_dependency_lock

        def compare(before, after):
            verify_dependency_lock(before, after)
            if stage == "replace" and actions == ["reconfigure", "build"]:
                candidate = root / "updater-resolved.lock"
                candidate.write_text(json.dumps(self.lock), encoding="utf-8")
                native.save_variant_lock(candidate, pinned)

        args = argparse.Namespace(component_index=None, write_lock=updating, resolve_only=updating)
        with (patch.object(native, "load_profile", return_value={"idf_version": "6.0.3"}),
              patch.object(native, "verify_idf", return_value={}),
              patch.object(native, "defaults", return_value={}),
              patch.object(native.sdk_selection, "write", return_value={}),
              patch.object(native.subprocess, "check_output", return_value="test-release"),
              patch.object(build, "run_tool", side_effect=run_tool),
              patch.object(build, "verify_dependency_lock", side_effect=compare),
              patch.object(build, "flash_layout", return_value={}),
              patch.object(build, "build_input_paths", return_value=set())):
            error = ("must not use links" if updating else "Dependency lock changed during" if stage == "replace"
                     else "Dependency lock differs.*espressif/mqtt")
            with self.assertRaisesRegex(ValueError, error):
                native.build(args, {}, {"lib": root, "key": "fixture", "contract": root / "fixture-contract.json",
                                       "defaults": root / "fixture-defaults"}, contract, info, root)
        expected = {"reconfigure": ["reconfigure"], "build": ["reconfigure", "build"],
                    "replace": ["reconfigure", "build"]}.get(stage, [])
        self.assertEqual(actions, expected)
        self.assertEqual(base.read_bytes(), product_pin)
        if stage != "replace":
            self.assertEqual(pinned.read_bytes(), original)
        self.assertFalse((out / "build-record.json").exists())
        if stage == "replace":
            self.assertFalse((out / "build-inputs.json").exists())
        self.assertFalse((out / ".xiaotai-build.lock").exists())


class HostIdfSelectionTest(unittest.TestCase):
    product = "xiaotai-fixture"
    component = "fixture/widget"
    roles = {"widget": (component, (product,))}

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def write(self, path, contents):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(contents), encoding="utf-8")
        return path

    def lock(self, path, version, digest):
        return self.write(path, {"dependencies": {self.component: {"version": version, "component_hash": digest}}})

    def resolve(self, line=None):
        return hc.resolve("widget", line=line, root=self.root, roles=self.roles)

    def test_optional_line_uses_its_native_profile_lock_and_default_is_unchanged(self):
        self.write(f"profiles/native-firmware/{self.product}.json", {})
        self.lock(f"profiles/dependency-locks/{self.product}-native.lock", "1.1.0", "b" * 64)
        self.write(f"profiles/native-firmware/idf5.5/{self.product}.json", {})
        pin = self.lock(f"profiles/dependency-locks/idf5.5/{self.product}-native.lock", "2.1.0", "d" * 64)

        with patch.dict(os.environ, {hc.LINE_VARIABLE: "5.5"}):
            identity = self.resolve()
            self.assertEqual((identity.version, identity.component_hash), ("2.1.0", "d" * 64))
            self.assertEqual(self.resolve("6.0").version, "1.1.0")
            pin.unlink()
            with self.assertRaisesRegex(hc.ComponentError, f"missing product dependency lock: .*{pin.name}"):
                self.resolve()

    def test_products_without_an_optional_line_profile_use_their_default_lock(self):
        self.write(f"profiles/native-firmware/{self.product}.json", {})
        self.lock(f"profiles/dependency-locks/{self.product}-native.lock", "1.1.0", "b" * 64)
        self.assertEqual(self.resolve("5.5").version, "1.1.0")


if __name__ == "__main__":
    unittest.main()
