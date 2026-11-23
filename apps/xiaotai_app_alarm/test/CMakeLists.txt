# SPDX-License-Identifier: MIT
xiaotai_host_test(app_alarm SOURCES alarm_test.c ../src/alarm.c LINK xiaotai_port xiaotai_framework xiaotai_at
    INCLUDE_ONLY xiaotai_app_alarm xiaotai_mod_clock xiaotai_ui xiaotai_feedback xiaotai_sounds)
foreach(case pages ring busy timeout config timer_settings snooze)
    add_test(NAME app_alarm_${case} COMMAND app_alarm ${case})
    set_tests_properties(app_alarm_${case} PROPERTIES LABELS xiaotai_app_alarm TIMEOUT 60)
endforeach()
