# SPDX-License-Identifier: MIT
xiaotai_host_test(part_display SOURCES display_test.c LINK xiaotai_part_display)
target_link_options(part_display PRIVATE "LINKER:--wrap=xiaotai_delay_ms,--wrap=xiaotai_time_us")
