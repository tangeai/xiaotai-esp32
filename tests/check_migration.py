# SPDX-License-Identifier: MIT
"""Verify the V2 -> V3 host-test ledger without building or removing tests.

Targets use an exact CTest name in the component's test/CMakeLists.txt or a
repo-relative script path. A pending plan may use null for an unnamed target.
After the replacement assertions pass, mark the row complete and put
"Replaces legacy: <path>[::<case>]" in each replacement source's file header.
Rows closed by a product retirement still name their legacy source. They are
reported separately from passed replacements and have no active destination.

Once every row was closed, the legacy inventory was frozen into
tests/migration_inventory.json together with the commit that still holds every
legacy source. From then on the inventory and the retired board manifests are
read from that file, so legacy sources can be deleted from the tree; read them
at the recorded commit. `--freeze` writes the file from the current tree.

Classify a row from its legacy assertions: add scope with either common=true
or a nonempty products list, plus a basis explaining the classification.
If the whole row's scope is not known, leave it unclassified. An unclassified
row blocks every product cutover; the global inventory check still works.
"""

import argparse
import ast
from collections import Counter
from datetime import date
import json
from pathlib import Path, PurePosixPath
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import product_scope  # noqa: E402
FROZEN = "tests/migration_inventory.json"
V3_TESTS = {"test_sdk_host.py", "test_sdk_selection.py", "test_products.py", "test_migration.py",
            "test_port_usb_monitor.py", "test_native_build_entry.py", "test_p4_native_opt_in.py",
            "test_elf_static_memory.py", "test_ai_signaling_policy.py", "test_vendor_idle_stack_profile.py",
            "test_product_host_runner.py"}
VOICE = "tests/regression/test_voice_reliability.py"
CRYPTO = "tests/host_crypto/run.py"
UNREGISTERED = (
    "test_feedback.c",
    "test_platform_task_metrics.c",
    "xmini_memtrace/test_memtrace.c",
    "xmini_memtrace/test_heap_failure.c",
    "heapcap_status_fixture.c",
    "platform_http_metadata_fixture.c",
)


def voice_cases(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.If) and ast.unparse(node.test) == "__name__ == '__main__'":
            for expr in node.body:
                if isinstance(expr, ast.Expr) and isinstance(expr.value, ast.Call):
                    func = expr.value.func
                    if isinstance(func, ast.Name) and func.id.startswith("test_"):
                        yield func.id


def registrations(path, group=None):
    """Read test declarations and the simple foreach forms used by the host suites."""
    commands = re.finditer(
        r"^[ \t]*(foreach|endforeach|add_test|add_executable|xiaotai_host_test|xmini_test|mz28_test)"
        r"\(([^)]+)?\)", path.read_text(encoding="utf-8"), re.M,
    )
    contexts = [[{}]]
    sources = {}
    for command, raw in (match.groups() for match in commands):
        tokens = (raw or "").split()
        if command == "foreach":
            key, *values = tokens
            if values[:1] == ["RANGE"]:
                try:
                    bounds = list(map(int, values[1:]))
                except ValueError:
                    bounds = []
                if len(bounds) == 1:
                    bounds.insert(0, 0)
                step = bounds[2] if len(bounds) == 3 else 1
                values = [str(i) for i in range(bounds[0], bounds[1] + 1, step)] if bounds else []
            contexts.append([{**ctx, "${" + key + "}": value} for ctx in contexts[-1] for value in values])
            continue
        if command == "endforeach":
            contexts.pop()
            continue
        for context in contexts[-1]:
            args = [re.sub(r"\$\{\w+\}", lambda m: context.get(m[0], m[0]), t.strip('"'))
                    for t in tokens]
            name = args[0]
            if "${" in name or (command == "add_test" and "${" in args[1]):
                # Function declarations use unresolved parameters; only calls define cases.
                continue
            if command == "add_executable" and len(args) > 1:
                sources[name] = args[1]
            elif command == "xmini_test" and group == "xmini":
                sources[name] = args[1]
                yield name, args[1]
            elif command == "mz28_test" and group == "mz28":
                sources[name] = f"test_{name}.c"
                yield name, sources[name]
            elif command == "xiaotai_host_test" and group is None:
                source = args[2] if args[1:2] == ["SOURCES"] else None
                sources[name] = source
                yield name, source
            elif command == "add_test":
                if args[:1] != ["NAME"] or args[2:3] != ["COMMAND"]:
                    raise ValueError(f"unsupported test declaration in {path}: {raw}")
                name, executable = args[1], args[3]
                yield name, sources[executable] if group else sources.get(executable)


