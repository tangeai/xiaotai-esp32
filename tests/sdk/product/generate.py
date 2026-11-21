# SPDX-License-Identifier: MIT
"""Make the exact V3 glue and the small host-only board fixture header."""

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests"))
from test_products import (board_manifest, configuration_cases, load_inventory, read_json, selection)


def configurations(product):
    plan = lambda config: selection.plan(ROOT, config)  # noqa: E731
    return dict(configuration_cases(ROOT, product, plan(product), plan))


def emit(path, text):
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")


def generate(product, destination):
    board_path = board_manifest(ROOT, product)
    board = read_json(board_path)
    result = selection.write(ROOT, product, destination)
    emit(destination / "storage_boot.cmake", "set(XIAOTAI_HOST_STORAGE_RECOVERY " +
         ("ON" if product.get("local_on_storage_boot_failure") else "OFF") + ")\n")
    net = product.get("parameters", {}).get("net", {})
    raw = net.get("layout") == "XIAOTAI_NET_STORE_PLAIN"
    space = net.get("space", "wifi_cfg")
    board_defaults = (board_path.parent / "sdkconfig.board").read_text(encoding="utf-8")
    main_stack = re.search(r"^CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)$", board_defaults, re.MULTILINE)
    if not main_stack:
        raise ValueError(f"{board_path}: expected main task stack in the board defaults")
    at = selection.at_parameters(product, board)
    pause_ai = product.get("parameters", {}).get("ai_talk", {}).get("pause_platform_signaling_while_active", False)
    emit(destination / "product_host.h",
         "// Generated host fixture values. Do not edit.\n#pragma once\n"
         f"#define PRODUCT_HOST_EXTERNAL_RAM {int(board['memory']['external_ram'])}\n"
         f"#define PRODUCT_HOST_PLATFORM_CONSOLE {int(not at.get('no_platform_console'))}\n"
         f"#define PRODUCT_HOST_AT_TABLES {int(not at.get('omit_command_tables'))}\n"
         f"#define PRODUCT_HOST_AI_PAUSES_SIGNALING {int(pause_ai)}\n"
         f"#define PRODUCT_HOST_NET_SPACE {json.dumps(space)}\n"
         f"#define PRODUCT_HOST_NET_RAW {int(raw)}\n"
         f"#define CONFIG_ESP_MAIN_TASK_STACK_SIZE {main_stack[1]}\n")
    # The host port consumes the same emitted options; y/n use its explicit
    # 1/0 override syntax rather than IDF's absent-symbol convention.
    options = ["// Generated host options. Do not edit.\n#pragma once\n"]
    for line in result["options"] + result["sdkconfig"]:
        key, value = line.split("=", 1)
        if key.startswith("CONFIG_XIAOTAI_"):
            value = {"y": "1", "n": "0"}.get(value, value)
            options.append(f"#define {key} {value}\n")
    emit(destination / "product_options.h", "".join(options))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", action="store_true")
    parser.add_argument("--product")
    parser.add_argument("--case", default="default")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.inventory:
        print(json.dumps({
            product["name"]: {
                case: {"at": "xiaotai_at" in selection.plan(ROOT, config)["components"],
                       "storage_recovery": config.get("local_on_storage_boot_failure", False)}
                for case, config in configurations(product).items()
            }
            for product in load_inventory(ROOT)
        }))
    elif args.product and args.out:
        product = selection.load_product(ROOT / "products" / args.product / "product.json")
        generate(product if args.case == "default" else configurations(product)[args.case], args.out)
    else:
        parser.error("choose --inventory or --product and --out")


if __name__ == "__main__":
    main()
