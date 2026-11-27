# SPDX-License-Identifier: MIT
set(ui_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../xiaotai_ui/test")
set(integration "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/sdk/integration")
xiaotai_host_test(theme_lcd_home SOURCES "${integration}/lcd_home_test.c" "${ui_fixture}/ui_fake_board.cc"
                  LINK xiaotai_theme_lcd320 lvgl::lvgl INCLUDE "${ui_fixture}" ROLES lvgl)
if(TARGET theme_lcd_home)
    target_compile_definitions(theme_lcd_home PRIVATE LCD_TEST_REAL_THEME=1)
endif()
xiaotai_host_test(theme_lcd320_match SOURCES lcd_match_test.c "${ui_fixture}/ui_fake_board.cc" LINK xiaotai_theme_lcd320
                  INCLUDE "${ui_fixture}" ROLES lvgl)
