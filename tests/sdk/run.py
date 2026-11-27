#!/usr/bin/env python3
"""Build and run the SDK host tests.

  python3 tests/sdk/run.py                      every component found, then every board package
  python3 tests/sdk/run.py xiaotai_media ...    these components and what they depend on
  python3 tests/sdk/run.py --board lckfb/szpi-esp32s3 ...
                                                these board packages (boards/<vendor>/<board>)
  python3 tests/sdk/run.py --all-boards         every board package, no component run

Configures tests/sdk in a fresh temporary directory (honoring TMPDIR), builds
it (Ninja when available), and runs CTest. Board packages share the component name
xiaotai_board, so each board is a build of its own: the board, what it depends
on, and the board's tests (tests of its dependencies belong to the component
run). Their tests are reported as <vendor>/<board>:<test>. Nothing is written
into the source tree and every build has its own directory, so any number of
runs can go on at once. If the selection contains stack tests, a second
unsanitized build runs only those tests. Exits non-zero when configuring, building or any test
fails; the output of the failing step, including the output of each failing
test, is printed.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

import host_roles

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*$")
BOARD = re.compile(r"^[a-z0-9][a-z0-9_.-]*/[a-z0-9][a-z0-9_.-]*$")
TEST_LINE = re.compile(r"Test\s+#\d+:\s+(\S+)")


def step(title, command, cwd=None, env=None, show=False):
    """Run one step; print its output when it fails (or when asked to)."""
    result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace")
    if result.returncode != 0 or show:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
    if result.returncode != 0:
        print(f"FAIL: SDK host {title} (exit {result.returncode})", flush=True)
    return result


def boards_found(root):
    """Board packages of a tree as <vendor>/<board>, sorted."""
    return sorted(path.parent.relative_to(root / "boards").as_posix()
                  for path in (root / "boards").glob("*/*/CMakeLists.txt"))


def build(args, cmake, ctest, base, components, roles, board=None):
    """One configure, build and test; the names of the tests that ran, or None after a failure."""
    directory = tempfile.mkdtemp(prefix="xiaotai-sdk-host-", dir=base)
    what = f" ({board})" if board else ""
    try:
        ninja = shutil.which("ninja")
        # A board build tests the board only: its dependencies have their own run.
        dependency_tests = "OFF" if args.skip_dependency_tests or board else "ON"
        configure = [cmake, "-S", str(ROOT / "tests/sdk"), "-B", directory, *(["-G", "Ninja"] if ninja else []),
                     f"-DXIAOTAI_COMPONENTS={';'.join(components)}", f"-DXIAOTAI_BOARD={board or ''}",
                     f"-DXIAOTAI_HOST_DEPENDENCY_TESTS={dependency_tests}",
                     f"-DXIAOTAI_HOST_ALLOW_MISSING={'ON' if args.allow_missing else 'OFF'}",
                     f"-DXIAOTAI_HOST_PYTHON={sys.executable}"]
        for role, path in roles.items():
            configure.append(f"-DXIAOTAI_HOST_VERIFIED_{role}={path or ''}")
        if args.root:
            configure.append(f"-DXIAOTAI_ROOT={args.root.resolve()}")
        if step(f"configure{what}", configure, show=args.verbose).returncode != 0:
            return None
        # Keep going after the first error so one run reports every broken target.
        keep_going = ["--", "-k", "0"] if ninja else ["--", "-k"]
        compile_ = [cmake, "--build", directory, "--parallel", str(args.jobs), *keep_going]
        if step(f"build{what}", compile_, show=args.verbose).returncode != 0:
            return None
        # Detached worker threads outlive main by design, so leak detection is off;
        # the sanitizers still report every memory and undefined-behaviour error.
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"}
        if not args.verbose:
            env["XIAOTAI_HOST_LOG_QUIET"] = "1"
        tests = [ctest, "--output-on-failure", "--no-tests=error", "--parallel", str(args.jobs),
                 *(["-V"] if args.verbose else [])]
        result = step(f"tests{what}", tests, cwd=directory, env=env, show=args.verbose)
        if result.returncode != 0:
            return None
        names = set(TEST_LINE.findall(result.stdout))
        skipped_file = Path(directory) / "host-skipped.txt"
        skipped = skipped_file.read_text(encoding="utf-8").splitlines() if skipped_file.is_file() else []
        stack_manifest = Path(directory) / "host-stack-tests.txt"
        if stack_manifest.is_file():
            expected_stack = set(stack_manifest.read_text(encoding="utf-8").splitlines())
            stack_dir = str(Path(directory) / "stack")
            stack_configure = [stack_dir if value == directory else value for value in configure]
            stack_configure.append("-DXIAOTAI_HOST_SANITIZE=OFF")
            if step(f"stack configure{what}", stack_configure, show=args.verbose).returncode != 0:
                return None
            stack_compile = [stack_dir if value == directory else value for value in compile_]
            if step(f"stack build{what}", stack_compile, show=args.verbose).returncode != 0:
                return None
            result = step(f"stack tests{what}", [*tests, "-L", "^stack$"], cwd=stack_dir, env=env, show=args.verbose)
            if result.returncode != 0:
                return None
            ran_stack = set(TEST_LINE.findall(result.stdout))
            if ran_stack != expected_stack or names & ran_stack:
                print(f"FAIL: SDK host stack registration{what}: expected {sorted(expected_stack)}, "
                      f"ran {sorted(ran_stack)}, duplicate {sorted(names & ran_stack)}", flush=True)
                return None
            names.update(ran_stack)
        names = sorted(names)
        if board:
            names = [f"{board}:{name}" for name in names]
            skipped = [f"{board}:{name}" for name in skipped]
        return names, skipped
    finally:
        if args.keep:
            print(f"build directory kept{what}: {directory}", flush=True)
        else:
            shutil.rmtree(directory, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("components", nargs="*", metavar="component",
                        help="component directory names, for example xiaotai_media (default: all)")
    parser.add_argument("--board", action="append", default=[], metavar="VENDOR/BOARD",
                        help="a board package under boards/ (repeatable)")
    parser.add_argument("--all-boards", action="store_true", help="every board package under boards/")
    parser.add_argument("--skip-dependency-tests", action="store_true",
                        help="build dependencies but run only the named components' tests")
    parser.add_argument("--allow-missing", action="store_true",
                        help="report and skip tests needing sources that are not prepared")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--keep", action="store_true", help="keep the build directory and print its path")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="show all build and test output, and let the host port log")
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)  # another component tree (helper self-test)
    args = parser.parse_args(argv)

    for name in args.components:
        if not COMPONENT.match(name):
            parser.error(f"not a component name: {name}")
    for name in args.board:
        if not BOARD.match(name):
            parser.error(f"not a board package (<vendor>/<board>): {name}")
    cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
    if os.name != "posix" or not cmake or not ctest:
        print("FAIL: the SDK host build needs a POSIX host (Linux or WSL) with cmake and ctest", flush=True)
        return 2

    root = args.root.resolve() if args.root else ROOT
    everything = not args.components and not args.board and not args.all_boards
    boards = boards_found(root) if everything or args.all_boards else []
    boards += [name for name in args.board if name not in boards]
    base = tempfile.gettempdir()
    started = time.monotonic()
    roles = {}
    missing = []
    if everything and not args.root:
        for role in host_roles.host_components.ALL_SET:
            try:
                roles[role] = host_roles.role_path(role)
            except host_roles.MissingRole as error:
                if not args.allow_missing:
                    print(f"FAIL: {error}", flush=True)
                    return 1
                roles[role] = None
                missing.append(role)
            except (host_roles.host_components.ComponentError, OSError) as error:
                print(f"FAIL: {error}", flush=True)
                return 1
    names, skipped = [], []
    if everything or args.components:
        ran = build(args, cmake, ctest, base, args.components, roles)
        if ran is None:
            return 1
        names += ran[0]
        skipped += ran[1]
    for board in boards:
        ran = build(args, cmake, ctest, base, [], roles, board)
        if ran is None:
            return 1
        names += ran[0]
        skipped += ran[1]
    scope = " ".join(args.components) or ("all components" if everything else "")
    if boards:
        scope = f"{scope}{'; ' if scope else ''}{len(boards)} board{'s' if len(boards) != 1 else ''}"
    for test in sorted(set(skipped)):
        print(f"SKIP: {test}", flush=True)
    if missing:
        print(f"MISSING: {', '.join(missing)}", flush=True)
    verdict = "PARTIAL" if missing or skipped else "PASS"
    print(f"{verdict}: SDK host build and {len(names)} tests ({scope}) in {time.monotonic() - started:.0f}s: "
          f"{' '.join(names)}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
