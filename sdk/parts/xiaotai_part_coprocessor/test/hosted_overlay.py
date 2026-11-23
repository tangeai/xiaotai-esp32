#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_hosted_overlay.py
"""Check component selection without writing the registry-managed Hosted tree."""

import argparse
from difflib import unified_diff
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

PART = Path(__file__).resolve().parents[1]
SYS = "host/features/eh_host_feat_system"
RPC = "host/features/eh_host_feat_rpc"
V2 = "host/features/eh_host_feat_rpc_ext_v2"
BUS = "host/mcu/eh_host_mcu_transport"
GROUPS = {
    SYS: ("eh_host_sys", "eh_host_feat_system_v2_req", "eh_host_feat_system_v2_resp_evt"),
    RPC: ("eh_host_feat_rpc_rx",),
    BUS: ("eh_host_bus_sdio", "eh_host_bus_spi"),
}
SOURCES = {g: tuple(f"{g}/src/{name}.c" for name in names) for g, names in GROUPS.items()}
HEADERS = {
    f"{SYS}/include/eh_host_sys.h": "#define SYS_MARK 0\n",
    f"{SYS}/include/eh_host_feat_system.h": '#include "eh_host_sys.h"\n',
    f"{V2}/include/eh_host_feat_rpc_ext_v2_types.h": "#define RPC_MARK 0\n",
    f"{V2}/include/eh_host_feat_rpc_ext_v2.h": '#include "eh_host_feat_rpc_ext_v2_types.h"\n',
}


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def command(*args, fails_with=None):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    if fails_with is None:
        assert not result.returncode, result.stdout
    else:
        assert result.returncode and fails_with in result.stdout, result.stdout


def patch(sections):
    return "".join(line for path, text in sections.items() for line in unified_diff(
        text.splitlines(keepends=True), text.replace("0", "1").splitlines(keepends=True),
        fromfile=f"a/{path}", tofile=f"b/{path}"))


def fake_tree(root):
    hosted, part = root / "managed", root / "component"
    hook = part / "esp-idf/hosted_overlay.cmake"
    hook.parent.mkdir(parents=True)
    shutil.copyfile(PART / "CMakeLists.txt", part / "CMakeLists.txt")
    shutil.copyfile(PART / "esp-idf/hosted_overlay.cmake", hook)
    content = HEADERS | {p: f"int {Path(p).stem}(void) {{ return 0; }}\n"
                         for paths in SOURCES.values() for p in paths}
    for path, text in content.items():
        write(hosted / path, text)
    write(hosted / "idf_component.yml", "version: 3.0.7\n")
    write(hook.parent / "patches/esp-hosted-3.0.7-app-desc.patch",
          patch({p: t for p, t in content.items() if not p.startswith(BUS)}))
    bus = SOURCES[BUS][0]
    write(hook.parent / "patches/esp-hosted-3.0.7-p4-sdio-rx-bounce.patch", patch({bus: content[bus]}))
    return part, hosted


