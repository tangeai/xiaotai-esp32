#!/usr/bin/env python3
"""Reject mixed I2C driver families in the linked P4 product (Windows/Unix)."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def check_family(symbols: str, link_map: str) -> None:
    legacy = re.search(r"\si2c_driver_install$", symbols, re.MULTILINE)
    modern = re.search(r"\si2c_new_master_bus$", symbols, re.MULTILINE)
    if legacy and modern:
        raise ValueError("ELF links both legacy I2C and driver_ng")
    # The legacy global constructor can survive install() being removed by GC.
    if re.search(r"driver/libdriver\.a\(i2c\.c\.obj\)|esp_lcd_panel_io_i2c_v1\.c\.obj",
                 link_map.replace("\\", "/")):
        raise ValueError("link map contains a legacy I2C object that aborts at boot")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--nm", default=os.environ.get("CROSS_NM", "riscv32-esp-elf-nm"))
    args = parser.parse_args()
    try:
        if not args.elf.is_file() or args.elf.stat().st_size == 0:
            raise ValueError("linked ELF is missing or empty")
        result = subprocess.run([args.nm, "-C", str(args.elf)], check=True,
                                capture_output=True, text=True, encoding="utf-8",
                                errors="replace")
        link_map = args.elf.with_suffix(".map").read_text(encoding="utf-8", errors="replace")
        check_family(result.stdout, link_map)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"FAIL: i2c_driver_family: {error}", file=sys.stderr)
        return 1
    print("PASS: i2c_driver_family")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
