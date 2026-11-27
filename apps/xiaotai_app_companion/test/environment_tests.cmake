# SPDX-License-Identifier: MIT
if(TARGET xiaotai_mod_environment AND xiaotai_mod_environment IN_LIST selected)
    xiaotai_companion_parts(environment_parts "${app}/src" "${CMAKE_CURRENT_BINARY_DIR}/test_environment_parts.c"
        "${optional}" "xiaotai_mod_environment")
    xiaotai_host_test(app_companion_environment SOURCES environment_test.c fakes.c ${core} ${environment_parts}
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
    target_link_options(app_companion_environment PRIVATE "LINKER:--wrap=xiaotai_net_status")
endif()
