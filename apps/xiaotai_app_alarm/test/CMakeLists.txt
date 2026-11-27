# SPDX-License-Identifier: MIT
xiaotai_host_test(app_alarm SOURCES alarm_test.c ../src/alarm.c ../src/alarm_sounds.c LINK xiaotai_port xiaotai_framework xiaotai_at
    INCLUDE_ONLY xiaotai_app_alarm xiaotai_mod_clock xiaotai_ui xiaotai_feedback xiaotai_sounds)
if(ai_tools)
    # The tools on fakes of the schedules and of the registry; the argument
    # reads are the ai_talk module's own source.
    xiaotai_host_test(app_alarm_ai_tools SOURCES alarm_ai_tools_test.c ../src/alarm_ai_tools.c
            "${XIAOTAI_ROOT}/sdk/modules/xiaotai_mod_ai_talk/src/ai_talk_args.c"
        INCLUDE ../src INCLUDE_ONLY xiaotai_app_alarm xiaotai_mod_clock xiaotai_mod_ai_talk
        LINK xiaotai_port xiaotai_json)
endif()
foreach(case pages ring busy late timeout config timer_settings snooze)
    add_test(NAME app_alarm_${case} COMMAND app_alarm ${case})
    set_tests_properties(app_alarm_${case} PROPERTIES LABELS xiaotai_app_alarm TIMEOUT 60)
endforeach()
