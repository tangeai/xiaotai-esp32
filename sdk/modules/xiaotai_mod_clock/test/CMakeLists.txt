# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_clock_alarm SOURCES clock_schedule_test.c clock_test.c LINK xiaotai_mod_clock)
target_link_options(mod_clock_alarm PRIVATE -Wl,--wrap=xiaotai_clock_init)
foreach(scenario IN ITEMS countdown step spring gate config at)
    add_test(NAME mod_clock_${scenario} COMMAND mod_clock_alarm ${scenario})
    set_tests_properties(mod_clock_${scenario} PROPERTIES LABELS xiaotai_mod_clock TIMEOUT 15)
endforeach()
xiaotai_host_test(mod_clock_store SOURCES clock_store_test.c clock_test.c LINK xiaotai_mod_clock)
target_link_options(mod_clock_store PRIVATE -Wl,--wrap=xiaotai_clock_init)
