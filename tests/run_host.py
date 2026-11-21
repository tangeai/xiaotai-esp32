"""Run the repository host checks: build tooling, product graphs and SDK tests."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
IDF_SOURCE_TESTS = {
    "tests/test_port_usb_monitor.py",
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--p4-idf-path", type=Path, default=os.environ.get("IDF_PATH"),
                    help="ESP-IDF checkout for image and URL-parser tests (defaults to IDF_PATH)")
parser.add_argument("--idf", metavar="LINE", help="ESP-IDF line whose product locks select host components")
args = parser.parse_args()
if args.p4_idf_path is None or not (args.p4_idf_path / "tools/idf.py").is_file():
    parser.error("set --p4-idf-path or IDF_PATH to a valid ESP-IDF checkout")
out = ROOT / "build/host"
out.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(ROOT / "tests"))
import fetch_components  # noqa: E402
import host_components  # noqa: E402

if args.idf:
    os.environ[host_components.LINE_VARIABLE] = args.idf
try:
    host_components.selected_line()
except host_components.ComponentError as error:
    parser.error(str(error))
# Prepare locked component sources from local copies only; a missing one fails just its tests.
components = []
trusted = {}
try:
    identities, errors = fetch_components.identities_for(host_components.ALL_SET)
    results, _ = fetch_components.prepare(
        identities, isolated=os.environ.get(fetch_components.ISOLATED_VARIABLE) == "1")
    fetch_components.write_paths(identities, results)
    for identity in identities:
        result = results[identity.role]
        target = host_components.prepared_root() / identity.dirname
        components.append(dict(role=identity.role, component=identity.component, version=identity.version,
                               component_hash=identity.component_hash, status=result["status"],
                               source=result["source"], dir=target.as_posix()))
        digest = host_components.stamp_digest(identity) if result["status"] == "ready" else None
        if digest:
            trusted[target.as_posix()] = digest
    components += [dict(role=role, status="error", error=error) for role, error in errors.items()]
except Exception as error:  # report and keep running; dependent tests explain what is missing
    components.append(dict(role="*", status="error", error=f"{type(error).__name__}: {error}"))
ready = sum(c["status"] == "ready" for c in components)
print(f"host components: {ready}/{len(components)} ready in {host_components.prepared_root()}", flush=True)
for item in components:
    if item["status"] != "ready":
        what = " ".join(filter(None, (item.get("component"), item.get("version"), item.get("error"))))
        print(f"  {item['status']} {item['role']}: {what}; see python3 tests/fetch_components.py", flush=True)
(out / "components.json").write_text(json.dumps(components, indent=2) + "\n", encoding="utf-8")
# Just verified above: child tests accept these copies while their stamps are unchanged
# instead of re-inventorying every file (slow on /mnt/c).
os.environ[host_components.TRUSTED_VARIABLE] = json.dumps(trusted)

commands = []
tests = sorted((ROOT / "tests").glob("test_*.py"))
for test in tests:
    relative = test.relative_to(ROOT).as_posix()
    command = [sys.executable, str(test)]
    if relative in IDF_SOURCE_TESTS:
        command += ["--idf-path", str(args.p4_idf_path.resolve())]
    commands.append((relative, command))

report = []
for relative, command in commands:
    start = time.monotonic()
    process = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    notices = [line for line in process.stdout.splitlines() if line.startswith(("DEFERRED: ", "PARTIAL: "))]
    status = "FAIL" if process.returncode else notices[-1].split(":", 1)[0] if notices else "PASS"
    report.append(dict(test=relative, returncode=process.returncode, status=status,
                       seconds=round(time.monotonic()-start, 3), output=process.stdout))
    print(f"{status} {relative}", flush=True)
    if process.returncode:
        print(process.stdout, flush=True)
    else:
        for notice in notices:
            print(f"  {notice}", flush=True)
(out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
deferred = sum(r["status"] in ("DEFERRED", "PARTIAL") for r in report)
summary = f"{sum(r['status'] == 'PASS' for r in report)}/{len(report)} test programs passed"
print(summary + (f", {deferred} deferred or partial" if deferred else ""))
sys.exit(1 if any(r["returncode"] for r in report) else 0)
