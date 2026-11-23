# SPDX-License-Identifier: MIT
"""Emit only the preferences and settings glue from a real product manifest."""

from dataclasses import replace
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection


def main():
    name, mode, output = sys.argv[1:]
    if mode not in {"panel", "door_panel", "xmini"}:
        raise ValueError(f"unknown settings fixture mode: {mode}")
    product = sdk_selection.load_product(ROOT / "products" / name / "product.json")
    catalog = sdk_selection.catalog(ROOT)["module"]
    units = tuple(replace(catalog[unit], configure=[entry for entry in catalog[unit].configure
                  if entry["function"] == function]) for unit, function in (
                      ("identity", "xiaotai_preferences_configure"), ("settings", "xiaotai_settings_configure")))
    parameters = {key: value for key, value in product["parameters"].items() if key in {"preferences", "settings"}}
    glue, headers, calls = sdk_selection._glue((), units, dict(product, parameters=parameters))
    includes = "".join(f'#include "{header}"\n' for header in headers)
    body = "".join(call + "    if (err != XIAOTAI_OK) return err;\n" for call in calls)
    Path(output).write_text(includes + "".join(glue.objects) +
                            f'const char settings_legacy_mode[] = "{mode}";\n' +
                            "xiaotai_err_t settings_legacy_configure(void) {\n"
                            "    xiaotai_err_t err = XIAOTAI_OK;\n" + body + "    return err;\n}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
