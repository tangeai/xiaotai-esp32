# SPDX-License-Identifier: MIT
set(ui "${CMAKE_CURRENT_LIST_DIR}/../../../xiaotai_ui")
xiaotai_host_test(theme_mono_canvas SOURCES canvas_test.c LINK xiaotai_theme_mono128 INCLUDE ../src)
foreach(case IN ITEMS match list)
    xiaotai_host_test(theme_mono_${case} SOURCES theme_test.c "${ui}/test/ui_fake_board.cc"
                      LINK xiaotai_theme_mono128 INCLUDE "${ui}/test" "${ui}/src")
    target_compile_definitions(theme_mono_${case} PRIVATE MONO_TEST_CASE="${case}")
endforeach()
xiaotai_host_test(theme_mono_pages SOURCES pages_test.c "${ui}/test/ui_fake_board.cc"
                  LINK xiaotai_theme_mono128 INCLUDE ../src "${ui}/test" "${ui}/src")
xiaotai_host_test(theme_mono_owner_stack SOURCES owner_stack_test.c "${ui}/test/ui_fake_board.cc"
                  LINK xiaotai_theme_mono128 INCLUDE "../src" "${ui}/src" "${ui}/test" STACK)
if(TARGET theme_mono_owner_stack)
    target_link_options(theme_mono_owner_stack PRIVATE
        "LINKER:--wrap=xiaotai_time_us,--wrap=xiaotai_timer_start,--wrap=xiaotai_timer_cancel,--wrap=xiaotai_task_create")
endif()
