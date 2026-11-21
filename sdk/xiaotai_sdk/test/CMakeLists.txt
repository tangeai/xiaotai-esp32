# SPDX-License-Identifier: MIT
# The public API is usable from C and from C++ products.
xiaotai_host_test(sdk_public_api SOURCES public_api_test.c public_api_cpp.cc LINK xiaotai_sdk)
# xiaotai_start() against a generated selection, a product glue and a board,
# all supplied by the test.
xiaotai_host_test(sdk_start SOURCES start_test.c LINK xiaotai_sdk)
xiaotai_host_test(sdk_caller_start SOURCES caller_start_test.c ../src/xiaotai.c LINK xiaotai_sdk)
target_compile_definitions(sdk_caller_start PRIVATE CONFIG_XIAOTAI_FRAMEWORK_OWNER_ON_MAIN=1)
