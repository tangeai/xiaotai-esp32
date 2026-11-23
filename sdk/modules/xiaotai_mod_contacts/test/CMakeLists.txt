# SPDX-License-Identifier: MIT
# Host tests of the contacts module (work package MC-1), under the address and
# undefined-behaviour sanitizers.
#
# mod_contacts_parse is the reply reader alone (pure: JSON text in, cache and
# counts out). Every other test runs the module's sources against the real
# runtime framework on the host port and a fake of the platform core
# (fake_platform.c: requests, replies, signals and the binding generation
# under the test's control); the platform library is therefore included, not
# linked. mod_contacts_needs_profile builds the module with
# CONFIG_XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE=1.
set(module_sources ../src/contacts.c ../src/contacts_parse.c ../src/contacts_fetch.c ../src/contacts_match.c
                   ../src/contacts_change.c ../src/contacts_at.c)
set(harness contacts_harness.c fake_platform.c)
set(links xiaotai_framework xiaotai_identity xiaotai_json xiaotai_eventlog xiaotai_port xiaotai_at)
set(src "${CMAKE_CURRENT_SOURCE_DIR}/../src")

xiaotai_host_test(mod_contacts_parse SOURCES contacts_parse_test.c ../src/contacts_parse.c
                  LINK xiaotai_json xiaotai_port INCLUDE_ONLY xiaotai_mod_contacts INCLUDE ${src})

foreach(name view refresh fresh kinds background match query change at at_kind provisioning)
    xiaotai_host_test(mod_contacts_${name} SOURCES contacts_${name}_test.c ${harness} ${module_sources}
                      LINK ${links} INCLUDE_ONLY xiaotai_mod_contacts xiaotai_net INCLUDE ${src})
endforeach()

target_link_options(mod_contacts_query PRIVATE "LINKER:--wrap=xiaotai_time_us")
add_test(NAME mod_contacts_query_owner_deadline COMMAND mod_contacts_query owner-deadline)
add_test(NAME mod_contacts_query_failed_reply_cache COMMAND mod_contacts_query failed-reply-cache)
set_tests_properties(mod_contacts_query_owner_deadline mod_contacts_query_failed_reply_cache
                     PROPERTIES LABELS xiaotai_mod_contacts TIMEOUT 30)

# A provisioning boot cannot transition to station mode in the same process.
if(XIAOTAI_HOST_SANITIZE)
    foreach(mode station voice-provision voice-station)
        string(REPLACE "-" "_" test_name "mod_contacts_provisioning_${mode}")
        add_test(NAME ${test_name} COMMAND mod_contacts_provisioning "${mode}")
        set_tests_properties(${test_name} PROPERTIES LABELS xiaotai_mod_contacts TIMEOUT 30)
    endforeach()
endif()

xiaotai_host_test(mod_contacts_needs_profile SOURCES contacts_needs_profile_test.c ${harness} ${module_sources}
                  LINK ${links} INCLUDE_ONLY xiaotai_mod_contacts xiaotai_net INCLUDE ${src})
target_compile_definitions(mod_contacts_needs_profile PRIVATE CONFIG_XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE=1)
