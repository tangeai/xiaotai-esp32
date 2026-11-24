# SPDX-License-Identifier: MIT
# Host tests of the UI system, under the address and undefined-behaviour
# sanitizers. The tests include ../src for the internal interface (the slots
# taken at exact times, the store's copy and comparison). ui_fake_board.cc is
# the board with the display parts a test sets (xiaotai_hal_init() runs once
# per process, so each test that needs displays links it and sets its own);
# ui_fake_theme.c holds fake themes and templates.
#
#   ui_store     copies, limits, the retention order, identity, change bits
#   ui_layers    layer rules, durations, coalescing, views before start,
#                memory of the slots, the framework's view sink
#   ui_registry  themes (scores, names, capacity, the product's theme and its
#                refusal), templates, images; start failures
set(src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
xiaotai_host_test(ui_store SOURCES ui_store_test.c LINK xiaotai_ui INCLUDE "${src}")
xiaotai_host_test(ui_layers SOURCES ui_layers_test.c ui_fake_board.cc ui_fake_theme.c LINK xiaotai_ui
                  INCLUDE "${src}")
xiaotai_host_test(ui_registry SOURCES ui_registry_test.c ui_fake_board.cc ui_fake_theme.c LINK xiaotai_ui
                  INCLUDE "${src}")
foreach(case IN ITEMS focus digits hold local_hold swipe timer stale_press)
    xiaotai_host_test(ui_${case} SOURCES ui_scene_input_test.c ui_fake_board.cc ui_fake_theme.c
                      LINK xiaotai_ui INCLUDE "${src}")
    target_compile_definitions(ui_${case} PRIVATE UI_TEST_CASE="${case}")
endforeach()

function(ui_run_case target case)
    xiaotai_host_test(${target} SOURCES ui_driver_test.c ui_fake_board.cc LINK xiaotai_ui INCLUDE "${src}" ${ARGN})
    target_compile_definitions(${target} PRIVATE UI_DRIVER_CASE="${case}")
    target_link_options(${target} PRIVATE
        "LINKER:--wrap=xiaotai_time_us,--wrap=xiaotai_timer_start,--wrap=xiaotai_timer_cancel,--wrap=xiaotai_task_create,--wrap=xiaotai_task_wait_notify")
endfunction()
ui_run_case(ui_frame_dedup frames)
ui_run_case(ui_frame_task task)
ui_run_case(ui_frame_rotation rotation)
ui_run_case(ui_owner_timer timer)
ui_run_case(ui_sleep power)
ui_run_case(ui_wake_gate dark_operates)
ui_run_case(ui_diag_owner diag_owner)
ui_run_case(ui_diag_task diag_task)
ui_run_case(ui_diag_wait diag_wait)
ui_run_case(ui_inject inject)
ui_run_case(ui_owner_stack stack STACK)
target_compile_definitions(ui_owner_stack PRIVATE UI_OWNER_STACK=1)
xiaotai_host_test(ui_commands SOURCES ui_commands_test.c ui_fake_board.cc LINK xiaotai_ui INCLUDE "${src}")
if(NOT XIAOTAI_HOST_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(XIAOTAI_HOST_PYTHON "${Python3_EXECUTABLE}")
endif()
add_test(NAME ui_screen_decode COMMAND "${XIAOTAI_HOST_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ui_screen_decode_test.py")
set_tests_properties(ui_screen_decode PROPERTIES TIMEOUT 15 LABELS "xiaotai_ui")
xiaotai_host_test(ui_video_builtin SOURCES ui_video_builtin_test.c "${src}/with_video.c"
                  LINK xiaotai_ui INCLUDE "${src}")
xiaotai_host_test(ui_video_none SOURCES ui_video_none_test.c "${src}/video_none.c"
                  INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../include"
                  INCLUDE_ONLY xiaotai_hal xiaotai_framework xiaotai_port)
if(lvgl_root)
    xiaotai_host_test(ui_cores3_screen_sender SOURCES ui_commands_test.c ui_fake_board.cc
                      LINK xiaotai_ui lvgl::lvgl INCLUDE "${src}" ROLES lvgl)
    target_compile_definitions(ui_cores3_screen_sender PRIVATE UI_CORE_SCREEN=1)
    target_link_options(ui_cores3_screen_sender PRIVATE
        "LINKER:--wrap=lv_draw_buf_init" "LINKER:--wrap=xiaotai_mem_aligned")
    add_test(NAME ui_cores3_screen_wire COMMAND "${XIAOTAI_HOST_PYTHON}" -B
             "${CMAKE_CURRENT_SOURCE_DIR}/cores3_screen_wire_test.py" $<TARGET_FILE:ui_cores3_screen_sender>)
    set_tests_properties(ui_cores3_screen_wire PROPERTIES TIMEOUT 15 LABELS "xiaotai_ui")
    foreach(idf_case IN ITEMS port 504 555 602 no_dsi)
        set(idf_demo ui_lvgl_idf_${idf_case})
        xiaotai_host_test(${idf_demo} SOURCES ui_lvgl_idf_test.c ui_fake_board.cc "${src}/../esp-idf/ui_lvgl_idf.c"
                          LINK xiaotai_ui INCLUDE "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/idf_stubs")
        target_link_options(${idf_demo} PRIVATE "LINKER:--wrap=xiaotai_mem_aligned")
        if(NOT idf_case STREQUAL "no_dsi")
            target_link_options(${idf_demo} PRIVATE "LINKER:--wrap=lv_display_create")
        endif()
    endforeach()
    target_compile_definitions(ui_lvgl_idf_504 PRIVATE UI_TEST_IDF_504=1)
    target_compile_definitions(ui_lvgl_idf_555 PRIVATE UI_TEST_IDF_555=1)
    target_compile_definitions(ui_lvgl_idf_602 PRIVATE UI_TEST_IDF_602=1)
    target_compile_definitions(ui_lvgl_idf_no_dsi PRIVATE SOC_MIPI_DSI_SUPPORTED=0)
    xiaotai_host_test(ui_lvgl_idf_memory SOURCES ui_lvgl_mem_test.c "${src}/../esp-idf/ui_lvgl_mem.c"
                      INCLUDE "${lvgl_root}" "${CMAKE_CURRENT_SOURCE_DIR}/idf_stubs" INCLUDE_ONLY xiaotai_port)
    target_compile_definitions(ui_lvgl_idf_memory PRIVATE LV_CONF_SKIP=1 LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM)
    xiaotai_host_test(ui_lvgl_painter SOURCES ui_lvgl_painter_test.c ui_fake_board.cc
                      LINK xiaotai_ui INCLUDE "${src}")
    foreach(case IN ITEMS pointer even_areas video)
        xiaotai_host_test(ui_lvgl_${case} SOURCES ui_lvgl_backend_test.c ui_fake_board.cc
                          LINK xiaotai_ui INCLUDE "${src}")
        target_compile_definitions(ui_lvgl_${case} PRIVATE LVGL_CASE="${case}")
    endforeach()
endif()
