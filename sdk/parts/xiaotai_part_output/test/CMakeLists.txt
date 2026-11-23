# SPDX-License-Identifier: MIT
xiaotai_host_test(part_output SOURCES output_test.c LINK xiaotai_part_output INCLUDE ../src ../host)
xiaotai_host_test(part_output_rmt SOURCES rmt_test.c ../esp-idf/led_rmt.c INCLUDE_ONLY xiaotai_part_output
                  LINK xiaotai_port INCLUDE ../src rmt_stubs)
