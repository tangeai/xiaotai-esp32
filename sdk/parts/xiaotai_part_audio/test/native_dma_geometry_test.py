#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the real audio port admission with two locked IDF DMA calculators."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PART = HERE.parent
SDK = PART.parent.parent
IDF_SOURCE = "components/esp_driver_i2s/i2s_common.c"
PINS = {
    "4dd53bd7b95caf1c6b5840c0256eb9080445b3bf5bcdd496eb20e6430ebe76b5",
    "745d65622535ecf1284fc0a70f8825d9fb711f153f61b8927f6160dab0ba3c27",
}


def definition(source, name):
    match = re.search(r"(?m)^[\w ]+\b" + name + r"\([^;{}]*\)\n\{[\s\S]*?^\}", source)
    if match is None:
        raise ValueError(f"Missing definition of {name}")
    return match[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    port_source = (PART / "esp-idf/audio_i2s.c").read_text(encoding="utf-8")
    includes = [HERE, PART / "include", SDK / "xiaotai_hal/include", SDK / "xiaotai_port/include",
                SDK / "parts/xiaotai_part_bus/include"]
    with tempfile.TemporaryDirectory(prefix="xt-audio-dma-") as temp:
        temp = Path(temp)
        source = temp / "geometry.c"
        binary = temp / "geometry"
        for idf in args.idf:
            raw = (idf / IDF_SOURCE).read_bytes().replace(b"\r\n", b"\n")
            if hashlib.sha256(raw).hexdigest() not in PINS:
                raise ValueError(f"Unexpected locked source under {idf}")
            source.write_text('#include "native_dma_geometry_shim.h"\n' + "\n".join([
                definition(raw.decode("utf-8"), "i2s_get_buf_size"),
                definition(port_source, "std_slot_width"),
                definition(port_source, "xt_audio_io_describe"),
            ]), encoding="utf-8")
            for l1 in (0, 1):
                subprocess.run([os.environ.get("CC", "cc"), "-std=c17", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                                f"-DSOC_CACHE_INTERNAL_MEM_VIA_L1CACHE={l1}",
                                *[f"-I{p}" for p in includes], source, HERE / "native_dma_geometry_test.c",
                                "-o", binary], check=True)
                subprocess.run([binary], check=True,
                               env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                                    "UBSAN_OPTIONS": "halt_on_error=1"})
            print(f"Native DMA geometry matches {idf}", flush=True)


if __name__ == "__main__":
    main()
