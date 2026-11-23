#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the original and generated native LCD SPI code using a fake SPI master."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "esp-idf"
GENERATOR = NATIVE / "native_spi_overlay.py"

CMAKE_FIXTURE = r'''
cmake_minimum_required(VERSION 3.17)
project(panel_graph C)
set(ESP_PLATFORM TRUE)
add_library(pristine STATIC "${PINNED_LCD}/spi/esp_lcd_panel_io_spi.c")

function(idf_component_register)
    cmake_parse_arguments(reg "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
    add_library(part STATIC ${reg_SRCS})
    set(COMPONENT_LIB part PARENT_SCOPE)
endfunction()
function(idf_component_get_property out component property)
    if(NOT component STREQUAL "esp_lcd" OR NOT property STREQUAL "COMPONENT_DIR")
        message(FATAL_ERROR "Unexpected IDF component lookup")
    endif()
    set(${out} "${PINNED_LCD}" PARENT_SCOPE)
endfunction()
function(idf_build_get_property out property)
    if(NOT property STREQUAL "PYTHON")
        message(FATAL_ERROR "Unexpected IDF build lookup")
    endif()
    set(${out} "${PYTHON_PATH}" PARENT_SCOPE)
endfunction()

add_subdirectory("${PANEL_PART}" part)
get_target_property(pristine_sources pristine SOURCES)
if(NOT pristine_sources STREQUAL "${PINNED_LCD}/spi/esp_lcd_panel_io_spi.c")
    message(FATAL_ERROR "The pristine IDF target was modified")
endif()
get_target_property(sources part SOURCES)
get_target_property(definitions part COMPILE_DEFINITIONS)
if(ENABLE_EXTERNAL_DMA)
    if(NOT definitions MATCHES "XIAOTAI_SPI_EXTERNAL_DMA=1")
        message(FATAL_ERROR "External DMA capability depends on the patch selection")
    endif()
elseif(definitions MATCHES "XIAOTAI_SPI_EXTERNAL_DMA")
    message(FATAL_ERROR "External DMA capability escaped the SoC and PSRAM gate")
endif()
if(ENABLE_OVERLAY)
    if(NOT sources MATCHES "xiaotai_lcd_spi.c" OR NOT definitions MATCHES "XIAOTAI_SPI_NATIVE_CORRECTION=1")
        message(FATAL_ERROR "Private derived SPI LCD factory missing")
    endif()
elseif(sources MATCHES "xiaotai_lcd_spi.c" OR definitions MATCHES "XIAOTAI_SPI_NATIVE_CORRECTION")
    message(FATAL_ERROR "Native derivation leaked outside the capability/runtime gate")
elseif(EXISTS "${CMAKE_BINARY_DIR}/part/xiaotai_spi_native")
    message(FATAL_ERROR "The default graph still ran the overlay generator")
endif()
'''


def verify_generator_guards(idf, scratch):
    lcd = idf / "components/esp_lcd"
    fake_lcd = scratch / "modified" / "components/esp_lcd"
    fake_lcd.mkdir(parents=True)
    for name in ("spi/esp_lcd_panel_io_spi.c", "priv_include/esp_lcd_common.h"):
        output = fake_lcd / name
        output.parent.mkdir()
        output.write_bytes((lcd / name).read_bytes())
    original = fake_lcd / "spi/esp_lcd_panel_io_spi.c"
    original.write_bytes(original.read_bytes() + b"\n/* tamper */\n")
    result = subprocess.run(
        [sys.executable, "-B", GENERATOR, fake_lcd, scratch / "rejected"],
        capture_output=True, text=True)
    assert result.returncode != 0 and "Unreviewed pinned ESP-IDF" in result.stderr
    assert not (scratch / "rejected").exists()


def verify_graph(idf, scratch):
    project = scratch / "cmake-src"
    project.mkdir()
    (project / "CMakeLists.txt").write_text(CMAKE_FIXTURE, encoding="ascii")
    for label, supported, psram, early, opt_in in (("default", True, True, False, None),
                                                  ("off", True, True, False, False),
                                                  ("direct", True, True, False, True),
                                                  ("early", True, True, True, True),
                                                  ("no-dma", False, True, False, True),
                                                  ("no-psram", True, False, False, True)):
        dma = supported and psram and not early
        selection = [] if opt_in is None else [f"-DCONFIG_XIAOTAI_PART_PANEL_SPI_DMA_OWNERSHIP_FIX={opt_in}"]
        built = subprocess.run(["cmake", "-S", project, "-B", scratch / label,
                                f"-DPINNED_LCD={idf / 'components/esp_lcd'}", f"-DPANEL_PART={NATIVE.parent}",
                                f"-DPYTHON_PATH={sys.executable}", f"-DENABLE_OVERLAY={dma and bool(opt_in)}",
                                f"-DENABLE_EXTERNAL_DMA={dma}", *selection,
                                f"-DCONFIG_SOC_PSRAM_DMA_CAPABLE={supported}", f"-DCONFIG_SPIRAM={psram}",
                                f"-DCMAKE_BUILD_EARLY_EXPANSION={early}"], capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr


def run(idf, scratch):
    source = idf / "components/esp_lcd/spi/esp_lcd_panel_io_spi.c"
    version = idf / "tools/cmake/version.cmake"
    major, minor, patch = re.findall(r"set\(IDF_VERSION_\w+ (\d+)\)", version.read_text())
    print(f"ESP-IDF {major}.{minor}.{patch}", flush=True)
    pristine = source.read_bytes()
    generated = Path(subprocess.check_output(
        [sys.executable, "-B", GENERATOR, idf / "components/esp_lcd", scratch / "generated"], text=True).strip())
    saved = generated.read_bytes()
    timestamp = generated.stat().st_mtime_ns
    subprocess.run([sys.executable, "-B", GENERATOR, idf / "components/esp_lcd", scratch / "generated"],
                   check=True, stdout=subprocess.DEVNULL)
    assert generated.read_bytes() == saved and generated.stat().st_mtime_ns == timestamp
    assert (generated.parent / "LICENSE-APACHE-2.0").read_bytes() == (idf / "LICENSE").read_bytes().replace(b"\r\n", b"\n")
    verify_generator_guards(idf, scratch)
    verify_graph(idf, scratch)
    print("  generator: reconfigure, tamper rejection and component-only source selection passed", flush=True)

    stubs = scratch / "stubs"
    for include in set(re.findall(r'^#include "([^"]+)"', saved.decode(), re.M)) | {"esp_lcd_io_spi.h"}:
        if (NATIVE / include).is_file():
            continue
        header = stubs / include
        header.parent.mkdir(parents=True, exist_ok=True)
        header.touch()
    for production, template in [(False, source), (True, generated)]:
        binary = scratch / ("corrected" if production else "pristine")
        defines = [] if production else ["-DPRISTINE_DRIVER=1"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                        "-I", stubs, "-I", NATIVE, "-include", HERE / "native_spi_shim.h",
                        *defines, HERE / "native_spi_overlay_test.c", template, "-o", binary], check=True)
        subprocess.run([binary], check=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                            "UBSAN_OPTIONS": "halt_on_error=1"})
        print("  corrected: all completion, no-pop and stop cases passed" if production else
              "  pristine: reproduced empty wait with stale inflight count", flush=True)
    assert source.read_bytes() == pristine


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    for idf in args.idf:
        with tempfile.TemporaryDirectory(prefix="xt-spi-native-") as scratch:
            run(idf.resolve(), Path(scratch))


if __name__ == "__main__":
    main()
