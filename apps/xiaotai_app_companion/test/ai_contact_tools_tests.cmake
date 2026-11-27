# SPDX-License-Identifier: MIT
if(xiaotai_mod_ai_talk IN_LIST selected AND xiaotai_mod_contacts IN_LIST selected)
    xiaotai_host_test(app_companion_ai_call
        SOURCES ai_contact_tools_test.c ../src/ai_contact_tools.c ../src/companion_contact_route.c
        INCLUDE ../src INCLUDE_ONLY xiaotai_app_companion
        LINK xiaotai_port xiaotai_framework xiaotai_json)
    target_link_options(app_companion_ai_call PRIVATE "LINKER:--wrap=xiaotai_mem_alloc")
    if(XIAOTAI_HOST_SANITIZE)
        add_test(NAME app_companion_contact_query COMMAND app_companion_ai_call query)
        add_test(NAME app_companion_contact_query_lifecycle COMMAND app_companion_ai_call query-lifecycle)
        set_tests_properties(app_companion_contact_query app_companion_contact_query_lifecycle
            PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endif()
endif()
