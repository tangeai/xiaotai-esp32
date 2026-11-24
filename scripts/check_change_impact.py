#!/usr/bin/env python3
"""Plan cross-product checks from changed paths. Does not build, flash, or certify isolation."""
import argparse
import json
import re
import posixpath
from pathlib import Path, PurePosixPath
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

SDK_GRAPH_INPUTS = {"engine.json", "theme.json", "module.json", "application.json", "CMakeLists.txt", "Kconfig"}
SDK_BUILD_CONTRACTS = {
    "scripts/build.py", "scripts/xiaotai_build.py", "scripts/xiaotai_build_native.py", "scripts/idf_environment.py",
    "scripts/sdk_selection.py", "scripts/flash_layout.py", "scripts/elf_static_memory.py",
}


def normalized(path):
    value = str(path).replace("\\", "/")
    parts = PurePosixPath(value).parts
    if not parts or value.startswith("/") or ":" in value or ".." in parts:
        raise ValueError(f"Expected repository-relative path: {path}")
    return str(PurePosixPath(value))


def inside(path, directory):
    return path == directory or path.startswith(directory + "/")


def package_roots(root, target, sdk):
    """Map each ESP-IDF line a product supports to its TiRTC package directory."""
    table = json.loads((root / "components/tirtc_sdk/packages.json").read_text(encoding="utf-8"))
    result = {}
    for line, choice in sdk.items():
        name = f"generic-{target}" if choice == "generic" else choice
        entry = table["lines"][line]["packages"][name]
        # A downloaded official package is owned by its build contract (the archive
        # is not in the tree); an in-tree package owns the directory of its lib and headers.
        if "path" in entry:
            owned = entry["contract"]
        else:
            owned = posixpath.commonpath([entry["lib"], entry["include"]]) or entry["lib"]
        result[line] = {"package": f"{line}/{name}", "root": "components/tirtc_sdk/" + owned,
                        "default": line == table["default_line"]}
    return result


def load_sdk_products(root):
    """Use the same manifest resolver as the SDK product generator."""
    import sdk_selection as selection
    import xiaotai_build_native as native

    result = {}
    for path in sorted((root / "products").glob("*/product.json")):
        product = selection.load_product(path)
        name = product["name"]
        if path.parent.name != name or name in result:
            raise ValueError(f"Product directory must match a unique name: {path}")
        plan = selection.plan(root, product)
        roots = set(plan["components"].values())
        variants = sorted(product.get("variants", {}))
        for variant in variants:
            alternate = selection.plan(root, selection.apply_variant(product, variant))
            roots.update(alternate["components"].values())
        # Omitted package declarations mean generic on the board's supported lines.
        sdk = plan["tirtc"] if "tirtc" in product else dict.fromkeys(plan["units"][2].get("idf", []), "generic")
        inputs = {}
        for line in native.common.sdk_lines(root)["lines"]:
            profile_path = native.profile_path(root, name, line=line)
            if not profile_path.is_file():
                continue
            profile = native.load_profile(name, root, line=line)
            if (profile["board"], profile["target"]) != (plan["board"], plan["target"]) or line not in sdk:
                raise ValueError(f"Native profile does not match SDK product: {profile_path}")
            native_files = [profile_path, root / "profiles" / f"{name}{native.line_suffix(line, root)}-native.yml",
                            native.dependency_lock_path(root, name, line=line)]
            native_files.extend(native.dependency_lock_path(root, name, variant, line=line) for variant in variants
                                if product["variants"][variant].get("without"))
            inputs.update((file.relative_to(root).as_posix(), line) for file in native_files)
        result[name] = {"name": name, "board": normalized(plan["board"]), "target": plan["target"],
                        "source_roots": sorted(normalized(p) for p in roots), "sdk": sdk,
                        "sdk_packages": package_roots(root, plan["target"], sdk),
                        "features": plan["modules"], "variants": variants,
                        "product_manifest": path.relative_to(root).as_posix(), "native_inputs": inputs}
    if not result:
        raise ValueError("No SDK product manifests found")
    return result


# The legacy board graph is gone; every product is an SDK product.
load_products = load_sdk_products


