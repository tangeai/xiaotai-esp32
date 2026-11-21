# SPDX-License-Identifier: MIT
# Host tests of the colour theme kit's component (LCD-K).
#
# theme_lcd_kit_unit: the lifecycle unit's init succeeds and allocates nothing
# (host memory counters).
#
# theme_lcd_kit_selection: scripts/sdk_selection.py over the real catalog for
# the six products (kit manifest, the theme units each product selects and
# their order, the LVGL option, the display check's refusals, named themes,
# CMake requirements against the manifests). It reads the manifests of the
# whole tree at test time, so a later change to any theme or to the selection
# script is checked on the next run without reconfiguring. The drawing tests
# (theme_lcd_home, theme_lcd_scale, ...) join with LCD-1 to LCD-4.
xiaotai_host_test(theme_lcd_kit_unit SOURCES kit_unit_test.c LINK xiaotai_theme_lcd_kit)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_filename_component(tree "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_test(NAME theme_lcd_kit_selection
         COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/kit_selection_test.py" "${tree}")
# Same label as xiaotai_host_test gives, so `ctest -L xiaotai_theme_lcd_kit` selects both.
set_tests_properties(theme_lcd_kit_selection PROPERTIES TIMEOUT 120 LABELS "xiaotai_theme_lcd_kit")
add_test(NAME theme_lcd_font_source
         COMMAND Python3::Interpreter -B "${tree}/scripts/generate_lcd_fonts.py" --check)
set_tests_properties(theme_lcd_font_source PROPERTIES TIMEOUT 30 LABELS "xiaotai_theme_lcd_kit")
xiaotai_host_test(theme_lcd_fonts SOURCES lcd_font_test.c
                  LINK xiaotai_theme_lcd_kit lvgl::lvgl ROLES lvgl)

set(ui_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../xiaotai_ui/test")
set(integration "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/sdk/integration")
xiaotai_host_test(theme_lcd_scale SOURCES "${integration}/lcd_home_test.c" "${ui_fixture}/ui_fake_board.cc"
                  LINK xiaotai_theme_lcd_kit lvgl::lvgl INCLUDE "${ui_fixture}" ROLES lvgl)
xiaotai_host_test(theme_lcd_face SOURCES face_test.c LINK xiaotai_theme_lcd_kit
                  INCLUDE ../src ROLES lvgl)
foreach(case form choice_confirm choice_matrix keypad overlay form_diag ptt_edges)
    xiaotai_host_test(theme_lcd_${case} SOURCES lcd_form_test.c "${ui_fixture}/ui_fake_board.cc"
                      LINK xiaotai_theme_lcd_kit lvgl::lvgl
                      INCLUDE "${ui_fixture}" "${ui_fixture}/../src" ROLES lvgl)
    if(TARGET theme_lcd_${case})
        target_compile_definitions(theme_lcd_${case} PRIVATE LCD_FORM_CASE="${case}")
    endif()
endforeach()
foreach(case IN ITEMS list code call notice)
    xiaotai_host_test(theme_lcd_${case} SOURCES "${integration}/lcd_pages_test.c" "${ui_fixture}/ui_fake_board.cc"
                      LINK xiaotai_theme_lcd_kit lvgl::lvgl INCLUDE "${ui_fixture}" ROLES lvgl)
    if(TARGET theme_lcd_${case})
        target_compile_definitions(theme_lcd_${case} PRIVATE LCD_PAGES_CASE="${case}")
    endif()
endforeach()
