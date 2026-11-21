"""Render the public SDK AT vocabulary from the same plans as commands.json."""
import argparse
from itertools import product
import json
from pathlib import Path

from console_sdk import WORD, _forms
from sdk_selection import ROOT, SelectionError, apply_variant, catalog, load_product, plan, vocabulary

START = "<!-- GENERATED: sdk AT vocabulary -->"
END = "<!-- END GENERATED: sdk AT vocabulary -->"
MIGRATION = "scripts/at_command_migration.json"


def code(value):
    return "`" + value.replace("|", r"\|") + "`"


def usage_forms(usage):
    # Match arity, optional groups and literal choices, not placeholder labels.
    def choices(token):
        if token.startswith("<"):
            if token == "<0|1>":
                return ("0", "1")
            return ("<arg>",)
        return tuple(token.split("|"))

    return {args for form in _forms(usage) for args in product(*(choices(token) for token in form))}


def migration_signature(spelling):
    command, _, usage = spelling.partition(" ")
    word, _, rest = usage.partition(" ")
    verb, usage = (word, rest) if WORD.fullmatch(word) else (None, usage)
    return (command, verb), usage_forms(usage)


def check_migration(ledger, units):
    owners = {}
    for kind in ("module", "application"):
        for unit in units[kind].values():
            owner = f"app.{unit.name}" if kind == "application" else unit.name
            actual = owners[owner] = {}
            for entry in vocabulary((unit,)):
                signature = (entry["command"], entry["verb"])
                actual.setdefault(signature, set()).update(usage_forms(entry["usage"]))
    missing = []
    for section in ("migrated", "added_in_section"):
        for group in ledger[section]:
            for owner in group["owners"]:
                for spelling in group["forms"]:
                    signature, expected = migration_signature(spelling)
                    if not expected <= owners.get(owner, {}).get(signature, set()):
                        missing.append(f"{owner}: {spelling}")
    if missing:
        raise SelectionError("AT migration owner/form missing: " + "; ".join(missing))


def vocabulary_table(root):
    root = Path(root)
    ledger = json.loads((root / MIGRATION).read_text(encoding="utf-8"))
    check_migration(ledger, catalog(root))
    entries = {}
    for path in sorted((root / "products").glob("*/product.json")):
        product = load_product(path)
        selections = [product] + [apply_variant(product, name) for name in sorted(product.get("variants", {}))]
        for selection in selections:
            for row in plan(root, selection)["commands"]:
                key = (row["command"], row["verb"] or "", row["usage"], row["access"], row["option"] or "")
                entries.setdefault(key, set()).update(row["units"])
    rows = ["| 写法 | 权限 | 登记单元 | 相关构建选项 |", "| --- | --- | --- | --- |"]
    for (command, verb, usage, access, option), units in sorted(entries.items()):
        spelling = code(" ".join(part for part in (command, verb, usage) if part))
        owners = "、".join(code(unit) for unit in sorted(units))
        rows.append(f"| {spelling} | `{access}` | {owners} | {code(option) if option else '-'} |")
    return "\n".join(rows) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when the checked-in table is stale")
    mode.add_argument("--write", action="store_true", help="replace only the generated Markdown block")
    args = parser.parse_args(argv)
    try:
        table = vocabulary_table(ROOT)
        if not (args.check or args.write):
            print(table, end="")
            return 0
        doc = ROOT / "docs/AT指令.md"
        current = doc.read_text(encoding="utf-8")
        if current.count(START) != 1 or current.count(END) != 1 or current.index(START) >= current.index(END):
            raise ValueError(f"{doc}: expected one generated vocabulary block")
        left, right = current.index(START) + len(START), current.index(END)
        expected = current[:left] + "\n\n" + table + "\n" + current[right:]
        if args.write:
            doc.write_text(expected, encoding="utf-8", newline="\n")
        elif current != expected:
            parser.error("AT vocabulary is stale; run python scripts/generate_at_reference.py --write")
    except (OSError, ValueError, SelectionError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