def git_paths(root, base=None):
    def run(*args):
        return subprocess.run(["git", "-C", str(root), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    # --no-renames keeps both sides of moves, including cross-board moves.
    commit = run("rev-parse", "--verify", "--end-of-options", (base or "HEAD") + "^{commit}").decode("ascii").strip()
    changes = run("diff", "--no-ext-diff", "--no-renames", "--name-only", "-z", commit, "--")
    added = run("ls-files", "--others", "--exclude-standard", "-z")
    return sorted({normalized(p.decode("utf-8")) for p in (changes + added).split(b"\0") if p})


def classify(path, products, graph="sdk"):
    names = set(products)
    if path.startswith("docs/") or path in {"README.md", "README.en.md", "LICENSE", "THIRD_PARTY.md", "CHANGELOG.md"}:
        return set(), "documentation", "Check links, commands and claims; no firmware inferred", False
    if path.startswith("tests/"):
        return names, "test-infrastructure", "Run host checks; verify tests were not weakened", False
    if graph == "sdk" and (path in SDK_BUILD_CONTRACTS or (
            path.startswith(("sdk/", "apps/")) and PurePosixPath(path).name in SDK_GRAPH_INPUTS)):
        return names, "contract", "SDK build or graph input; compare the affected native configurations", True
    if path.startswith("scripts/"):
        return names, "build-test-tooling", "Shared tooling: host checks and each product's dry-run", False
    if path == "config/adapter-contracts.json" or path.startswith("config/capabilities/"):
        return names, "contract", "Consumer graph changed: revalidate every product", False
    if path.startswith("components/tirtc_sdk/"):
        users = {n for n, p in products.items() if sdk_lines_touched(path, p)}
        if users:
            packages = sorted({e["package"] for p in products.values() for e in p["sdk_packages"].values()
                               if inside(path, e["root"])})
            return users, "sdk", f"TiRTC package consumer: {', '.join(packages)}", False
        return names, "sdk", "SDK headers, selector or unrecognized package: inspect all consumers", True
    if graph == "sdk":
        manifests = {n for n, p in products.items() if path == p["product_manifest"]}
        if manifests:
            return manifests, "product", "Selected SDK product manifest", False
        native_users = {n for n, p in products.items() if path in p["native_inputs"]}
        if native_users:
            return native_users, "dependencies", "Native product profile, manifest or dependency lock", False
        if (path.startswith("profiles/native-firmware/") or
                (path.startswith("profiles/") and path.endswith("-native.yml")) or
                (path.startswith("profiles/dependency-locks/") and
                 (re.search(r"-native(?:-idf[0-9]+\.[0-9]+)?\.lock$", path) or "--" in PurePosixPath(path).name))):
            return names, "unmapped", "Native dependency input has no current product; compare both revisions", True
    if path.startswith("main/") or path in {"CMakeLists.txt"}:
        return names, "unmapped", "The legacy entry was removed; nothing builds this path", True
    board_root = "boards/" if graph == "sdk" else "main/boards/"
    board_users = {n for n, p in products.items() if inside(path, board_root + p["board"])}
    if board_users:
        return board_users, "board", "Selected board ownership; review external includes before narrowing", False
    source_users = {n for n, p in products.items() if any(inside(path, d) for d in p["source_roots"])}
    if source_users:
        if graph == "sdk":
            return source_users, "profile-source", "Resolved SDK components and variants; review indirect includes", False
        return source_users, "profile-source", "Declared board.json source consumers; review indirect includes", False
    for name, product in products.items():
        if path == f"profiles/dependency-locks/{name}.lock" or any(
                path == f"profiles/dependency-locks/idf{line}/{name}.lock" for line in product["sdk_packages"]):
            return {name}, "dependencies", "Product dependency lock", False
    if path.startswith(("main/boards/common/", "components/", "cmake/", "config/", "profiles/")) or "/shared/" in path or path in {"CMakeLists.txt", "main/CMakeLists.txt", "main/Kconfig.projbuild"}:
        return names, "shared", "Shared interface, dependency or build configuration", False
    return names, "unmapped", "No trustworthy consumer mapping: retain all products for review", True


def sdk_lines_touched(path, product):
    return sorted(line for line, entry in product.get("sdk_packages", {}).items() if inside(path, entry["root"]))


def lines_touched(path, product, graph="sdk"):
    """ESP-IDF lines whose inputs this path belongs to: package, lock or board overlay."""
    lines = set(sdk_lines_touched(path, product))
    if graph == "sdk" and path in product["native_inputs"]:
        lines.add(product["native_inputs"][path])
    board_root = "boards" if graph == "sdk" else "main/boards"
    for line in product.get("sdk_packages", {}):
        if path in (f"profiles/dependency-locks/idf{line}/{product['name']}.lock",
                    f"{board_root}/{product['board']}/sdkconfig.idf{line}"):
            lines.add(line)
    return sorted(lines)


def analyze(paths, products, representative=None, graph="sdk"):
    rows, affected, kinds = [], set(), set()
    for path in sorted({normalized(p) for p in paths}):
        users, kind, reason, review = classify(path, products, graph)
        affected.update(users)
        kinds.add(kind)
        rows.append({"path": path, "kind": kind, "products": sorted(users),
                     "reason": reason, "manual_review": review})
    if representative is not None and representative not in affected:
        raise ValueError("Representative must be an affected product")
    # Consumer coverage is not an instruction to repeat hardware tests on every chip.
    board_specific = set().union(*(set(row["products"]) for row in rows
        if row["kind"] in {"board", "product"} or (row["kind"] == "profile-source" and len(row["products"]) == 1)))
    shared_runtime = any(row["kind"] in {"shared", "contract", "unmapped", "dependencies"}
                         or (row["kind"] == "profile-source" and len(row["products"]) > 1)
                         for row in rows)
    if representative is not None and not shared_runtime and representative not in board_specific:
        raise ValueError("These paths do not call for a representative hardware run")
    selected_hardware = board_specific | ({representative} if representative is not None else set())
    matrix = []
    for name in sorted(affected):
        product = products[name]
        local_kinds = {row["kind"] for row in rows if name in row["products"]}
        firmware = bool(local_kinds - {"test-infrastructure", "build-test-tooling"})
        # A change to a non-default line's package, lock or overlay needs that line's build too.
        extra_lines = sorted({line for row in rows if name in row["products"]
                              for line in lines_touched(row["path"], product, graph)
                              if not product["sdk_packages"][line]["default"]})
        if graph == "sdk":
            preview = ["python", "scripts/sdk_selection.py", "--product", product["product_manifest"]]
            commands = [preview, *(preview + ["--variant", variant] for variant in product["variants"])]
        else:
            build = ["python", "scripts/build.py", product["board"], "--name", name, "--entry", "legacy"]
            commands = [build + ["--dry-run"]]
            if firmware:
                commands.append(build)
            for line in extra_lines:
                commands.append(build + ["--idf", line])
        matrix.append({"product": name, "board": product["board"], "target": product["target"],
            "sdk": product.get("sdk"), "commands": commands,
            "firmware_build_required": firmware, "tests_executed": False,
            "hardware_run_required": name in selected_hardware,
            "hardware_scope": ("board-specific" if name in board_specific else
                               "representative" if name == representative else
                               "candidate-only" if firmware and shared_runtime else "none"),
            "hardware_checks": (["identity_boot", "network_binding", "affected_features",
                                  "repeat_start_stop", "failure_recovery", "resource_watermarks"]
                                if name in selected_hardware else []),
            "feature_candidates": product.get("features", []),
            "sdk_extra_checks": (["approved_dependency_identity", "archive_headers_abi", "elf_link_identity"]
                                 if "sdk" in local_kinds else [])})
        if graph == "sdk":
            matrix[-1]["extra_idf_lines"] = extra_lines
    result = {"schema": 1, "scope_kind": "conservative-planning-not-build-graph-proof",
            "tests_executed": False, "manual_review_required": any(r["manual_review"] for r in rows),
            "hardware_selection_required": shared_runtime and representative is None,
            "selected_hardware_products": sorted(selected_hardware),
            "sdk_change_requires_authorization": "sdk" in kinds,
            "changed_files": rows, "affected_products": sorted(affected), "matrix": matrix,
            "host_command": ["python", "tests/run_host.py"] if affected else None,
            "limitations": ["Indirect includes and generated sources require compile-graph review.",
                            "A product absent from this plan is not certified unaffected.",
                            "Tooling changes require builds if dry-run or effective inputs change.",
                            "Shared application paths use one representative; inspect actual hardware/ABI differences separately.",
                            "SDK changes require an approved dependency, not local SDK patches or repeated pressure matrices.",
                            "Hardware actions require the existing task authorization; no actions executed."]}
    if graph == "sdk":
        result["graph"] = graph
        result["host_command"] = ["python", "tests/test_products.py", "--require-products"] if affected else None
        result["limitations"].append("SDK commands preview selection only; use the independent ESP-IDF project for native builds.")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--graph", choices=("sdk",), default="sdk",
                        help="the SDK product manifests (the only graph since the legacy entry was removed)")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--files", nargs="+", help="Exact task paths; does not cover other dirty changes")
    selection.add_argument("--base", help="Compare this Git ref to current working tree, plus untracked files")
    parser.add_argument("--representative", help="Affected product selected for shared application runtime checks")
    parser.add_argument("--output", type=Path, help="Create a new JSON evidence file; never overwrite")
    parser.add_argument("--strict", action="store_true", help="Exit 2 when unknown paths need review")
    args = parser.parse_args()
    try:
        root = args.repo.resolve(strict=True)
        products = load_sdk_products(root)
        paths = args.files if args.files is not None else git_paths(root, args.base)
        result = analyze(paths, products, args.representative, args.graph)
        result["input_scope"] = "explicit-task-paths" if args.files else f"worktree-versus-{args.base or 'HEAD'}"
        payload = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("x", encoding="utf-8") as destination:
                destination.write(payload)
        print(payload)
        return 2 if args.strict and result["manual_review_required"] else 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"Impact check failed; scope is NOT cleared: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
