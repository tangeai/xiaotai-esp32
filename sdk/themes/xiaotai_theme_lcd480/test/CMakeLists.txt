# SPDX-License-Identifier: MIT
set(ui_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../xiaotai_ui/test")
set(integration "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/sdk/integration")
foreach(case IN ITEMS match scale video background clock desk talk)
    set(name theme_lcd480_${case})
    xiaotai_host_test(${name} SOURCES "${integration}/lcd480_test.c" "${ui_fixture}/ui_fake_board.cc"
                      LINK xiaotai_theme_lcd480 lvgl::lvgl INCLUDE "${ui_fixture}" "${ui_fixture}/../src" ROLES lvgl)
    if(TARGET ${name})
        target_compile_definitions(${name} PRIVATE LCD480_TEST_CASE="${case}")
        target_link_options(${name} PRIVATE "LINKER:--wrap=xiaotai_mem_alloc")
    endif()
endforeach()
