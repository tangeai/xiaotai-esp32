# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_usb_connection_monitor.py
"""Check the V3 USB monitor's build ownership against a reviewed local IDF."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "sdk/xiaotai_port"
MONITOR = PORT / "esp-idf/usb_monitor/usb_connection_monitor.c"
ADAPTER = MONITOR.with_name("usb_monitor.cmake")


def configure_fixture(temp, idf_version, original, mode, active_sources):
    project = f"""
cmake_minimum_required(VERSION 3.16)
project(test_usb_monitor NONE)
add_library(idf_usb OBJECT {active_sources})
set_target_properties(idf_usb PROPERTIES LINKER_LANGUAGE C)
function(idf_component_get_property out component property)
    if(NOT "${{component}}" STREQUAL "esp_driver_usb_serial_jtag")
        message(FATAL_ERROR "wrong component")
    endif()
    if("${{property}}" STREQUAL "COMPONENT_DIR")
        set(${{out}} "{original.parent.parent.as_posix()}" PARENT_SCOPE)
    else()
        set(${{out}} idf_usb PARENT_SCOPE)
    endif()
endfunction()
include("{idf_version.as_posix()}")
include("{ADAPTER.as_posix()}")
set(CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX {"OFF" if mode == "fix_off" else "ON"})
set(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG {"OFF" if mode == "no_usb" else "ON"})
xiaotai_port_configure_usb_monitor()
get_target_property(sources idf_usb SOURCES)
file(WRITE "${{CMAKE_BINARY_DIR}}/sources.txt" "${{sources}}")
"""
    (temp / "CMakeLists.txt").write_text(project, encoding="utf-8")
    out = temp / ("out_" + mode)
    result = subprocess.run(["cmake", "-S", str(temp), "-B", str(out)],
                            text=True, capture_output=True, check=False)
    return result, out / "sources.txt"


def build_ownership(idf, temp):
    native = idf / "components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_connection_monitor.c"
    version = idf / "tools/cmake/version.cmake"
    before = native.read_bytes()
    assert "SPDX-License-Identifier: Apache-2.0" in MONITOR.read_text(encoding="utf-8")
    assert "Espressif Systems" in MONITOR.read_text(encoding="utf-8")
    assert "ESP_SYSTEM_INIT_FN(usb_serial_jtag_conn_status_init, SECONDARY, BIT(0), 230)" in MONITOR.read_text()
    temp_source = temp / "local_usb/src/usb_serial_jtag_connection_monitor.c"
    temp_source.parent.mkdir(parents=True)
    temp_source.write_bytes(before.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
    companion = temp / "local_usb/src/usb_serial_jtag_vfs.c"
    companion.touch()
    active = f'"{temp_source.as_posix()}" "{companion.as_posix()}"'
    result, selected = configure_fixture(temp, version, temp_source, "active", active)
    assert result.returncode == 0, result.stderr
    assert selected.read_text().split(";") == [MONITOR.as_posix(), companion.as_posix()]

    result, _ = configure_fixture(temp, version, temp_source, "not_registered", f'"{companion.as_posix()}"')
    assert result.returncode and "source registration changed" in result.stderr, result.stderr
    temp_source.write_bytes(before + b"\n/* source identity changed */\n")
    result, _ = configure_fixture(temp, version, temp_source, "drift", active)
    assert result.returncode and "not the reviewed source" in result.stderr, result.stderr
    for mode in ("fix_off", "no_usb"):
        result, selected = configure_fixture(temp, version, temp_source, mode, active)
        assert result.returncode == 0, result.stderr
        assert selected.read_text().split(";") == [temp_source.as_posix(), companion.as_posix()]
    assert native.read_bytes() == before


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if args.idf_path is None:
        parser.error("provide --idf-path or IDF_PATH")
    with tempfile.TemporaryDirectory(prefix="xiaotai-port-usb-") as tmp:
        build_ownership(args.idf_path.resolve(), Path(tmp))
    print("PASS: USB monitor license, pinned identity, registration and both opt-out conditions")


if __name__ == "__main__":
    main()
