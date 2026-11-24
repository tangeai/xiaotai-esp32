# SPDX-License-Identifier: MIT
# The conversation's device tools on fakes of the settings, the clock port and
# the tool registry; the argument reads are the ai_talk module's own source.
if(xiaotai_mod_ai_talk IN_LIST selected)
    xiaotai_host_test(app_companion_ai_device_tools
        SOURCES ai_device_tools_test.c ../src/ai_device_tools.c
                "${XIAOTAI_ROOT}/sdk/modules/xiaotai_mod_ai_talk/src/ai_talk_args.c"
        INCLUDE ../src INCLUDE_ONLY xiaotai_app_companion
        LINK xiaotai_port xiaotai_json)
    target_link_options(app_companion_ai_device_tools PRIVATE
        "LINKER:--wrap=xiaotai_clock_synced,--wrap=xiaotai_clock_unix_ms,--wrap=xiaotai_clock_local")
endif()
