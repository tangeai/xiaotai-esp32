# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import runpy


here = Path(__file__).resolve()
board = here.parents[1]
generator = board.parents[2] / "scripts/generate_sc2336_isp.py"
tools = runpy.run_path(str(generator))
expected = tools["derive"](json.loads(tools["SOURCE"].read_text(encoding="utf-8")))
if json.loads((board / "assets/sc2336_isp.json").read_text(encoding="utf-8")) != expected:
    raise SystemExit("WT99P461-S1 tuning differs from the reviewed SC2336 gamma derivation")
