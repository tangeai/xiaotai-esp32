"""Run inherited business boundary tests and new build-profile checks."""
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
report = []
tests = [*sorted((ROOT / "tests").glob("test_*.py")), *sorted((ROOT / "tests/regression").glob("test_*.py")),
         *sorted((ROOT / "tests/native").rglob("test_*.py"))]
for test in tests:
    start = time.monotonic()
    process = subprocess.run([sys.executable, str(test)], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report.append(dict(test=test.relative_to(ROOT).as_posix(), returncode=process.returncode,
                       seconds=round(time.monotonic()-start, 3), output=process.stdout))
    print(f"{'PASS' if process.returncode == 0 else 'FAIL'} {test.name}", flush=True)
    if process.returncode:
        print(process.stdout, flush=True)
out = ROOT / "build/host"
out.mkdir(parents=True, exist_ok=True)
(out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(f"{sum(r['returncode'] == 0 for r in report)}/{len(report)} test programs passed")
sys.exit(1 if any(r["returncode"] for r in report) else 0)
