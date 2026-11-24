# SPDX-License-Identifier: MIT
xiaotai_host_test(part_camera_csi SOURCES camera_csi_test.c LINK xiaotai_part_camera_csi INCLUDE ../src ../host)
target_link_options(part_camera_csi PRIVATE -Wl,--wrap=xiaotai_mem_has_external -Wl,--wrap=xiaotai_mem_is_external)
target_link_options(part_camera_csi PRIVATE -Wl,--wrap=xiaotai_mem_aligned)

xiaotai_host_test(part_camera_csi_idf SOURCES camera_csi_idf_test.c ../esp-idf/camera_csi_io.c
                  LINK xiaotai_part_bus INCLUDE_ONLY xiaotai_part_camera_csi INCLUDE ../src idf_stubs)
target_compile_definitions(part_camera_csi_idf PRIVATE open=csi_test_open close=csi_test_close ioctl=csi_test_ioctl)
