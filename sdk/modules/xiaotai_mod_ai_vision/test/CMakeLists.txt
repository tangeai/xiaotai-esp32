# SPDX-License-Identifier: MIT
# The production module runs with the real owner and settings lifecycle;
# harness.c replaces only the frame producer and its transport.
function(ai_vision_test name source)
    xiaotai_host_test(mod_ai_vision_${name}
        SOURCES ${source} harness.c ../src/ai_vision.c
        INCLUDE_ONLY xiaotai_mod_ai_vision
        LINK xiaotai_port xiaotai_framework xiaotai_eventlog)
endfunction()

ai_vision_test(session session_test.c)
ai_vision_test(unavailable unavailable_test.c)
ai_vision_test(failure failure_test.c)
ai_vision_test(contrib contrib_test.c)
ai_vision_test(switch switch_test.c)
