# SPDX-License-Identifier: MIT
import json
import subprocess
import sys
from pathlib import Path


manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
actual = json.loads(subprocess.check_output([sys.argv[2]], text=True))
if actual != manifest["parts"]:
    sys.exit(f"JC1060F570C parts differ from board.json:\n{actual!r}\n{manifest['parts']!r}")
