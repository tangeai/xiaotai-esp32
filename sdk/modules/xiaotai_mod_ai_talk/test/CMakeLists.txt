# SPDX-License-Identifier: MIT
function(ai_talk_test name)
    xiaotai_host_test(${name}
        SOURCES conversation_test.c ../src/ai_talk.c ../src/ai_talk_protocol.c ../src/ai_talk_caption.c
                ../src/ai_talk_tools.c ../src/ai_talk_at.c
        INCLUDE ../src INCLUDE_ONLY xiaotai_mod_ai_talk
        LINK xiaotai_port xiaotai_framework xiaotai_json xiaotai_eventlog xiaotai_at)
    target_link_options(${name} PRIVATE "LINKER:--wrap=xiaotai_mem_has_external,--wrap=xiaotai_time_us")
endfunction()

ai_talk_test(mod_ai_talk_start)

foreach(scenario token_matrix handshake end stop_scope preroll dropped contrib contrib_empty config caption caption_capacity
                 tools tools_long tools_xmini tools_external tools_outbound tools_scope tools_compat
                 tools_json tools_json_external at)
    add_test(NAME mod_ai_talk_${scenario} COMMAND mod_ai_talk_start ${scenario})
    set_tests_properties(mod_ai_talk_${scenario} PROPERTIES LABELS xiaotai_mod_ai_talk TIMEOUT 120)
endforeach()

foreach(scenario signaling_hold_off signaling_hold_on signaling_hold_record signaling_hold_ready
                 signaling_hold_timeout signaling_hold_expired signaling_hold_offline signaling_hold_reset
                 signaling_hold_reused signaling_hold_transport_stopped signaling_hold_timer_error
                 start_context handshake_deadline)
    add_test(NAME mod_ai_talk_${scenario} COMMAND mod_ai_talk_start ${scenario})
    set_tests_properties(mod_ai_talk_${scenario} PROPERTIES LABELS xiaotai_mod_ai_talk TIMEOUT 20)
endforeach()

ai_talk_test(mod_ai_talk_policy)
target_compile_definitions(mod_ai_talk_policy PRIVATE AI_TALK_POLICY_TEST=1
    CONFIG_XIAOTAI_AI_TALK_ROLE_REQUIRED=1 CONFIG_XIAOTAI_AI_TALK_START_NEEDS_PROFILE=1
    CONFIG_XIAOTAI_AI_TALK_NOTIFY_FAILURES=1 CONFIG_XIAOTAI_AI_TALK_DROPPED_ENDS=1)
