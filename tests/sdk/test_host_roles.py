# SPDX-License-Identifier: MIT
"""Exercise role failures without preparing or downloading any sources."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

RUN = Path(__file__).with_name("run.py")


def check_host_roles():
    with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-roles-") as tmp:
        root = Path(tmp)
        fixture = root / "sdk/xiaotai_fixture"
        fixture.mkdir(parents=True)
        (fixture / "case.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
        (fixture / "CMakeLists.txt").write_text(
            "add_library(xiaotai_fixture INTERFACE)\n"
            "xiaotai_host_test(fixture_pure SOURCES case.c)\n"
            "xiaotai_host_test(fixture_cjson SOURCES case.c ROLES cjson)\n",
            encoding="utf-8",
        )
        env = {**os.environ, "XIAOTAI_HOST_CJSON_DIR": str(root / "absent")}

        def run(*args):
            return subprocess.run([sys.executable, "-B", str(RUN), "-j", "2", *args],
                                  capture_output=True, text=True, env=env, timeout=120)

        missing = run()
        assert missing.returncode == 1 and "XIAOTAI_HOST_CJSON_DIR: no directory" in missing.stdout, missing
        focused = run("--root", str(root), "xiaotai_fixture")
        assert focused.returncode == 1 and "Host role cjson required by fixture_cjson" in focused.stdout, focused
        allowed = run("--allow-missing", "--root", str(root), "xiaotai_fixture")
        assert allowed.returncode == 0 and "SKIP: fixture_cjson (needs cjson)" in allowed.stdout, allowed
        assert "PARTIAL: SDK host build and 2 tests" in allowed.stdout and "fixture_pure" in allowed.stdout, allowed
        Path(env["XIAOTAI_HOST_CJSON_DIR"]).mkdir()
        bad = run("--allow-missing", "--root", str(root), "xiaotai_fixture")
        assert bad.returncode == 1 and "does not match the lock" in bad.stdout, bad
        (fixture / "CMakeLists.txt").write_text(
            "add_library(xiaotai_fixture INTERFACE)\n"
            "xiaotai_host_test(fixture_unknown SOURCES case.c ROLES future-role)\n",
            encoding="utf-8",
        )
        unknown = run("--allow-missing", "--root", str(root), "xiaotai_fixture")
        assert unknown.returncode == 1 and "unknown host component role future-role" in " ".join(
            unknown.stdout.split()), unknown


if __name__ == "__main__":
    check_host_roles()
    print("PASS: SDK host role preflight")
