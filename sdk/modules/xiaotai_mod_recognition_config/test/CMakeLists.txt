# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_recognition_config_at SOURCES recognition_config_at_test.c
                  LINK xiaotai_mod_recognition_config)
target_link_options(mod_recognition_config_at PRIVATE
    "LINKER:--wrap=xiaotai_platform_request" "LINKER:--wrap=xiaotai_mem_stats")
add_test(NAME mod_recognition_config_at_xmini COMMAND mod_recognition_config_at xmini)
set_tests_properties(mod_recognition_config_at_xmini PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition_config)
