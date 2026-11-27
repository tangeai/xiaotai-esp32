# SPDX-License-Identifier: MIT
set(src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
xiaotai_host_test(feedback_patterns SOURCES feedback_patterns_test.c LINK xiaotai_feedback INCLUDE "${src}")

function(feedback_case target test lib)
    xiaotai_host_test(${target} SOURCES feedback_test.c feedback_board.cc LINK ${lib} INCLUDE "${src}" ${ARGN})
    target_compile_definitions(${target} PRIVATE FEEDBACK_TEST="${test}")
    target_link_options(${target} PRIVATE
        -Wl,--wrap=xiaotai_time_us -Wl,--wrap=xiaotai_timer_start -Wl,--wrap=xiaotai_timer_cancel
        -Wl,--wrap=xiaotai_task_create -Wl,--wrap=xiaotai_task_wait_notify)
endfunction()

foreach(case IN ITEMS priority effect_cover roles no_task time_wrap capacity)
    feedback_case(feedback_${case} ${case} xiaotai_feedback)
endforeach()
feedback_case(feedback_owner_frames frames xiaotai_feedback)
feedback_case(feedback_owner_stack stack xiaotai_feedback STACK)
target_compile_definitions(feedback_owner_stack PRIVATE FEEDBACK_STACK=1)

if(XIAOTAI_HOST_SANITIZE)
    set(common_absolute ${common_sources})
    list(TRANSFORM common_absolute PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../")
    add_library(feedback_worker_fixture STATIC ${common_absolute} "${src}/player_none.c")
    target_include_directories(feedback_worker_fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    target_link_libraries(feedback_worker_fixture PUBLIC xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
    target_compile_definitions(feedback_worker_fixture PUBLIC CONFIG_XIAOTAI_FEEDBACK_TASK=1)
    feedback_case(feedback_task_frames frames feedback_worker_fixture)
    feedback_case(feedback_task_no_task no_task feedback_worker_fixture)
endif()

function(feedback_sound_executable target lib)
    xiaotai_host_test(${target} SOURCES feedback_sound_test.c feedback_board.cc LINK ${lib} INCLUDE "${src}")
    target_link_options(${target} PRIVATE
        -Wl,--wrap=xiaotai_time_us -Wl,--wrap=xiaotai_timer_start -Wl,--wrap=xiaotai_timer_cancel)
endfunction()

feedback_sound_executable(feedback_slots xiaotai_feedback)
if(TEST feedback_slots)
    foreach(case IN ITEMS busy groups loop sequence generation done no_media commands teardown
                          cancel_fifo cancel_queue_full cancel_semantics cancel_word)
        add_test(NAME feedback_${case} COMMAND feedback_slots ${case})
        set_tests_properties(feedback_${case} PROPERTIES LABELS xiaotai_feedback)
    endforeach()
endif()

set(common_absolute ${common_sources})
list(TRANSFORM common_absolute PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../")
add_library(feedback_media_fixture STATIC ${common_absolute} "${src}/feedback_player_media.c")
target_include_directories(feedback_media_fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(feedback_media_fixture PUBLIC xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at xiaotai_media)
feedback_sound_executable(feedback_default_player feedback_media_fixture)
target_compile_definitions(feedback_default_player PRIVATE FEEDBACK_ADAPTER=1)
