# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_wake_gate SOURCES wake_test.c ../src/wake.c ../src/wake_at.c
    INCLUDE ../src INCLUDE_ONLY xiaotai_mod_wake xiaotai_wake xiaotai_media xiaotai_mod_ai_talk
    LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_at)
foreach(scenario disabled unavailable switch persisted detect announce inject selftest selftest-bad selftest-empty preroll_lost at)
    add_test(NAME mod_wake_${scenario} COMMAND mod_wake_gate ${scenario})
    set_tests_properties(mod_wake_${scenario} PROPERTIES LABELS xiaotai_mod_wake TIMEOUT 120)
endforeach()
