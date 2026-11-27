# SPDX-License-Identifier: MIT
xiaotai_host_test(app_doorbell SOURCES doorbell_test.c ../src/doorbell.c LINK xiaotai_framework
    INCLUDE_ONLY xiaotai_app_doorbell xiaotai_mod_call xiaotai_mod_contacts xiaotai_mod_binding xiaotai_mod_provisioning xiaotai_feedback xiaotai_interaction
                 xiaotai_sounds xiaotai_eventlog xiaotai_platform xiaotai_transport xiaotai_identity xiaotai_media
                 xiaotai_net)
target_link_options(app_doorbell PRIVATE "LINKER:--wrap=xiaotai_time_us")
foreach(case look setup mode)
    add_test(NAME app_doorbell_${case} COMMAND app_doorbell ${case})
    set_tests_properties(app_doorbell_${case} PROPERTIES LABELS xiaotai_app_doorbell TIMEOUT 30)
endforeach()
