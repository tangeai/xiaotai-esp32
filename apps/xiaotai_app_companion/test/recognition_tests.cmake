# SPDX-License-Identifier: MIT
if(TARGET xiaotai_mod_recognition AND xiaotai_mod_recognition IN_LIST selected)
    xiaotai_companion_parts(recognition_parts "${app}/src" "${CMAKE_CURRENT_BINARY_DIR}/test_recognition_parts.c"
        "${optional}" "xiaotai_mod_recognition")
    xiaotai_host_test(app_companion_recognition SOURCES recognition_test.c fakes.c ${core} ${recognition_parts}
        ../src/companion_recognition_text.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps} xiaotai_json)
    target_link_options(app_companion_recognition PRIVATE "LINKER:--wrap=xiaotai_time_us")
    if(xiaotai_theme_lcd320 IN_LIST XIAOTAI_HOST_SELECTED)
        xiaotai_host_require(xiaotai_theme_lcd320)
    endif()
    if(xiaotai_theme_lcd320 IN_LIST XIAOTAI_HOST_SELECTED AND TARGET lvgl::lvgl)
        set(ui "${app}/../../sdk/xiaotai_ui")
        xiaotai_companion_parts(recognition_lcd_parts "${app}/src"
            "${CMAKE_CURRENT_BINARY_DIR}/test_recognition_lcd_parts.c"
            "${optional}" "xiaotai_ui;xiaotai_mod_recognition")
        xiaotai_host_test(app_companion_recognition_lcd
            SOURCES "${XIAOTAI_ROOT}/tests/sdk/integration/recognition_lcd_test.c" fakes.c
            ${core} ${recognition_lcd_parts} ../src/companion_recognition_text.c "${ui}/test/ui_fake_board.cc"
            INCLUDE . ../src "${ui}/src" "${ui}/test" INCLUDE_ONLY xiaotai_app_companion
            LINK ${test_deps} xiaotai_json xiaotai_theme_lcd320 lvgl::lvgl ROLES lvgl)
        if(TARGET app_companion_recognition_lcd)
            target_compile_definitions(app_companion_recognition_lcd PRIVATE COMPANION_TEST_REAL_UI=1)
        endif()
    endif()
endif()
