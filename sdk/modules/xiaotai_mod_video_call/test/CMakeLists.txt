# SPDX-License-Identifier: MIT
set(test_program mod_video_call_session)
if(DEFINED XIAOTAI_VIDEO_CALL_HOST_SELECTED)
    set(test_program mod_video_call_select)
endif()
xiaotai_host_test(${test_program}
    SOURCES video_call_test.c
    INCLUDE ../src
    INCLUDE_ONLY xiaotai_mod_call xiaotai_mod_wechat xiaotai_ui
    LINK xiaotai_mod_video_call)

if(DEFINED XIAOTAI_VIDEO_CALL_HOST_SELECTED)
    foreach(option IN ITEMS call wechat ui)
        if(option IN_LIST XIAOTAI_VIDEO_CALL_HOST_SELECTED)
            string(TOUPPER "${option}" option_upper)
            target_compile_definitions(${test_program} PRIVATE "TEST_WITH_${option_upper}=1")
        endif()
    endforeach()
    target_compile_definitions(${test_program} PRIVATE TEST_SELECT=1)
endif()
set(scenarios contrib peer_streams configure unavailable)
if(NOT DEFINED XIAOTAI_VIDEO_CALL_HOST_SELECTED)
    list(APPEND scenarios subscribe camera missing_camera unsupported_sensor
         profile profile_wt99 profile_headless profile_default profile_receive_only at)
endif()
foreach(scenario IN LISTS scenarios)
    add_test(NAME mod_video_call_${scenario} COMMAND ${test_program} ${scenario})
    set_tests_properties(mod_video_call_${scenario} PROPERTIES LABELS xiaotai_mod_video_call TIMEOUT 15)
endforeach()
