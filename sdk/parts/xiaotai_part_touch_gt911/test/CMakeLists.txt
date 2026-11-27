# SPDX-License-Identifier: MIT
xiaotai_host_test(part_touch_gt911 SOURCES touch_test.c LINK xiaotai_part_touch_gt911
    INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

xiaotai_host_require(xiaotai_part_panel_jd9165)
xiaotai_host_test(part_jd9165_gt911_host SOURCES display_host_test.c
    LINK xiaotai_part_touch_gt911 xiaotai_part_panel_jd9165)

if(DEFINED ENV{XIAOTAI_GT911_NATIVE_TEST_PACKAGE} AND DEFINED ENV{XIAOTAI_GT911_NATIVE_TEST_IDF})
    set(idfs "$ENV{XIAOTAI_GT911_NATIVE_TEST_IDF}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME part_gt911_native_poll
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_poll_test.py"
                     --package "$ENV{XIAOTAI_GT911_NATIVE_TEST_PACKAGE}" --idf ${idfs})
    set_tests_properties(part_gt911_native_poll PROPERTIES LABELS xiaotai_part_touch_gt911 TIMEOUT 120)
endif()
