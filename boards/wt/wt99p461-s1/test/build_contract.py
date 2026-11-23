# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_hosted_spi_diagnostics.py
"""Exercise the WT99 build graph and locked Hosted SPI worker without ESP-IDF."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile


BOARD = Path(__file__).resolve().parents[1]


def read_board_config():
    config = {}
    for line in (BOARD / "sdkconfig.board").read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_"):
            key, value = line.split("=", 1)
            config[key] = value.strip('"')
    # These are emitted by IDF Kconfig from CONFIG_ESP32P4_REV_MIN_301.
    assert config["CONFIG_ESP32P4_REV_MIN_301"] == "y"
    config.update(CONFIG_ESP32P4_REV_MIN_FULL="301", CONFIG_ESP32P4_REV_MAX_FULL="399",
                  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ="400")
    return config


def assert_check(config, expected=""):
    support = (BOARD / "build-support/check_board_config.cmake").as_posix()
    with tempfile.TemporaryDirectory() as path:
        script = Path(path) / "config.cmake"
        source = ["cmake_minimum_required(VERSION 3.20)",
                  "function(idf_build_get_property out property)",
                  f'    set(${{out}} "{Path(sys.executable).as_posix()}" PARENT_SCOPE)', "endfunction()"]
        source += [f'set({key} "{value}")' for key, value in config.items()]
        source += [f'include("{support}")']
        script.write_text("\n".join(source) + "\n", encoding="utf-8")
        result = subprocess.run(["cmake", "-P", script], text=True, capture_output=True, check=False)
        assert bool(result.returncode) == bool(expected) and expected in result.stderr, result.stderr


def check_partitions():
    with (BOARD / "partitions.csv").open(encoding="utf-8", newline="") as stream:
        rows = list(csv.reader(line for line in stream if not line.startswith("#")))
    actual = [(row[0].strip(), int(row[3], 0), int(row[4], 0)) for row in rows]
    assert actual == [("nvs", 0x9000, 0x4000), ("phy_init", 0xF000, 0x1000), ("factory", 0x10000, 0xFF0000)]


def check_spi_worker(source_path, base, compiler, sanitize, *, with_counters):
    source = source_path.read_text(encoding="utf-8")
    # Extract just the pinned worker; its closing brace is at column zero.
    start = source.index("\nstatic void eh_spi_rx_task(void *arg)\n") + 1
    worker = source[start:source.index("\n}\n", start) + 2]
    counters = ""
    if with_counters:
        start = source.index("/* Temporary counters:")
        counters = source[start:source.index("#endif", start)]
    fixture = BOARD.parents[2] / "tests/hosted_spi"
    variant = "diagnostics" if with_counters else "stock"
    unit = base / f"{variant}.c"
    unit.write_text(f'#include "host.h"\n{counters}{worker}\n#include "cases.h"\n', encoding="utf-8")
    modes = [("normal", [])]
    if sanitize:
        modes.append(("asan-ubsan", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]))
    for mode, flags in modes:
        binary = base / f"{variant}-{mode}"
        print(f"WT99 Hosted SPI: {variant} ({mode})", flush=True)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", *flags,
                        *(["-DDIAGNOSTICS"] if with_counters else []),
                        "-I", fixture, unit, "-o", binary], check=True)
        subprocess.run([binary], check=True)


def check_hosted_graph(hosted, compiler, sanitize):
    transport = hosted / "host/mcu/eh_host_mcu_transport"
    original = transport / "src/eh_host_bus_spi.c"
    before = original.read_bytes()
    adapter = BOARD / "build-support/cmake/hosted_spi_diagnostics.cmake"
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        (base / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(wt99_hosted_spi C)
set(CONFIG_ESP_HOSTED_HOST_TRANSPORT_MCU_READY ON)
set(CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SPI_READY ON)
add_subdirectory("{transport.as_posix()}" transport)
function(idf_component_get_property out component property)
    set(${{out}} "{hosted.as_posix()}" PARENT_SCOPE)
endfunction()
include("{adapter.as_posix()}")
cmake_language(DEFER CALL xiaotai_enable_hosted_spi_diagnostics)
function(assert_spi_selection)
    get_target_property(selected eh_host_mcu_transport SOURCES)
    if(NOT "${{CMAKE_BINARY_DIR}}/xiaotai_hosted/eh_host_bus_spi.c" IN_LIST selected OR
       "src/eh_host_bus_spi.c" IN_LIST selected)
        message(FATAL_ERROR "WT99 diagnostics source not selected")
    endif()
endfunction()
cmake_language(DEFER CALL assert_spi_selection)
''', encoding="utf-8")
        subprocess.run(["cmake", "-S", base, "-B", base / "out"], check=True, capture_output=True)
        assert original.read_bytes() == before
        check_spi_worker(original, base, compiler, sanitize, with_counters=False)
        generated = base / "out/xiaotai_hosted/eh_host_bus_spi.c"
        check_spi_worker(generated, base, compiler, sanitize, with_counters=True)


if __name__ == "__main__":
    if len(sys.argv) in (2, 4):
        compiler, sanitize = sys.argv[2:] if len(sys.argv) == 4 else ("cc", "ON")
        check_hosted_graph(Path(sys.argv[1]), compiler, sanitize == "ON")
        sys.exit(0)
    base = read_board_config()
    assert_check(base)
    for flag, value, diagnostic in (
        ("CONFIG_ESP32P4_REV_MIN_FULL", "100", "rev3.1-3.99"),
        ("CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO", "y", "C61 full-duplex SPI"),
        ("CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN", "y", "owns Hosted startup"),
        ("CONFIG_ESP_HOSTED_HOST_SPI_MOSI_GPIO", "10", "SPI MOSI must use GPIO14"),
        ("CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ", "20", "SPI mode 3 at 10 MHz"),
        ("CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH", "elsewhere.json", "reviewed ISP"),
    ):
        assert_check({**base, flag: value}, diagnostic)
    check_partitions()