def frozen_inventory(root):
    """Return the frozen legacy inventory, or None while the tree is still scanned."""
    path = root / FROZEN
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not re.fullmatch(r"[0-9a-f]{40}", str(data.get("commit"))):
        raise ValueError(f"{FROZEN}: expected schema 1 and a full legacy commit")
    return data


def inventory(root):
    frozen = frozen_inventory(root)
    if frozen is not None:
        found = {}
        for path, case, source in frozen["tests"]:
            if (path, case) in found:
                raise ValueError(f"{FROZEN} lists {path}::{case} twice")
            found[path, case] = source
        return found
    return scan_inventory(root)


def legacy_board_products(root, board):
    """Product names a legacy board manifest declares, from the tree or the frozen record."""
    if (root / board).is_file():
        return [item["name"] for item in json.loads((root / board).read_text(encoding="utf-8"))["products"]]
    frozen = frozen_inventory(root)
    if frozen is None or board not in frozen.get("legacy_boards", {}):
        raise ValueError("expected a retained legacy board manifest")
    return frozen["legacy_boards"][board]


def freeze(root):
    """Write the frozen inventory from the current tree (all legacy sources present)."""
    import subprocess
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                            capture_output=True, text=True).stdout.strip()
    ledger = json.loads((root / "tests/migration.json").read_text(encoding="utf-8"))
    boards = sorted({row["retirement"]["legacy_board"] for row in ledger["tests"] if "retirement" in row})
    data = {
        "schema": 1,
        "commit": commit,
        "note": "Legacy V2 host-test inventory, frozen after every migration row closed. "
                "Sources are read at this commit; the tree may no longer contain them.",
        "tests": [[path, case, source] for (path, case), source in sorted(
            scan_inventory(root).items(), key=lambda item: (item[0][0], item[0][1] or ""))],
        "legacy_boards": {board: legacy_board_products(root, board) for board in boards},
    }
    (root / FROZEN).write_text(json.dumps(data, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    return data


def scan_inventory(root):
    tests = root / "tests"
    found = {}
    for path in (*tests.glob("test_*.py"), *(tests / "regression").glob("test_*.py"),
                 *(tests / "native").rglob("test_*.py")):
        rel = path.relative_to(root).as_posix()
        if path.parent == tests and path.name in V3_TESTS:
            continue
        cases = list(voice_cases(path)) if rel == VOICE else [None]
        if not cases:
            raise ValueError(f"no runnable voice cases found in {rel}")
        for case in cases:
            key = rel, case
            if key in found:
                raise ValueError(f"legacy runner schedules {key} twice")
            found[key] = "auto"
    for group in ("xmini", "mz28", "lvgl"):
        if not (tests / group / "CMakeLists.txt").is_file():
            continue
        for case, fixture in registrations(tests / group / "CMakeLists.txt", group):
            key = f"tests/{group}/{fixture}", case
            if key in found:
                raise ValueError(f"duplicate CTest registration {key}")
            found[key] = group
    for case in ("platform_hmac", "c61_image_sha256"):
        found[CRYPTO, case] = "host_crypto"
    for fixture in UNREGISTERED:
        found[f"tests/xmini/{fixture}", None] = "unregistered"
    for fixture in ("p4/test_tirtc_endpoint.c", "wt99/test_camera_frame_layout.c"):
        found[f"tests/native/{fixture}", None] = "native_fixture"
    return found


def valid_path(value):
    if not isinstance(value, str) or not value or any(ch in value for ch in "\\:\0"):
        return False
    path = PurePosixPath(value)
    return bool(path.parts) and path.as_posix() == value and not path.is_absolute() and ".." not in path.parts


def destination_source(root, target):
    component = root / target["component"]
    test = target["test"]
    if test is None:
        return None
    if "/" in test:
        return root / test if valid_path(test) and (root / test).is_file() else None
    declaration = component / "test/CMakeLists.txt"
    if declaration.is_file():
        for name, source in registrations(declaration):
            if name == test and source and (declaration.parent / source).is_file():
                return declaration.parent / source
    return None


def retirement_status(row, products, root):
    retirement = row.get("retirement")
    if (not isinstance(retirement, dict)
            or set(retirement) != {"product", "decision", "legacy_board", "previous_status"}):
        raise ValueError("expected product, decision date, legacy board and previous status")
    product, board = retirement["product"], retirement["legacy_board"]
    if not isinstance(product, str) or not product or product in products:
        raise ValueError("product must be absent from the active SDK graph")
    if retirement["previous_status"] not in {"pending", "complete"}:
        raise ValueError("previous status must be pending or complete")
    if date.fromisoformat(retirement["decision"]).isoformat() != retirement["decision"]:
        raise ValueError("decision must be an ISO date")
    if (not valid_path(board) or PurePosixPath(board).parts[:2] != ("main", "boards")
            or PurePosixPath(board).name != "board.json"):
        raise ValueError("expected a retained legacy board manifest")
    if product not in legacy_board_products(root, board):
        raise ValueError("legacy board does not declare the retired product")
    if row["disposition"] != "retire" or row["targets"] or "scope" in row:
        raise ValueError("retirement must have no destinations or active scope")
    return retirement["previous_status"]


def scope_products(scope, products):
    """Return None for common scope, or the explicit set of product consumers."""
    if not isinstance(scope, dict) or not isinstance(scope.get("basis"), str) or not scope["basis"].strip():
        raise ValueError("scope needs a source-based basis")
    keys = set(scope)
    if keys == {"common", "basis"} and scope["common"] is True:
        return None
    if keys != {"products", "basis"} or not isinstance(scope["products"], list) or not scope["products"]:
        raise ValueError("scope must name common or one or more products")
    names = scope["products"]
    if any(not isinstance(name, str) or name not in products for name in names) or len(set(names)) != len(names):
        raise ValueError("scope contains unknown or duplicate products")
    return set(names)


def validate(data, root=ROOT, *, require_complete=False, product=None):
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("tests"), list):
        raise ValueError("expected migration schema 1 and a tests array")
    required = inventory(root)
    archived = frozen_inventory(root) is not None
    products = {path.parent.name for path in (root / "products").glob("*/product.json") if path.is_file()}
    # A source release may leave out an optional product; its ledger rows still name it.
    products |= set(product_scope.OPTIONAL)
    if product is not None and product not in products:
        raise ValueError(f"unknown product: {product}")
    seen = set()
    counts = Counter()
    errors = []
    for row in data["tests"]:
        path, case = row["path"], row.get("case")
        key = path, case
        label = f"{path}::{case}" if case else path
        if key in seen:
            errors.append(f"duplicate {label}")
        seen.add(key)
        if key not in required:
            errors.append(f"unknown legacy test {label}")
        elif row["source"] != required[key]:
            errors.append(f"wrong source group for {label}: expected {required[key]}")
        if not valid_path(path) or not (archived or (root / path).is_file()):
            errors.append(f"missing legacy test source: {path}")
        state, disposition, targets = row["status"], row["disposition"], row["targets"]
        counts[state] += 1
        if state == "retired":
            try:
                prior = retirement_status(row, products, root)
            except (KeyError, OSError, TypeError, ValueError) as error:
                errors.append(f"invalid product retirement for {label}: {error}")
            else:
                counts[f"retired_{prior}"] += 1
        elif "scope" in row:
            try:
                consumers = scope_products(row["scope"], products)
            except ValueError as error:
                errors.append(f"invalid scope for {label}: {error}")
            else:
                if product is not None:
                    if consumers is None or product in consumers:
                        counts[f"scoped_{state}"] += 1
                    else:
                        counts["outside_scope"] += 1
        else:
            counts["unclassified"] += 1
        if state not in {"pending", "complete", "retired"} or disposition not in {"move", "adapt", "replace", "retire"}:
            errors.append(f"bad status or disposition for {label}")
        if state != "retired" and "retirement" in row:
            errors.append(f"unexpected product retirement metadata for {label}")
        packages = row.get("packages", [])
        if not packages or len(set(packages)) != len(packages) or any(
            not re.fullmatch(r"[A-Z][A-Z0-9]*(?:-[a-zA-Z0-9]+)*", p) for p in packages
        ):
            errors.append(f"missing or invalid work packages for {label}")
        if disposition == "retire":
            if targets or not row.get("reason"):
                errors.append(f"retirement for {label} needs a reason and no destination")
        elif not targets:
            errors.append(f"missing destination for {label}")
        keys = set()
        for target in targets:
            name = target["test"]
            if not valid_path(target["component"]) or target["package"] not in packages or (
                name is not None and not valid_path(name)
            ):
                errors.append(f"invalid destination component/package for {label}")
                continue
            target_key = target["component"], name
            if target_key in keys:
                errors.append(f"duplicate destination for {label}: {target_key}")
            keys.add(target_key)
            source = destination_source(root, target)
            counts["located" if source else "planned"] += 1
            if state == "complete":
                if source is None:
                    errors.append(f"unregistered destination for {label}: {name}")
                    continue
                marker = f"Replaces legacy: {label}"
                if not any(
                    line.strip(" \t*/#") == marker for line in source.read_text(encoding="utf-8").splitlines()[:80]
                ):
                    errors.append(f"missing destination evidence marker for {label}: {source.relative_to(root)}")
    for path, case in sorted(required.keys() - seen):
        errors.append(f"missing {path}" + (f"::{case}" if case else ""))
    if require_complete:
        if product is None and counts["pending"]:
            errors.append(f"cutover blocked: {counts['pending']} test migrations pending")
        elif product is not None:
            if counts["unclassified"]:
                errors.append(f"cutover blocked for {product}: {counts['unclassified']} rows lack reviewed scope")
            if counts["scoped_pending"]:
                errors.append(f"cutover blocked for {product}: {counts['scoped_pending']} test migrations pending")
            if not (counts["scoped_pending"] or counts["scoped_complete"]):
                errors.append(f"cutover blocked for {product}: no reviewed product or common rows")
    return counts, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-complete", action="store_true", help="require completed migrations for the selected scope")
    parser.add_argument("--product", metavar="PRODUCT", help="scope the cutover to this product and common rows")
    parser.add_argument("--freeze", action="store_true", help=f"write {FROZEN} from the current tree")
    args = parser.parse_args()
    if args.freeze:
        data = freeze(ROOT)
        print(f"Froze {len(data['tests'])} legacy tests at {data['commit']}")
    try:
        counts, errors = validate(json.loads((ROOT / "tests/migration.json").read_text(encoding="utf-8")),
                                  require_complete=args.require_complete, product=args.product)
    except (KeyError, OSError, SyntaxError, TypeError, ValueError) as error:
        errors, counts = [str(error)], Counter()
    print(f"Migration: {counts['complete']} complete, {counts['pending']} pending, "
          f"{counts['retired']} product-retired (previously {counts['retired_complete']} complete / "
          f"{counts['retired_pending']} pending); "
          f"{counts['located']} destinations located, {counts['planned']} planned/unregistered")
    if args.product is not None:
        print(f"Scope {args.product}: {counts['scoped_complete']} complete, {counts['scoped_pending']} pending, "
              f"{counts['outside_scope']} outside scope, {counts['unclassified']} unclassified")
    for error in errors:
        print(error, file=sys.stderr)
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
