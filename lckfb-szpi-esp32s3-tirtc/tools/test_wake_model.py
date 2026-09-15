#!/usr/bin/env python3
"""Validate the bundled wake model/head with the same FlatBuffer schema as IDF."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
tflm = root / "managed_components/espressif__esp-tflite-micro"
voice = root / "components/starter_voice"
if not (tflm / "tensorflow/lite/schema/schema_generated.h").is_file():
    raise SystemExit("Local TFLM sources are required; run idf.py reconfigure first.")
with tempfile.TemporaryDirectory(prefix="xiaotai-wake-model-") as directory:
    binary = Path(directory) / "test_wake_model"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(tflm),
        "-I", str(tflm / "third_party/flatbuffers/include"),
        "-I", str(voice / "model"), "-I", str(voice / "vendor"),
        str(root / "tools/test_voicute_model.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(voice / "model/nihaoxiaotai.tflite")],
                   check=True, timeout=30)
