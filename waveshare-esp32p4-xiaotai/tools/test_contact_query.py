#!/usr/bin/env python3
"""Exercise the local query implementation with P4's actual SDK contract."""
from pathlib import Path
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
subprocess.run([
    sys.executable, str(project / 'tools/contact_query_cases.py'),
    '--sdk-header', str(project / 'components/tirtc_sdk/include/tiRTC.h'),
    '--cjson-dir', str(project / 'managed_components/espressif__cjson/cJSON'),
], check=True)
