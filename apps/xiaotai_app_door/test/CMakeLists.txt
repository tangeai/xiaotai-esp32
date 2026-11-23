# SPDX-License-Identifier: MIT
set(door_sources fakes.c ../src/door.c ../src/door_home.c ../src/door_call.c ../src/door_open.c
    ../src/door_fallback.c ../src/door_at.c ../src/door_station.c
    ../src/with_stream.c ../src/with_stream_video.c ../src/with_video_call.c)
set(door_tests door_test.c ${door_sources})
set(test_components xiaotai_app_door xiaotai_mod_stream xiaotai_mod_video_call xiaotai_mod_stream_video)
set(features XIAOTAI_DOOR_WITH_STREAM XIAOTAI_DOOR_WITH_STREAM_VIDEO XIAOTAI_DOOR_WITH_VIDEO_CALL)
xiaotai_host_test(app_door_home SOURCES ${door_tests} LINK xiaotai_port xiaotai_framework xiaotai_at
    INCLUDE_ONLY ${test_components} INCLUDE ../src)
target_compile_definitions(app_door_home PRIVATE ${features})
target_link_options(app_door_home PRIVATE "LINKER:--wrap=xiaotai_time_us")
foreach(case presenter call open stream stream_stop_policy stream_stop_full_timers fallback fallback_announce station remote_open directory)
    add_test(NAME app_door_${case} COMMAND app_door_home ${case})
    set_tests_properties(app_door_${case} PROPERTIES LABELS xiaotai_app_door TIMEOUT 120)
endforeach()
xiaotai_host_test(app_door_open_lock SOURCES ${door_tests} ../src/with_lock.c LINK xiaotai_port xiaotai_framework xiaotai_at
    INCLUDE_ONLY ${test_components} xiaotai_mod_lock INCLUDE ../src)
target_compile_definitions(app_door_open_lock PRIVATE ${features} XIAOTAI_DOOR_WITH_LOCK DOOR_TEST_OPEN_LOCK)
target_link_options(app_door_open_lock PRIVATE "LINKER:--wrap=xiaotai_time_us")

set(at_fixture "${XIAOTAI_ROOT}/sdk/xiaotai_at/test")
xiaotai_host_test(app_door_at SOURCES door_at_test.c ${door_sources} "${at_fixture}/at_test_help.c"
    LINK xiaotai_at INCLUDE_ONLY ${test_components} INCLUDE ../src "${at_fixture}")
target_compile_definitions(app_door_at PRIVATE ${features})
xiaotai_host_test(app_door_fallback_wechat SOURCES ${door_tests} LINK xiaotai_at
    INCLUDE_ONLY ${test_components} xiaotai_mod_wechat INCLUDE ../src)
target_compile_definitions(app_door_fallback_wechat PRIVATE ${features} XIAOTAI_DOOR_WITH_WECHAT)
target_link_options(app_door_fallback_wechat PRIVATE "LINKER:--wrap=xiaotai_time_us")
foreach(case station_wechat directory)
    add_test(NAME app_door_wechat_${case} COMMAND app_door_fallback_wechat ${case})
    set_tests_properties(app_door_wechat_${case} PROPERTIES LABELS xiaotai_app_door TIMEOUT 120)
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME app_door_at_help COMMAND Python3::Interpreter -B "${at_fixture}/at_test_help.py"
    --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../application.json"
    -- $<TARGET_FILE:app_door_at> --help-records)
set_tests_properties(app_door_at_help PROPERTIES LABELS xiaotai_app_door TIMEOUT 120)

if(door_wide)
    set(ui_fixture "${XIAOTAI_ROOT}/sdk/xiaotai_ui/test")
    set(integration "${XIAOTAI_ROOT}/tests/sdk/integration")
    xiaotai_host_test(app_door_templates SOURCES "${integration}/door_wide_test.c" "${ui_fixture}/ui_fake_board.cc"
        LINK xiaotai_app_door xiaotai_theme_lcd_kit lvgl::lvgl INCLUDE ../src "${ui_fixture}" "${CMAKE_CURRENT_SOURCE_DIR}" ROLES lvgl)
    add_test(NAME app_door_font_source COMMAND Python3::Interpreter -B "${XIAOTAI_ROOT}/scripts/generate_door_fonts.py" --check)
    set_tests_properties(app_door_font_source PROPERTIES LABELS xiaotai_app_door TIMEOUT 120)
    if(XIAOTAI_HOST_SANITIZE)
        add_test(NAME app_door_fonts COMMAND app_door_templates fonts)
        set_tests_properties(app_door_fonts PROPERTIES LABELS xiaotai_app_door TIMEOUT 120)
    endif()
endif()
