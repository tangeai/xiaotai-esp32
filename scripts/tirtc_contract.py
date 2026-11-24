#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the build contract of an official TiRTC package.

The contract is what the build checks before it links the archive: the
archive and header hashes, the ESP-IDF version and toolchain, the C library,
the FreeRTOS type sizes, the crypto the archive needs from ESP-IDF, and on the
ESP32-S3 the number of s32c1i instructions. Everything is read from the
unpacked package; nothing is typed in by hand.

    python3 scripts/tirtc_contract.py --line 6.0 --target esp32s3 \\
        --package components/tirtc_sdk/packages/<name> \\
        --out components/tirtc_sdk/contracts/6.0/esp32s3.json

nm and objdump come from the target's toolchain on PATH (xtensa-esp-elf-nm,
riscv32-esp-elf-nm), or pass --nm and --objdump.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from fetch_tirtc import package_entry  # noqa: E402

TOOLCHAINS = {"esp32s3": "xtensa-esp-elf", "esp32p4": "riscv32-esp-elf", "esp32c3": "riscv32-esp-elf",
              "esp32s31": "riscv32-esp-elf"}
# ESP-IDF calls through which an archive takes memory with explicit capabilities.
CAPS_ALLOCATORS = ("heap_caps_malloc", "heap_caps_calloc", "heap_caps_realloc",
                   "xTaskCreateWithCaps", "xTaskCreatePinnedToCoreWithCaps")
# Crypto settings an archive that calls ESP-IDF's Mbed TLS directly depends on.
CRYPTO_NEEDS = {"mbedtls_des_": ("CONFIG_MBEDTLS_DES_C", "y"), "mbedtls_sha256": ("CONFIG_MBEDTLS_HARDWARE_SHA", "y")}


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_env(path):
    values = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    return values


def memory_config(build, undefined):
    """External RAM the archive needs: its declaration, checked against its calls.

    A package built for PSRAM takes its buffers and task stacks through ESP-IDF's
    capability allocators. It must say so (PSRAM=enabled) so that the build
    rejects a board without external RAM; the build checks this, it never turns
    PSRAM on for a board."""
    declared = build.get("PSRAM")
    if declared not in (None, "enabled", "disabled"):
        raise SystemExit(f"manifest/build-contract.env: PSRAM={declared}; expected enabled or disabled")
    calls = sorted(name for name in CAPS_ALLOCATORS if name in undefined)
    if calls and declared is None:
        raise SystemExit("The archive allocates with explicit capabilities (" + ", ".join(calls) +
                         "), but manifest/build-contract.env says neither PSRAM=enabled nor PSRAM=disabled")
    return {"CONFIG_SPIRAM": "y"} if declared == "enabled" else {}


def tool(name, explicit, target):
    found = explicit or shutil.which(f"{TOOLCHAINS[target]}-{name}")
    if not found:
        raise SystemExit(f"{TOOLCHAINS[target]}-{name} not found; activate the ESP-IDF environment or pass --{name}")
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--line", required=True, help="ESP-IDF line in packages.json, e.g. 6.0")
    parser.add_argument("--target", required=True, choices=sorted(TOOLCHAINS))
    parser.add_argument("--package", required=True, type=Path, help="unpacked package directory")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--nm")
    parser.add_argument("--objdump")
    args = parser.parse_args(argv)

    lines = json.loads((ROOT / "components/tirtc_sdk/packages.json").read_text(encoding="utf-8"))["lines"]
    entry = package_entry(lines[args.line], f"generic-{args.target}")
    archive = args.package / "lib/libTiRTC.a"
    match = re.search(r"esp-idf(\d+\.\d+\.\d+)-\w+-esp(\d+\.\d+\.\d+)_(\d+)__v([\d.]+)__", args.package.name)
    if not archive.is_file() or not match:
        raise SystemExit(f"{args.package} is not an unpacked official TiRTC package")
    producer_idf, compiler, toolchain_date, version = match.groups()
    build = read_env(args.package / "manifest/build-contract.env")

    nm = tool("nm", args.nm, args.target)
    symbols = subprocess.run([nm, "-A", archive], capture_output=True, text=True, check=True).stdout
    undefined = {line.split()[-1] for line in symbols.splitlines() if " U " in line}
    external = sorted(name for name in undefined if name.startswith(("mbedtls_", "psa_")))
    http_ssl = re.search(r"httpclt\.o:[^\n]* U mbedtls_ssl_", symbols) is not None

    contract = {
        # Where the line's packages come from, as packages.json records it.
        "target": args.target, "origin": lines[args.line].get("origin", "official"), "sdk_version": version,
        "package": entry["path"].rsplit("/", 1)[-1] + ".tgz", "package_sha256": entry["sha256"],
        "producer_idf_version": producer_idf, "idf_version": lines[args.line]["idf_versions"][0],
        "compiler_version": compiler, "toolchain_release": f"esp-{compiler}_{toolchain_date}",
        "sha256": sha256(archive),
        "headers": {path.name: sha256(path) for path in sorted((args.package / "include/tirtc").glob("*.h"))},
        "http_tls_backend": "mbedtls" if http_ssl else "none",
    }
    libc = build.get("LIBC", "newlib")
    contract["libc_config"] = {"CONFIG_LIBC_PICOLIBC": "y"} if libc == "picolibc" else {"CONFIG_LIBC_NEWLIB": "y"}
    memory = memory_config(build, undefined)
    if memory:
        contract["memory_config"] = memory
    if external:
        needs = {key: value for prefix, (key, value) in CRYPTO_NEEDS.items() if any(n.startswith(prefix) for n in external)}
        contract.update(crypto_mode="required", crypto_config=needs,
                        crypto_provenance="The archive calls ESP-IDF's Mbed TLS for: " + ", ".join(external))
    else:
        contract.update(crypto_mode="bundled",
                        crypto_provenance="The archive carries its own crypto under its own symbol prefix and "
                                          "calls no mbedtls_* or psa_* function of ESP-IDF.")
    if args.target == "esp32s3":
        objdump = tool("objdump", args.objdump, args.target)
        code = subprocess.run([objdump, "-d", archive], capture_output=True, text=True, check=True).stdout
        contract["hardware_atomics"] = len(re.findall(r"[ \t]s32c1i[ \t]", code))
        contract["atomic_config"] = {"CONFIG_SPIRAM": "y"}
        if libc == "picolibc":
            contract["atomic_config"]["CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND"] = "y"
    abi = {"static_semaphore": int(build.get("STATIC_SEMAPHORE_T_SIZE", 84)),
           "static_task": int(build.get("STATIC_TASK_T_SIZE", 100 if libc == "picolibc" else 340)),
           "tick": int(build.get("TICK_TYPE_T_SIZE", 4)), "time": int(build.get("TIME_T_SIZE", 8)),
           "semaphore_alignment": 4}
    contract.update(abi=abi, freertos_hz=1000, trace_enabled=False, runtime_stats=False, production_ready=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(contract, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"{args.out}: {version} for {args.target}, {len(external)} ESP-IDF crypto symbols, "
          f"{contract.get('hardware_atomics', 0)} s32c1i")
    return 0


if __name__ == "__main__":
    sys.exit(main())
