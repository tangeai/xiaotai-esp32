# SPDX-License-Identifier: MIT
xiaotai_host_test(part_panel_jd9165 SOURCES panel_test.c LINK xiaotai_part_panel_jd9165
    INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
if(XIAOTAI_HOST_SANITIZE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME part_panel_jd9165_vendor_delete COMMAND Python3::Interpreter -B
             "${CMAKE_CURRENT_SOURCE_DIR}/vendor_delete_test.py" "${CMAKE_C_COMPILER}")
    set_tests_properties(part_panel_jd9165_vendor_delete PROPERTIES TIMEOUT 20 LABELS xiaotai_part_panel_jd9165)
endif()