def harness(root, part, hosted, actual):
    write(root / "wrapper.c", "void overlay_wrapper(void) {}\n")
    write(root / "verify.c", """#include "eh_host_feat_system.h"
#include "eh_host_feat_rpc_ext_v2.h"
int eh_host_sys(void);
int eh_host_feat_system_v2_req(void);
int eh_host_feat_system_v2_resp_evt(void);
int eh_host_feat_rpc_rx(void);
int eh_host_bus_sdio(void);
int eh_host_bus_spi(void);
int main(void) {
    int sum = eh_host_sys() + eh_host_feat_system_v2_req()
        + eh_host_feat_system_v2_resp_evt() + eh_host_feat_rpc_rx();
    return sum != 4 * SELECTED || SYS_MARK != SELECTED || RPC_MARK != SELECTED
        || eh_host_bus_sdio() != USE_SDIO || eh_host_bus_spi() != 0;
}
""")
    targets = "".join(f'add_library({g.rsplit("/", 1)[1]} STATIC ' +
                      " ".join(f'"{hosted.as_posix()}/{p}"' for p in paths) + ")\n"
                      for g, paths in SOURCES.items())
    write(root / "CMakeLists.txt", f"""cmake_minimum_required(VERSION 3.22)
project(hosted_wiring C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
{targets}
add_library(eh_host_feat_rpc_ext_v2 STATIC wrapper.c)
target_include_directories(eh_host_feat_system PUBLIC "{hosted.as_posix()}/{SYS}/include")
target_include_directories(eh_host_feat_rpc_ext_v2 PUBLIC "{hosted.as_posix()}/{V2}/include")
add_library(test_hosted INTERFACE)
target_include_directories(test_hosted INTERFACE "{hosted.as_posix()}/{SYS}/include" "{hosted.as_posix()}/{V2}/include")
target_link_libraries(test_hosted INTERFACE eh_host_feat_system eh_host_feat_rpc eh_host_feat_rpc_ext_v2 eh_host_mcu_transport)
function(idf_component_get_property out name property)
    if(NOT name STREQUAL "espressif__esp_hosted")
        message(FATAL_ERROR "Unexpected component: ${{name}}")
    endif()
    if(property STREQUAL "COMPONENT_DIR")
        set(${{out}} "{hosted.as_posix()}" PARENT_SCOPE)
    else()
        set(${{out}} test_hosted PARENT_SCOPE)
    endif()
endfunction()
function(idf_component_register)
    add_library(test_part INTERFACE)
endfunction()
if(PART_SELECTED)
    set(ESP_PLATFORM TRUE)
    add_subdirectory("{part.as_posix()}" part)
endif()
if(NOT {int(actual)})
    add_executable(verify verify.c)
    target_compile_definitions(verify PRIVATE SELECTED=$<BOOL:${{PART_SELECTED}}>
        USE_SDIO=$<AND:$<BOOL:${{PART_SELECTED}}>,$<BOOL:${{CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO}}>>)
    target_link_libraries(verify PRIVATE test_hosted)
    enable_testing()
    add_test(NAME verify COMMAND verify)
endif()
""")


def exercise(root, part, hosted, actual=False):
    harness(root, part, hosted, actual)
    paths = [hosted / "idf_component.yml", *(hosted / p for p in HEADERS),
             *(hosted / p for paths in SOURCES.values() for p in paths)]
    before = {p: p.read_bytes() for p in paths}
    for name, selected, sdio in (("none", 0, 1), ("spi", 1, 0), ("sdio", 1, 1)):
        build = root / name
        configure = ("cmake", *(["-G", "Ninja"] if shutil.which("ninja") else []),
                     "-S", str(root), "-B", str(build),
                     f"-DPART_SELECTED={selected}", f"-DCONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO={sdio}")
        for _ in range(1 if actual else 2):
            command(*configure)
            if not actual:
                command("cmake", "--build", str(build))
                command("ctest", "--test-dir", str(build), "--output-on-failure")
            assert all(p.read_bytes() == text for p, text in before.items())
        compiled = [Path(entry["file"]).resolve() for entry in json.loads(
            (build / "compile_commands.json").read_text(encoding="utf-8"))]
        for group, paths in SOURCES.items():
            for path in paths:
                replace = selected and (group != BUS or sdio and path == SOURCES[BUS][0])
                overlay = (build / "xiaotai-coprocessor/overlay" / path).resolve()
                assert compiled.count(overlay) == int(bool(replace)), (name, path)
                assert compiled.count((hosted / path).resolve()) == int(not replace), (name, path)
    if not actual:
        target = hosted / SOURCES[SYS][0]
        write(target, "int eh_host_sys(void) { return 17; }\n")
        command(*configure, fails_with="patch does not match locked source")
        assert target.read_bytes() == b"int eh_host_sys(void) { return 17; }\n"
        target.write_bytes(before[target])
        write(hosted / "idf_component.yml", "version: 3.0.8\n")
        command(*configure, fails_with="Hosted 3.0.7 is required")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--hosted", type=Path, help="also configure against the local locked Hosted 3.0.7 cache")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xiaotai-hosted-overlay-") as temp:
        root = Path(temp)
        part, hosted = fake_tree(root)
        exercise(root, part, hosted)
        if args.hosted:
            exercise(root / "locked", PART, args.hosted.resolve(), actual=True)
    print("Hosted overlay: selected SPI/SDIO, unselected, pristine sources, reconfigure and failure gates passed")
