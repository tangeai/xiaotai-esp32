# SPDX-License-Identifier: MIT
# Composes the actual WT99 display part and ESP scanout with host bus I/O.
set(ui "${XIAOTAI_ROOT}/sdk/xiaotai_ui")
get_target_property(board_includes board_wt99_parts INCLUDE_DIRECTORIES)
get_target_property(board_wrappers board_wt99_parts LINK_OPTIONS)
xiaotai_host_test(board_wt99_display_bind
    SOURCES board_test.cc "${CMAKE_CURRENT_LIST_DIR}/wt99_display_bind_test.c" "${ui}/esp-idf/ui_lvgl_idf.c"
    LINK xiaotai_board ${parts} xiaotai_ui
    INCLUDE ${board_includes} "${CMAKE_CURRENT_LIST_DIR}" "${ui}/src" "${ui}/test/idf_stubs"
    ROLES lvgl)
if(TARGET board_wt99_display_bind)
    target_compile_definitions(board_wt99_display_bind PRIVATE WT99_UI_BIND_PROBE=1 UI_TEST_IDF_602=1)
    target_link_options(board_wt99_display_bind PRIVATE ${board_wrappers}
        "LINKER:--wrap=lv_display_create" "LINKER:--wrap=lv_indev_create")
    foreach(case IN ITEMS borrow pointer-fail timeout detach-fail success release-fail)
        add_test(NAME board_wt99_display_bind_${case}
                 COMMAND board_wt99_display_bind ui-bind-${case})
        set_tests_properties(board_wt99_display_bind_${case} PROPERTIES LABELS integration TIMEOUT 120)
    endforeach()
endif()
