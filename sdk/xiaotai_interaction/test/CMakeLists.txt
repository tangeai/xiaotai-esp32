# SPDX-License-Identifier: MIT
set(interaction_sources ../src/interaction_core.c ../src/interaction_recognizer.c
    ../src/interaction_task.c ../src/interaction_commands.c)
set(interaction_fixtures INCLUDE ../src INCLUDE_ONLY xiaotai_hal xiaotai_interaction
    LINK xiaotai_port xiaotai_framework xiaotai_at)

xiaotai_host_test(interaction_clicks SOURCES interaction_test.c ${interaction_sources} ${interaction_fixtures})
xiaotai_host_test(interaction_task_poll SOURCES interaction_test.c ${interaction_sources} ${interaction_fixtures})
target_compile_definitions(interaction_task_poll PRIVATE CONFIG_XIAOTAI_INTERACTION_TASK=1)
xiaotai_host_test(interaction_owner_stack SOURCES interaction_test.c ${interaction_sources} ${interaction_fixtures} STACK)
if(TARGET interaction_owner_stack)
    target_compile_definitions(interaction_owner_stack PRIVATE INTERACTION_STACK_TEST=1)
endif()
foreach(target interaction_clicks interaction_task_poll interaction_owner_stack)
    if(TARGET ${target})
        target_link_options(${target} PRIVATE "LINKER:--wrap=interaction_poll_start")
    endif()
endforeach()

foreach(scenario long wrap void rules apps passthrough contact inject events saturation reject_saturation capacity level owner_poll gap no_profile no_input at)
    add_test(NAME interaction_${scenario} COMMAND interaction_clicks ${scenario})
    set_tests_properties(interaction_${scenario} PROPERTIES LABELS xiaotai_interaction TIMEOUT 120)
endforeach()
foreach(scenario clicks long wrap void rules apps passthrough contact inject events saturation reject_saturation capacity level gap no_profile no_input at)
    add_test(NAME interaction_task_${scenario} COMMAND interaction_task_poll ${scenario})
    set_tests_properties(interaction_task_${scenario} PROPERTIES LABELS xiaotai_interaction TIMEOUT 120)
endforeach()
