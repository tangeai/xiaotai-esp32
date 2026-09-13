"""Exercise the same CMake lock expansion used by the P4 project (IDF Python)."""
import argparse
import copy
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "@XIAOTAI_P4_PROJECT_DIR@"


def dependency_graph(path):
    graph = yaml.safe_load(path.read_text(encoding="utf-8"))
    for component in graph["dependencies"].values():
        source = component.get("source", {})
        if source.get("type") == "local":
            source["path"] = Path(source["path"]).name
    return graph


class DependencyLockTest(unittest.TestCase):
    def test_relocation_preserves_graph(self):
        seed = (ROOT / "dependencies.lock").read_bytes()
        graph = yaml.safe_load(seed)
        local = [v["source"] for v in graph["dependencies"].values()
                 if v.get("source", {}).get("type") == "local"]
        self.assertEqual(len(local), 5)
        self.assertTrue(all(s["path"].startswith(TOKEN + "/components/") for s in local))
        with tempfile.TemporaryDirectory(prefix="xiaotai lock ") as directory:
            parent = Path(directory)
            script = parent / "expand.cmake"
            script.write_text(
                f'include("{(ROOT / "tools/dependency_lock.cmake").as_posix()}")\n'
                'xiaotai_prepare_dependency_lock("${SOURCE}" "${OUTPUT}")\n',
                encoding="utf-8")
            for name in ("checkout with spaces", "checkout_\u5c0f\u949b"):
                source = parent / name
                source.mkdir()
                (source / "dependencies.lock").write_bytes(seed)
                output = source / "build"
                subprocess.run(["cmake", f"-DSOURCE={source.as_posix()}",
                                f"-DOUTPUT={output.as_posix()}", "-P", str(script)],
                               check=True, capture_output=True)
                expanded = yaml.safe_load((output / "dependencies.lock").read_bytes())
                expected = copy.deepcopy(graph)
                for component in expected["dependencies"].values():
                    entry = component.get("source", {})
                    if entry.get("type") == "local":
                        entry["path"] = entry["path"].replace(TOKEN, source.as_posix())
                self.assertEqual(expanded, expected)
                self.assertEqual((source / "dependencies.lock").read_bytes(), seed)
        self.assertEqual((ROOT / "dependencies.lock").read_bytes(), seed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolved", type=Path,
                        help="Also verify an IDF-generated lock against the reviewed seed")
    args, remaining = parser.parse_known_args()
    if args.resolved:
        if dependency_graph(args.resolved) != dependency_graph(ROOT / "dependencies.lock"):
            raise SystemExit("Resolved dependencies differ from the reviewed lock")
        print("Resolved dependency graph matches reviewed lock")
    unittest.main(argv=[__file__] + remaining)
