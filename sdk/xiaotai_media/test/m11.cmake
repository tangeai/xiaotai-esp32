# SPDX-License-Identifier: MIT
set(m11_common m11_test.c fixed_speaker.c ../src/diag.c ../src/commands.c)
xiaotai_host_test(media_diag_commands_off SOURCES ${m11_common}
                  LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ../src)
xiaotai_host_test(media_diag_commands_on SOURCES ${m11_common}
                  ../src/trace.c ../src/at_test.c ../src/at_sound.c
                  LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ../src)
target_compile_definitions(media_diag_commands_on PRIVATE
                           CONFIG_XIAOTAI_MEDIA_TEST_COMMANDS=1 CONFIG_XIAOTAI_MEDIA_SOUND_COMMANDS=1
                           CONFIG_XIAOTAI_MEDIA_TRACE=1)
xiaotai_host_test(media_diag_commands_arrivals SOURCES ${m11_common}
                  ../src/trace.c ../src/at_test.c ../src/at_sound.c
                  LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ../src)
target_compile_definitions(media_diag_commands_arrivals PRIVATE
                           CONFIG_XIAOTAI_MEDIA_TEST_COMMANDS=1 CONFIG_XIAOTAI_MEDIA_SOUND_COMMANDS=1
                           CONFIG_XIAOTAI_MEDIA_TRACE=1 CONFIG_XIAOTAI_MEDIA_TRACE_ARRIVALS=1)
xiaotai_host_test(media_diagnostic_ownership SOURCES diagnostic_ownership_test.c
                  LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ../src)
target_link_options(media_diagnostic_ownership PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot")
if(XIAOTAI_HOST_SANITIZE)
    add_test(NAME media_diagnostic_ownership_default COMMAND media_diagnostic_ownership default)
    add_test(NAME media_diagnostic_no_stream COMMAND media_diagnostic_ownership no-stream)
    set_tests_properties(media_diagnostic_ownership_default media_diagnostic_no_stream
                         PROPERTIES LABELS xiaotai_media)
endif()
foreach(target media_diag_commands_on media_diag_commands_off media_diag_commands_arrivals)
    foreach(wrapped xiaotai_media_status xiaotai_media_info xiaotai_media_play_clips
                    xiaotai_media_stop xiaotai_media_record_stop media_session_record_alaw
                    xiaotai_media_uplink_tone xiaotai_media_echo_gate_set xiaotai_media_echo_gate_enabled
                    xiaotai_mem_has_external xiaotai_mutex_lock)
        target_link_options(${target} PRIVATE "LINKER:--wrap=${wrapped}")
    endforeach()
endforeach()
if(XIAOTAI_HOST_SANITIZE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME media_diag_help_manifest COMMAND "${Python3_EXECUTABLE}" -B
             "${CMAKE_CURRENT_SOURCE_DIR}/m11_manifest.py" "${CMAKE_CURRENT_SOURCE_DIR}/../engine.json"
             $<TARGET_FILE:media_diag_commands_off> $<TARGET_FILE:media_diag_commands_on>
             $<TARGET_FILE:media_diag_commands_arrivals>)
    set_tests_properties(media_diag_help_manifest PROPERTIES LABELS xiaotai_media)
endif()
