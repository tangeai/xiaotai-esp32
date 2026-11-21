# SPDX-License-Identifier: MIT
# Cross-component contracts only when both real components were selected.
if(TARGET xiaotai_mod_stream_video AND TARGET xiaotai_mod_ai_vision)
    set(stream_video_fixture "${XIAOTAI_ROOT}/sdk/modules/xiaotai_mod_stream_video/test/stream_video_test.c")
    set(stream_writer "${XIAOTAI_ROOT}/sdk/modules/xiaotai_mod_stream/src/stream_profile.c")
    xiaotai_host_test(mod_stream_video_ai_matrix SOURCES stream_video_ai_matrix_test.c
                      "${stream_video_fixture}" "${stream_writer}" INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}"
                      LINK xiaotai_mod_stream_video xiaotai_mod_ai_vision)
    target_compile_definitions(mod_stream_video_ai_matrix PRIVATE SV_TEST_CASE=4 SV_TEST_UI=1)
endif()

if(TARGET xiaotai_app_companion AND TARGET xiaotai_theme_lcd_kit)
    xiaotai_host_test(companion_lcd_emotions SOURCES companion_lcd_emotions_test.c
                      LINK xiaotai_app_companion xiaotai_theme_lcd_kit ROLES lvgl)
endif()

if(TARGET xiaotai_app_companion AND TARGET xiaotai_app_door AND TARGET xiaotai_mod_ai_talk AND
   (NOT DEFINED XIAOTAI_APP_COMPANION_WITH OR ("xiaotai_mod_ai_talk" IN_LIST XIAOTAI_APP_COMPANION_WITH AND
                                           "xiaotai_mod_call" IN_LIST XIAOTAI_APP_COMPANION_WITH)))
    xiaotai_host_test(app_companion_presenter SOURCES companion_presenter_test.c
                      "${XIAOTAI_ROOT}/sdk/xiaotai_ui/test/ui_fake_board.cc"
                      LINK xiaotai_app_companion xiaotai_app_door)
    target_link_options(app_companion_presenter PRIVATE
        "LINKER:--wrap=xiaotai_hal_needs" "LINKER:--wrap=xiaotai_net_status"
        "LINKER:--wrap=xiaotai_platform_status" "LINKER:--wrap=xiaotai_provisioning_status"
        "LINKER:--wrap=xiaotai_binding_status" "LINKER:--wrap=xiaotai_call_status"
        "LINKER:--wrap=xiaotai_ai_talk_status" "LINKER:--wrap=xiaotai_hal_input"
        "LINKER:--wrap=interaction_poll_start" "LINKER:--wrap=xiaotai_ai_talk_start" "LINKER:--wrap=xiaotai_ai_talk_stop")
    foreach(case IN ITEMS call setup binding overlap navigation standalone key_press key_click)
        add_test(NAME app_companion_presenter_${case} COMMAND app_companion_presenter ${case})
        set_tests_properties(app_companion_presenter_${case} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endforeach()
endif()

if(TARGET xiaotai_theme_lcd320 AND TARGET xiaotai_theme_lcd480)
    set(ui_fixture "${CMAKE_CURRENT_LIST_DIR}/../../../sdk/xiaotai_ui/test")
    set(kit_source "${CMAKE_CURRENT_LIST_DIR}/../../../sdk/themes/xiaotai_theme_lcd_kit/src")
    xiaotai_host_test(theme_lcd_face_live SOURCES lcd_face_live_test.c "${ui_fixture}/ui_fake_board.cc"
                      LINK xiaotai_theme_lcd320 xiaotai_theme_lcd480 lvgl::lvgl
                      INCLUDE "${kit_source}" "${ui_fixture}" "${ui_fixture}/../src" ROLES lvgl)
    if(TARGET theme_lcd_face_live)
        foreach(case IN ITEMS owner scaled wide)
            add_test(NAME theme_lcd_face_${case} COMMAND theme_lcd_face_live ${case})
            set_tests_properties(theme_lcd_face_${case} PROPERTIES LABELS integration TIMEOUT 120)
        endforeach()
    endif()
endif()

if(TARGET xiaotai_board AND TARGET xiaotai_net AND TARGET xiaotai_part_coprocessor AND
   XIAOTAI_BOARD STREQUAL "waveshare/esp32-p4-wifi6-touch-lcd-3.5")
    xiaotai_host_test(waveshare_c6_identity SOURCES waveshare_c6_identity_test.c
                      LINK xiaotai_board xiaotai_net xiaotai_part_coprocessor)
    target_link_options(waveshare_c6_identity PRIVATE
        "LINKER:--wrap=xiaotai_mem_has_external,--wrap=xiaotai_kv_get_u8"
        "LINKER:--wrap=xiaotai_coprocessor_link_wait,--wrap=xiaotai_coprocessor_link_ota_begin")
endif()
