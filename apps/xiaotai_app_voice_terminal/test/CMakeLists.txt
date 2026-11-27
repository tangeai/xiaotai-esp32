# SPDX-License-Identifier: MIT
set(terminal_support fakes.c ../src/terminal.c ../src/terminal_key.c
    ../src/terminal_state.c ../src/terminal_tables.c ../src/terminal_reset.c ../src/terminal_binding.c
    ../src/with_environment.c)
xiaotai_host_test(app_terminal_key SOURCES terminal_test.c ${terminal_support} LINK xiaotai_framework xiaotai_interaction
    INCLUDE_ONLY xiaotai_app_voice_terminal xiaotai_mod_wechat xiaotai_mod_contacts
                 xiaotai_mod_binding xiaotai_mod_provisioning xiaotai_mod_environment xiaotai_mod_settings
                 xiaotai_feedback xiaotai_net xiaotai_platform
                 xiaotai_transport xiaotai_identity xiaotai_media
    INCLUDE ../src)
target_compile_definitions(app_terminal_key PRIVATE XIAOTAI_TERMINAL_WITH_ENVIRONMENT XIAOTAI_TERMINAL_WITH_SETTINGS)
target_link_options(app_terminal_key PRIVATE "LINKER:--wrap=interaction_poll_start,--wrap=xiaotai_time_us")
foreach(case state tables boot_button disabled no_network no_binding origin quiet reset reset_network reset_binding
        reset_timeout reset_cue_failure reset_store binding binding_defaults binding_start_interval)
    add_test(NAME app_terminal_${case} COMMAND app_terminal_key ${case})
    set_tests_properties(app_terminal_${case} PROPERTIES LABELS xiaotai_app_voice_terminal TIMEOUT 20)
endforeach()
