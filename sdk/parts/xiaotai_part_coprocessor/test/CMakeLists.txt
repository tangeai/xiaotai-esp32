# SPDX-License-Identifier: MIT
# Host tests of the coprocessor part against the scripted fake coprocessor
# (host/coprocessor_link.c). The test supplies its own attempt-marker store
# (xiaotai_kv_get_u8, _set_u8, _erase, _flush) and SHA-256 (xiaotai_sha256):
# both journal into the fake's call log, so the order of storage and link
# calls is asserted as one sequence, and the digest is a reference
# implementation checked against published vectors.
xiaotai_host_test(part_coprocessor SOURCES coprocessor_test.c sha256_reference.c
                  LINK xiaotai_part_coprocessor INCLUDE ../src TIMEOUT 120)
xiaotai_host_test(part_coprocessor_update SOURCES update_test.c sha256_reference.c
                  LINK xiaotai_part_coprocessor INCLUDE ../src)
target_link_options(part_coprocessor_update PRIVATE "LINKER:--wrap=xiaotai_delay_ms")

foreach(mode IN ITEMS peer no_peer)
    set(target part_coprocessor_idf_${mode})
    xiaotai_host_test(${target} SOURCES idf_link_test.c ../esp-idf/coprocessor_link.c
                      LINK xiaotai_port INCLUDE ../src hosted_stubs)
    if(mode STREQUAL "peer")
        target_compile_definitions(${target} PRIVATE CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_READY=1)
        target_link_options(${target} PRIVATE "LINKER:--wrap=xiaotai_signal_take,--wrap=free")
    endif()
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME part_coprocessor_overlay
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosted_overlay.py")
set_tests_properties(part_coprocessor_overlay PROPERTIES LABELS xiaotai_part_coprocessor TIMEOUT 120)
