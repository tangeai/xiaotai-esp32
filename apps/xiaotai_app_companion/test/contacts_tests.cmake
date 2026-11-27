# SPDX-License-Identifier: MIT
if(xiaotai_mod_contacts IN_LIST selected)
    xiaotai_host_test(app_companion_contacts SOURCES contacts_test.c fakes.c ${core}
        ../src/with_contacts.c ../src/companion_contact_route.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
    xiaotai_host_test(app_companion_recents SOURCES contacts_test.c fakes.c ${core}
        ../src/with_contacts.c ../src/companion_contact_route.c ../src/companion_recents.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
    if(TARGET app_companion_recents)
        target_compile_definitions(app_companion_recents PRIVATE COMPANION_WITH_RECENTS=1)
    endif()
    if(XIAOTAI_HOST_SANITIZE)
        add_test(NAME app_companion_contacts_keys COMMAND app_companion_contacts keys)
        add_test(NAME app_companion_contact_entry COMMAND app_companion_contacts entry)
        add_test(NAME app_companion_contacts_plain COMMAND app_companion_contacts plain)
        add_test(NAME app_companion_recents_plain COMMAND app_companion_recents plain)
        set_tests_properties(app_companion_contacts_keys app_companion_contact_entry
            app_companion_contacts_plain app_companion_recents_plain
            PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endif()
endif()
