"""Run the actual AT STATUS capability queries with IDF macros and API mocks."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    source = (tests.parent / "main/app_main.c").read_text(encoding="utf-8")
    begin = source.index('        printf("+HEAPCAP:')
    end = source.index('        printf("+STREAM:', begin)
    block = source[begin:end]
    assert 'printf("+HEAP: free=%u largest=%u minimum=%u integrity=%d main_hwm=%u uptime_ms=%llu\\n"' in source
    assert source.index('printf("+HEAP:', source.index("case XMINI_CMD_STATUS:")) < begin
    assert source.count("+HEAPCAP:") == 1
    allowed = {"printf", "heap_caps_get_free_size", "heap_caps_get_largest_free_block", "heap_caps_get_minimum_free_size"}
    assert set(re.findall(r"\b([a-zA-Z_][a-zA-Z_0-9]*)\(", block)) == allowed
    header = (args.idf / "components/heap/include/esp_heap_caps.h").read_text(encoding="utf-8")
    macros = []
    for name in ("MALLOC_CAP_INTERNAL", "MALLOC_CAP_8BIT", "MALLOC_CAP_DMA", "MALLOC_CAP_RTCRAM"):
        macro = re.search(r"^#define " + name + r"\s+\(1<<\d+\)", header, re.MULTILINE)
        assert macro, "Unexpected official capability definition: " + name
        macros.append(macro.group(0))
    fixture = (tests / "heapcap_status_fixture.c").read_text(encoding="utf-8")
    unit_text = fixture.replace("/* ACTUAL_HEAP_CAPABILITY_MACROS */", "\n".join(macros))
    unit_text = unit_text.replace("/* ACTUAL_HEAPCAP_STATUS */", block)
    with tempfile.TemporaryDirectory(prefix="xmini-heapcap-status-") as directory:
        unit, executable = Path(directory) / "actual_status.c", Path(directory) / "test"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.compiler, "-std=c11", "-g", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", str(unit), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        output = subprocess.check_output([str(executable)], text=True)
    lines = output.splitlines()
    assert len(lines) == 3
    for mode, line in enumerate(lines):
        assert line.startswith("+HEAPCAP: ")
        fields = dict(field.split("=", 1) for field in line.removeprefix("+HEAPCAP: ").split())
        expected = {"unit": "bytes"}
        for index, (name, caps) in enumerate((("internal", "00000804"), ("dma", "0000080c"), ("rtc", "00008000"))):
            expected[name + "_caps"] = caps
            for kind, amount in (("free", 100), ("largest", 70), ("minimum", 30)):
                expected[name + "_" + kind] = str((index + 1) * 1000 + amount if mode == 0 else 0 if mode == 1 else 0xffffffff)
        assert fields == expected, (fields, expected)
    print("Actual HEAPCAP queries/fields/IDF masks/zero/uint32 boundary PASS")


if __name__ == "__main__":
    main()
