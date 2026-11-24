#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""List what a new ESP-IDF line or a new chip has to touch in this checkout.

    python .claude/skills/xiaotai-port/scripts/port_inventory.py --line 6.1
    python .claude/skills/xiaotai-port/scripts/port_inventory.py --target esp32s31
    python .claude/skills/xiaotai-port/scripts/port_inventory.py --line 6.1 --target esp32s31 --strict

Each row is [有] already registered, [缺] has to be added before a build can
pass, or [查] a place to read and decide. Chip tables and line branches are
found by scanning, not from a fixed list, so a table added later still shows
up. Read-only: nothing is written, downloaded or built. --strict exits 1 when
any [缺] row remains.
"""

import argparse
import ast
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SKIP_DIRS = {".git", "build", "managed_components", "packages", "vendor", "__pycache__", "node_modules"}
CODE_DIRS = ("sdk", "components", "boards", "cmake")
TOOL_DIRS = ("scripts", "tools")
LIMIT = 40
rows = []


def say(kind, text):
    rows.append(kind)
    print(f"  [{kind}] {text}")


def head(text):
    print(f"\n== {text}")


def rel(path):
    return path.relative_to(ROOT).as_posix()


def walk(bases, suffixes=(), prefixes=()):
    for base in bases:
        for path in sorted((ROOT / base).rglob("*")):
            if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts) or not path.is_file():
                continue
            if path.suffix in suffixes or (prefixes and path.name.startswith(prefixes)):
                yield path


def text(path):
    return path.read_text(encoding="utf-8-sig", errors="replace")


def is_test(path):
    parts = path.relative_to(ROOT).parts
    return "test" in parts or "tests" in parts or path.name.startswith("test_") or path.stem.endswith("_test")


def grep(paths, pattern, limit=0):
    limit = max(limit, LIMIT)
    regex = re.compile(pattern)
    hits = []
    for path in paths:
        for number, line in enumerate(text(path).splitlines(), 1):
            if regex.search(line):
                hits.append(f"{rel(path)}:{number}: {line.strip()[:110]}")
    for hit in hits[:limit]:
        say("查", hit)
    if len(hits) > limit:
        say("查", f"……还有 {len(hits) - limit} 处没列出，加 --all 看全部")
    return hits


def module_literals(path):
    """(name, line, value) for every module-level assignment whose value is a literal."""
    try:
        tree = ast.parse(text(path))
    except SyntaxError:
        return []
    found = []
    for node in tree.body:
        if not isinstance(node, ast.Assign) or not all(isinstance(t, ast.Name) for t in node.targets):
            continue
        value = node.value
        if isinstance(value, ast.Call) and getattr(value.func, "id", "") in {"set", "frozenset", "tuple"} and value.args:
            value = value.args[0]
        try:
            found.append((node.targets[0].id, node.lineno, ast.literal_eval(value)))
        except (ValueError, SyntaxError, TypeError):
            continue
    return found


def named_literal(path, name):
    return next((value for key, _, value in module_literals(path) if key == name), None)


def literal_tables(path, names):
    """Module-level literal sets, tuples and dicts that list at least two of names."""
    found = []
    for name, lineno, data in module_literals(path):
        keys = set(data) if isinstance(data, (dict, set, frozenset, list, tuple)) else set()
        keys = {key for key in keys if isinstance(key, str)}
        if len(keys & names) >= 2:
            found.append((name, lineno, keys))
    return found


def load_json(path):
    return json.loads(text(path))


def products(packages):
    """(product, line, profile path) for every native profile.

    The line comes from the profile itself: a profile in the top folder is the
    product's own default and may name a line other than the repository default
    (a chip only a newer line supports)."""
    base = ROOT / "profiles/native-firmware"
    result = []
    for path in sorted(base.glob("*.json")) + sorted(base.glob("idf*/*.json")):
        fallback = packages["default_line"] if path.parent == base else path.parent.name[3:]
        result.append((path.stem, load_json(path).get("idf_line", fallback), path))
    return result


def boards():
    result = {}
    for path in sorted((ROOT / "boards").rglob("board.json")):
        data = load_json(path)
        result[path.parent.relative_to(ROOT / "boards").as_posix()] = data
    return result


def board_of(board_id, known):
    if board_id in known:
        return known[board_id]
    matches = [data for key, data in known.items() if key.endswith("/" + board_id)]
    return matches[0] if len(matches) == 1 else {}


def check_line(line, packages, known, target):
    head(f"ESP-IDF {line} 线：版本和 TiRTC 包")
    lock_versions = set(named_literal(ROOT / "scripts/idf_environment.py", "LOCKS") or {})
    versions = sorted(v for v in lock_versions if v.startswith(line + "."))
    if versions:
        say("有", f"scripts/idf_environment.py LOCKS：{', '.join(versions)}")
    else:
        say("缺", f"scripts/idf_environment.py LOCKS 没有 {line}.x：装好官方发布版后登记版本和官方提交号")
    entry = packages["lines"].get(line)
    if not entry:
        say("缺", f"components/tirtc_sdk/packages.json 没有 \"{line}\" 线：写 idf_versions（第一个就是契约要求的版本）、origin、packages")
    else:
        say("有", f"packages.json \"{line}\" 线，idf_versions={entry.get('idf_versions')}")
        for version in entry.get("idf_versions", []):
            if version not in lock_versions:
                say("缺", f"packages.json 的 {version} 不在 LOCKS 里")
    # Each product uses its chip's generic package unless product.json names another for this line.
    wanted = {}
    for path in sorted((ROOT / "products").glob("*/product.json")):
        product = load_json(path)
        board = board_of(product.get("board", ""), known)
        if line in board.get("idf", []) and board.get("target"):
            key = (product.get("tirtc") or {}).get(line, f"generic-{board['target']}")
            wanted.setdefault(key, (board["target"], []))[1].append(product.get("name", path.parent.name))
    if not wanted:
        # Nothing declares the line yet: estimate from every product, each on its chip's generic package.
        say("查", f"还没有板子的 board.json 声明 {line}；下面按现有全部产品估计要哪些包")
        for path in sorted((ROOT / "products").glob("*/product.json")):
            product = load_json(path)
            board = board_of(product.get("board", ""), known)
            if board.get("target"):
                wanted.setdefault(f"generic-{board['target']}", (board["target"], []))[1].append(
                    product.get("name", path.parent.name))
    if target:
        wanted.setdefault(f"generic-{target}", (target, []))
    for key, (chip, users) in sorted(wanted.items()):
        check_package(line, chip, entry, key, users)
    if entry and entry.get("packages"):
        extra = sorted(set(entry["packages"]) - set(wanted))
        if extra:
            say("查", f"这条线还登记了没有产品用的包：{', '.join(extra)}")

    head(f"ESP-IDF {line} 线：产品档案、组件清单、依赖锁")
    default = packages["default_line"]
    by_product = {}
    for name, profile_line, path in products(packages):
        by_product.setdefault(name, {})[profile_line] = path
    for name, lines in sorted(by_product.items()):
        any_profile = load_json(next(iter(lines.values())))
        board = board_of(any_profile.get("board", ""), known)
        declared = line in board.get("idf", [])
        suffix = "" if line == default else f"-idf{line}"
        lock_dir = "profiles/dependency-locks" + ("" if line == default else f"/idf{line}")
        files = {
            "档案": (lines.get(line), f"profiles/native-firmware/{'' if line == default else f'idf{line}/'}{name}.json"
                                    + ("" if line == default else f"（只在 {line} 上编的产品放 profiles/native-firmware/{name}.json）")),
            "组件清单": (ROOT / f"profiles/{name}{suffix}-native.yml", f"profiles/{name}{suffix}-native.yml"),
            "依赖锁": (ROOT / f"{lock_dir}/{name}-native.lock", f"{lock_dir}/{name}-native.lock"),
        }
        present = {label: bool(path and Path(path).is_file()) for label, (path, _) in files.items()}
        if not declared and not any(present.values()):
            say("查", f"{name}：board.json 的 idf 没有 {line}，要上这条线就加，并补下面三样")
            continue
        if not declared:
            say("缺", f"{name}：有 {line} 的文件，但 board.json 的 idf 没声明 {line}")
        for label, (path, shown) in files.items():
            if present[label]:
                if label == "档案":
                    profile = load_json(path)
                    problems = []
                    if profile.get("idf_line") != line:
                        problems.append(f"idf_line={profile.get('idf_line')}")
                    if profile.get("idf_version") not in lock_versions:
                        problems.append(f"idf_version={profile.get('idf_version')} 不在 LOCKS")
                    chosen = str(profile.get("tirtc_package", ""))
                    if not chosen.startswith(line + "/"):
                        problems.append(f"tirtc_package={chosen}")
                    else:
                        # CMake requires the active IDF to equal the package contract's idf_version.
                        need = contract_version(line, chosen.split("/", 1)[1], packages)
                        if need and need != profile.get("idf_version"):
                            problems.append(f"idf_version={profile.get('idf_version')}，包的契约要求 {need}")
                    say("缺" if problems else "有", f"{name} {label} {shown}" + (f"：{'；'.join(problems)}" if problems else ""))
                else:
                    say("有", f"{name} {label} {shown}")
            else:
                say("缺", f"{name} {label} {shown}（复制现有线的再改；锁只解析、不编译生成，审过差异再放进来）")

    head(f"ESP-IDF {line} 线：按线记录的审过源码哈希")
    keyed = re.compile(r"_SHA256_(\d+\.\d+)\b")
    for path in walk(CODE_DIRS, (".cmake", ".txt")):
        found = set(keyed.findall(text(path)))
        if found:
            if line in found:
                say("有", f"{rel(path)} 有 {line} 的哈希")
            else:
                say("缺", f"{rel(path)} 只审过 {', '.join(sorted(found))}：读 {line} 的 IDF 源码、和已审的比，确认行为后才登记新哈希")

    head("按版本线或 IDF 版本写的分支（新线默认走哪边，逐个判断）")
    tool_files = [p for p in walk(TOOL_DIRS, (".py",)) if not is_test(p)] + [ROOT / "tests/verify_build.py"]
    grep(tool_files, r"""(line|idf_line)["']?[\])]?\s*[!=]=\s*["']\d+\.\d+["']|version\("idf-component-manager"\)""")
    code_files = [p for p in walk(CODE_DIRS, (".c", ".h", ".cc", ".cmake", ".txt"), ("Kconfig",)) if not is_test(p)]
    grep(code_files, r"IDF_VERSION_(MAJOR|MINOR|PATCH)|ESP_IDF_VERSION_VAL|STREQUAL \"\d+\.\d+\.\d+\"")


def contract_path(line, chip, package):
    # Downloaded packages name their contract; an in-tree build keeps it beside its library.
    default = package["lib"] + "/contract.json" if "lib" in package else f"contracts/{line}/{chip}.json"
    return ROOT / "components/tirtc_sdk" / package.get("contract", default)


def contract_version(line, key, packages):
    package = packages["lines"].get(line, {}).get("packages", {}).get(key)
    path = contract_path(line, key.rsplit("-", 1)[-1], package) if package else None
    return load_json(path).get("idf_version") if path and path.is_file() else None


def check_package(line, chip, entry, key=None, users=()):
    key = key or f"generic-{chip}"
    packages = (entry or {}).get("packages", {})
    if key not in packages:
        who = f"（{', '.join(users)} 要用）" if users else ""
        say("缺", f"{line} 线没有 {key} 包{who}：TiRTC 要出一份用这条线的 IDF 和工具链编的 {chip} 包")
        return
    contract = contract_path(line, chip, packages[key])
    package = packages[key]
    if not contract.is_file():
        say("缺", f"{rel(contract)}：下载包后用 scripts/tirtc_contract.py 生成")
        return
    data = load_json(contract)
    allowed = (entry or {}).get("idf_versions", [])
    if data.get("idf_version") not in allowed:
        say("缺", f"{rel(contract)} 的 idf_version={data.get('idf_version')} 不在这条线的 {allowed} 里：重新生成契约")
    else:
        facts = [f"用在 IDF {data.get('idf_version')}"]
        if data.get("producer_idf_version"):
            facts.append(f"编包 IDF {data['producer_idf_version']}")
        facts.append(str(data.get("toolchain_release", "工具链未写")))
        if data.get("libc_config"):
            facts.append(next(iter(data["libc_config"])).replace("CONFIG_LIBC_", "").lower())
        if data.get("crypto_mode"):
            facts.append(data["crypto_mode"])
        version = package.get("version") or ("仓库内" if "lib" in package else "?")
        say("有", f"{key} {version} → {rel(contract)}（{'，'.join(facts)}）")


def check_target(target, packages, known):
    chips = {data.get("target") for data in known.values() if data.get("target")}
    head(f"{target}：芯片表（扫出来的，含两个以上已知芯片的字面量表）")
    tables = 0
    for path in [p for p in walk(TOOL_DIRS, (".py",)) if not is_test(p)] + [ROOT / "tests/verify_build.py"]:
        for name, lineno, keys in literal_tables(path, chips):
            tables += 1
            where = f"{rel(path)}:{lineno} {name}"
            if target in keys:
                say("有", where)
            elif chips <= keys:
                # Lists every chip a board uses: a registry the new chip must join.
                say("缺", f"{where}（登记表，现有 {', '.join(sorted(keys & chips))}；值从官方 IDF 和 esptool 查，不猜）")
            else:
                say("查", f"{where}（只列了 {', '.join(sorted(keys & chips))}，按能力决定新芯片加不加）")
    if not tables:
        say("查", "没扫到芯片表，确认扫描范围")

    head(f"{target}：板和 TiRTC 包")
    own = sorted(key for key, data in known.items() if data.get("target") == target)
    if own:
        say("有", f"板：{', '.join(own)}")
    else:
        say("缺", "还没有这款芯片的板：芯片登记齐了以后用 xiaotai-new-board 接第一块")
    for line, entry in sorted(packages["lines"].items()):
        if f"generic-{target}" in entry.get("packages", {}):
            check_package(line, target, entry)
        else:
            say("查", f"{line} 线没有 generic-{target} 包（这条线要上这款芯片才需要）")

    head(f"{target}：组件清单里按芯片装的依赖")
    for path in walk(CODE_DIRS + ("profiles",), (".yml",)):
        for number, line in enumerate(text(path).splitlines(), 1):
            match = re.search(r"target\s+(not\s+)?in\s+\[([^\]]*)\]", line)
            if match:
                listed = {item.strip() for item in match.group(2).split(",")}
                kind = "有" if target in listed else "查"
                say(kind, f"{rel(path)}:{number}: {line.strip()}" + ("" if target in listed else "（新芯片装不上这项，确认组件是否支持它）"))

    head(f"{target}：代码里按芯片写的分支（按能力判断新芯片走哪边）")
    code_files = [p for p in walk(CODE_DIRS, (".c", ".h", ".cc", ".cmake", ".txt"), ("Kconfig",)) if not is_test(p)]
    grep(code_files, r"CONFIG_IDF_TARGET_|IDF_TARGET_ESP32|IDF_TARGET\s+STREQUAL|STREQUAL\s+\"esp32")
    tool_files = [p for p in walk(TOOL_DIRS, (".py",)) if not is_test(p)] + [ROOT / "tests/verify_build.py"]
    grep(tool_files, r"""(target|chip)["']?[\])]?\s*(==|!=|in)\s*\(?["']esp32\w*["']""")


def check_review_gates():
    head("审过源码才放行的地方（换线或组件升级后会在这里停下，读源码再登记，不能只抄哈希）")
    files = [p for p in walk(CODE_DIRS, (".cmake", ".txt")) if not is_test(p)]
    grep(files, r"FATAL_ERROR.*([Uu]nreviewed|reviewed|review)", limit=60)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--line", help="ESP-IDF line, e.g. 6.1")
    parser.add_argument("--target", help="IDF target name, e.g. esp32s31")
    parser.add_argument("--strict", action="store_true", help="exit 1 when anything is missing")
    parser.add_argument("--all", action="store_true", help="list every [查] hit")
    parser.add_argument("--repo", type=Path, help="checkout to scan (default: the one holding this script, "
                                                  "or the current directory when it is a checkout)")
    args = parser.parse_args(argv)
    if not args.line and not args.target:
        parser.error("give --line, --target or both")
    if args.line and not re.fullmatch(r"\d+\.\d+", args.line):
        parser.error("--line is MAJOR.MINOR, e.g. 6.1")
    global ROOT, LIMIT
    marker = Path("components/tirtc_sdk/packages.json")
    if args.repo:
        ROOT = args.repo.resolve()
    elif not (ROOT / marker).is_file() and (Path.cwd() / marker).is_file():
        ROOT = Path.cwd().resolve()
    if not (ROOT / marker).is_file():
        parser.error(f"{ROOT} is not a XiaoTai checkout; pass --repo")
    if args.all:
        LIMIT = 10 ** 6
    packages = load_json(ROOT / "components/tirtc_sdk/packages.json")
    known = boards()
    print(f"仓库 {ROOT}，默认线 {packages['default_line']}，已有线 {', '.join(sorted(packages['lines']))}")
    if args.line:
        check_line(args.line, packages, known, args.target)
    if args.target:
        check_target(args.target, packages, known)
    check_review_gates()
    missing = rows.count("缺")
    print(f"\n合计：缺 {missing}，查 {rows.count('查')}，有 {rows.count('有')}")
    return 1 if args.strict and missing else 0


if __name__ == "__main__":
    sys.exit(main())
