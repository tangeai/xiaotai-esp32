#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the dormant overlay against a local pristine component and optional S3 compile databases.

Uses syntax-only compilation of the generated translation unit, never a firmware
build. Run in the host environment that generated the compile databases.
"""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
GENERATE = HERE / "generate_raw_try_take.cmake"
EXTENSION = HERE.parent / "esp-idf" / "raw_try_take.inc"


def cmake_generate(vendor: Path, output: Path, forbidden: str | None = None) -> Path:
    run = subprocess.run(["cmake", f"-DCAMERA_DIR={vendor}", "-P", str(GENERATE)],
                         cwd=output, capture_output=True, text=True, check=False)
    candidate = output / "xiaotai_camera_dvp_overlay" / "cam_hal.c"
    if forbidden:
        assert run.returncode and forbidden in run.stderr, run.stderr
        assert not candidate.exists(), candidate
    elif run.returncode:
        raise RuntimeError(run.stderr)
    return candidate


def syntax_check(database: Path, pristine: Path, candidate: Path, *options: str) -> None:
    items = json.loads(database.read_text(encoding="utf-8"))

    def source(item):
        return (Path(item["directory"]) / item["file"]).resolve()

    matching = [item for item in items if Path(item["file"]).name == pristine.name and source(item) == pristine]
    if len(matching) != 1:
        raise ValueError(f"Expected one pristine cam_hal.c compilation in {database}")
    entry = matching[0]
    args = entry.get("arguments") or shlex.split(entry["command"])
    cwd = Path(entry["directory"])
    result = [args[0]]
    replaced = 0
    index = 1
    while index < len(args):
        value = args[index]
        index += 1
        if value in ("-o", "-MF", "-MT", "-MQ"):
            index += 1
        elif value in ("-c", "-MD", "-MMD", "-MP"):
            continue
        elif not value.startswith("-") and (cwd / value).resolve() == pristine:
            result.append(str(candidate))
            replaced += 1
        else:
            result.append(value)
    if replaced != 1 or not Path(args[0]).name.endswith("-gcc"):
        raise ValueError(f"Unsupported native compile command in {database}")
    subprocess.run([*result, *options, "-fsyntax-only"], cwd=cwd, check=True)
    print(f"PASS: native syntax-only overlay in {database.parent}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component-root", type=Path, required=True)
    parser.add_argument("--compile-db", type=Path, action="append", default=[])
    args = parser.parse_args()
    vendor = args.component_root.resolve()
    original = vendor / "driver" / "cam_hal.c"
    stamp = vendor / ".component_hash"
    old_source, old_hash = original.read_bytes(), stamp.read_bytes()

    with tempfile.TemporaryDirectory(prefix="dvp-raw-try-take-") as tmp:
        scratch = Path(tmp)
        candidate = cmake_generate(vendor, scratch)
        expected = old_source.replace(b"\r\n", b"\n")
        insertions = [
            (b"static cam_obj_t *cam_obj = NULL;\n", b"\n" + EXTENSION.read_bytes().replace(b"\r\n", b"\n") + b"\n", True),
            (b"    size_t frame_buffer_queue_len = cam_obj->frame_cnt;", b"    xt_dvp_reset_sequence();\n", False),
            (b"                        //send frame\n", b"                        if (!cam_obj->frames[frame_pos].en) xt_dvp_stamp_raw(frame_pos);\n", True),
        ]
        for anchor, insertion, after in insertions:
            assert expected.count(anchor) == 1, anchor
            expected = expected.replace(anchor, anchor + insertion if after else insertion + anchor)
        assert candidate.read_bytes() == expected, "overlay must only add the reviewed tracking extensions"
        modified = scratch / "modified"
        modified_source = modified / "driver" / original.name
        modified_stamp = modified / stamp.name
        modified_source.parent.mkdir(parents=True)
        shutil.copyfile(stamp, modified_stamp)
        modified_source.write_bytes(old_source + b"\n/* source drift */\n")
        negative = scratch / "negative"
        negative.mkdir()
        cmake_generate(modified, negative, "Unreviewed esp32-camera source")
        shutil.copyfile(original, modified_source)
        modified_stamp.write_bytes(b"wrong component identity\n")
        cmake_generate(modified, negative, "Unreviewed esp32-camera source")
        shutil.copyfile(stamp, modified_stamp)
        cmake_generate(modified, modified, "separate build directory")
        for database in args.compile_db:
            syntax_check(database.resolve(), original, candidate)

    assert original.read_bytes() == old_source and stamp.read_bytes() == old_hash
    print("PASS: pristine sources, reviewed insertions, both hash refusals, no vendor-directory output")


if __name__ == "__main__":
    main()
