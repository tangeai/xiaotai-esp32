#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Configure a sandboxed overlay-selection fixture; syntax-check real SCCB and adapter TUs."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from check_raw_try_take import syntax_check


HERE = Path(__file__).resolve().parent
PART = HERE.parent
SDK = PART.parents[1]
PROJECT = HERE / "native_overlay_project"


def configure(sandbox, vendor, mode, error=None):
    proc = subprocess.run(["cmake", "-S", str(PROJECT), "-B", str(sandbox), f"-DCAMERA_DIR={vendor}", f"-Dmode={mode}"],
                          capture_output=True, text=True, check=False)
    if error:
        assert proc.returncode and error in proc.stderr, proc.stderr
    elif proc.returncode:
        raise RuntimeError(proc.stderr)


def wrapper_lifecycle(source, scratch, legacy=False):
    text = source.read_text(encoding="utf-8")
    begin = text.index("esp_err_t esp_camera_init(const camera_config_t *config)\n")
    end = text.index("#define FB_GET_TIMEOUT", begin)
    fragment = text[begin:end]
    assert fragment.count("esp_err_t esp_camera_init(") == 1
    assert fragment.count("esp_err_t esp_camera_deinit()") == 1
    (scratch / "wrapper_lifecycle.inc").write_text(fragment, encoding="utf-8")
    output = scratch / "wrapper_lifecycle"
    subprocess.run(["cc", "-std=c11", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-fno-pie", "-no-pie", "-I", str(scratch),
                    str(HERE / "wrapper_lifecycle_test.c"), "-o", str(output)], check=True)
    subprocess.run([str(output), *(["legacy"] if legacy else [])], check=True)
    print("PASS: exact original init reproduces ignored first format failure" if legacy else
          "PASS: exact generated init propagates first format/status errors and cleans up")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component-root", type=Path, required=True)
    parser.add_argument("--compile-db", type=Path, action="append", default=[])
    args = parser.parse_args()
    vendor = args.component_root.resolve()
    sccb = vendor / "driver" / "sccb-ng.c"
    cam = vendor / "driver" / "cam_hal.c"
    public = vendor / "driver" / "esp_camera.c"
    stamp = vendor / ".component_hash"
    initial = {path: path.read_bytes() for path in (sccb, cam, stamp, public)}
    with tempfile.TemporaryDirectory(prefix="dvp-native-context-") as temp:
        scratch = Path(temp)
        configure(scratch / "stock", vendor, "stock")
        configure(scratch / "old-product", vendor, "legacy_product")
        configure(scratch / "early", vendor, "early_scan")
        configure(scratch / "native", vendor, "native")
        wrapper_lifecycle(public, scratch, legacy=True)
        wrapper_lifecycle(scratch / "native" / "xiaotai_camera_dvp_overlay" / public.name, scratch)
        configure(scratch / "legacy", vendor, "legacy_sccb", "new-I2C SCCB source")
        configure(scratch / "missing", vendor, "missing_camera", "requires the product's locked esp32-camera")
        configure(scratch / "stock-missing", vendor, "stock_missing", "requires the product's locked esp32-camera")
        patched = scratch / "native" / "xiaotai_camera_dvp_overlay" / sccb.name
        addon = PART / "esp-idf" / "sccb_release.inc"
        expected = initial[sccb].replace(b"\r\n", b"\n") + b"\n" + addon.read_bytes().replace(b"\r\n", b"\n")
        assert patched.read_bytes() == expected
        changed = scratch / "changed-component"
        (changed / "driver").mkdir(parents=True)
        for path in (cam, stamp, vendor / "driver" / "esp_camera.c"):
            shutil.copyfile(path, changed / path.relative_to(vendor))
        (changed / sccb.relative_to(vendor)).write_bytes(initial[sccb] + b"\n/* source drift */\n")
        configure(scratch / "changed", changed, "native", "Unreviewed esp32-camera SCCB source")
        shutil.copyfile(sccb, changed / sccb.relative_to(vendor))
        wrapper = changed / "driver" / "esp_camera.c"
        wrapper.write_bytes(wrapper.read_bytes() + b"\n/* ownership semantics may have changed */\n")
        configure(scratch / "changed-wrapper", changed, "native", "Unreviewed esp32-camera public wrapper")

        private_headers = [PART / "src", PART / "include", SDK / "xiaotai_hal" / "include",
                           SDK / "xiaotai_port" / "include", SDK / "xiaotai_port" / "esp-idf" / "include",
                           PART.parent / "xiaotai_part_bus" / "include"]
        old_include = os.environ.get("C_INCLUDE_PATH")
        for database in args.compile_db:
            syntax_check(database.resolve(), sccb, patched)
            syntax_check(database.resolve(), public, scratch / "native" / "xiaotai_camera_dvp_overlay" / public.name)
        os.environ["C_INCLUDE_PATH"] = os.pathsep.join(map(str, private_headers))
        try:
            for database in args.compile_db:
                adapter = PART / "esp-idf" / "camera_dvp_io.c"
                syntax_check(database.resolve(), cam, adapter)
                syntax_check(database.resolve(), cam, adapter, "-DXIAOTAI_DVP_USE_OVERLAY=1")
        finally:
            if old_include is None:
                os.environ.pop("C_INCLUDE_PATH", None)
            else:
                os.environ["C_INCLUDE_PATH"] = old_include

    assert all(path.read_bytes() == value for path, value in initial.items())
    print("PASS: stock and opt-in selection, legacy and early scanning isolated, pristine vendor inputs")


if __name__ == "__main__":
    main()
