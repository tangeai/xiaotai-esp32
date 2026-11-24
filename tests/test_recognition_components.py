"""Check the actual native component registrations, not a second source list."""
# Replaces legacy: tests/test_recognition_components.py
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection as selection

FIELDS = ("SRCS", "INCLUDE_DIRS", "REQUIRES", "PRIV_REQUIRES")


def registration(folder, *, sdk=False):
    with tempfile.TemporaryDirectory(prefix="recognition-components-") as directory:
        output = Path(directory)
        script = output / "inspect.cmake"
        script.write_text('''cmake_minimum_required(VERSION 3.16)
set(CMAKE_BUILD_EARLY_EXPANSION TRUE)
function(idf_component_register)
    cmake_parse_arguments(R "" "" "SRCS;INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
    foreach(field SRCS INCLUDE_DIRS REQUIRES PRIV_REQUIRES)
        file(WRITE "${OUTPUT}/${field}.txt" "${R_${field}}")
    endforeach()
endfunction()
include("${COMPONENT}/CMakeLists.txt")
''', encoding="utf-8")
        subprocess.run(["cmake", f"-DESP_PLATFORM={'ON' if sdk else 'OFF'}", f"-DCOMPONENT={folder.as_posix()}",
                        f"-DOUTPUT={output.as_posix()}", "-P", str(script)], check=True)
        return {field: [value for value in (output / f"{field}.txt").read_text().split(";")
                        if value] for field in FIELDS}


def own_paths(folder, recipe):
    for field in ("SRCS", "INCLUDE_DIRS"):
        for relative in recipe[field]:
            path = (folder / relative).resolve()
            if not path.is_relative_to(folder.resolve()) or not path.exists():
                raise ValueError(f"{field} escapes component or does not exist: {path}")


class SDKRecognitionComponents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        catalog = selection.catalog(ROOT)
        cls.units = {name: catalog["module"][name] for name in ("recognition", "recognition_config")}
        cls.recipes = {name: registration(unit.path, sdk=True) for name, unit in cls.units.items()}

    def test_sdk_sources_and_exports_stay_with_their_owner(self):
        sources = []
        for name, unit in self.units.items():
            own_paths(unit.path, self.recipes[name])
            sources += [(unit.path / source).resolve() for source in self.recipes[name]["SRCS"]]
        self.assertEqual(len(sources), len(set(sources)))
        self.assertIn("src/recognition.c", self.recipes["recognition"]["SRCS"])
        self.assertIn("src/recognition_journal.c", self.recipes["recognition"]["SRCS"])
        self.assertIn("src/recognition_config.c", self.recipes["recognition_config"]["SRCS"])
        recipe = dict(self.recipes["recognition_config"],
                      SRCS=["../xiaotai_mod_recognition/src/recognition_journal.c"])
        with self.assertRaisesRegex(ValueError, "escapes component"):
            own_paths(self.units["recognition_config"].path, recipe)

    def test_sdk_dependencies_follow_manifest_and_service_ownership(self):
        depends = {name: set(recipe["REQUIRES"] + recipe["PRIV_REQUIRES"])
                   for name, recipe in self.recipes.items()}
        catalog = selection.catalog(ROOT)["module"]
        for name, unit in self.units.items():
            with self.subTest(component=name):
                expected = {catalog[dependency].component for dependency in unit.depends}
                self.assertLessEqual(expected, depends[name])
        self.assertTrue({"xiaotai_platform", "xiaotai_media", "xiaotai_mod_recognition_config",
                         "xiaotai_port", "xiaotai_json"} <= depends["recognition"])
        self.assertFalse({"xiaotai_mod_recognition", "xiaotai_media", "nvs_flash"} & depends["recognition_config"])
        journal = (self.units["recognition"].path / "src/recognition_journal.c").read_text(encoding="utf-8")
        self.assertIn('#include "xiaotai/port/kv.h"', journal)

    def test_sdk_product_graphs_keep_the_recognition_pipeline_on_s3(self):
        expected = {
            "xiaotai-szpi-s3": {"recognition", "recognition_config"},
            "xiaotai-cores3-lite": {"recognition", "recognition_config"},
            "xiaotai-waveshare-p4-rev3": set(),
            "xiaotai-wt99p461-s1-1v1": set(),
            "xiaotai-xmini-c3-v3": {"recognition_config"},
        }
        import product_scope
        for name, modules in expected.items():
            if name not in product_scope.expected(expected):
                continue
            with self.subTest(product=name):
                config = selection.load_product(ROOT / "products" / name / "product.json")
                actual = selection.plan(ROOT, config)
                self.assertEqual(set(actual["modules"]) & self.units.keys(), modules)
                for module in self.units:
                    self.assertEqual(self.units[module].component in actual["components"], module in modules)
                if "recognition" in modules:
                    for dependency in self.units["recognition"].depends:
                        self.assertLess(actual["modules"].index(dependency), actual["modules"].index("recognition"))


if __name__ == "__main__":
    unittest.main()
