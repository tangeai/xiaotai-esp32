#!/usr/bin/env python3
"""Exercise the shared query implementation with P4's actual SDK contract."""
from pathlib import Path
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
shared = project.parent / 'lckfb-szpi-esp32s3-tirtc'
subprocess.run([
    sys.executable, str(shared / 'tools/test_contact_query.py'),
    '--sdk-header', str(project / 'components/tirtc_sdk/include/tiRTC.h'),
    '--cjson-dir', str(project / 'managed_components/espressif__cjson/cJSON'),
], check=True)
