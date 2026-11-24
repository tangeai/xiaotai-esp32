# SPDX-License-Identifier: MIT
# The product settings fixture boots the real identity engine. The framework
# library itself still depends only on the port (D31).
xiaotai_host_test(framework_core SOURCES framework_test.c
    "${XIAOTAI_ROOT}/tests/sdk/integration/framework_preferences.c" LINK xiaotai_framework xiaotai_identity)
target_link_options(framework_core PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(framework_stack_measure STACK SOURCES stack_measure_test.c LINK xiaotai_framework)
xiaotai_host_test(framework_command_completion SOURCES command_completion_test.c LINK xiaotai_framework)
target_link_options(framework_command_completion PRIVATE "LINKER:--wrap=xiaotai_signal_give")
xiaotai_host_test(framework_subscription_lifecycle SOURCES subscription_lifecycle_test.c
                  LINK xiaotai_framework INCLUDE ../src)
target_link_options(framework_subscription_lifecycle PRIVATE "LINKER:--wrap=xiaotai_mem_calloc"
                    "LINKER:--wrap=xiaotai_mem_alloc")
add_test(NAME framework_subscription_publication COMMAND framework_subscription_lifecycle --publication)
set_tests_properties(framework_subscription_publication PROPERTIES LABELS "xiaotai_framework" TIMEOUT 30)
xiaotai_host_test(framework_settings_storage SOURCES settings_storage_test.c LINK xiaotai_framework INCLUDE ../internal)
add_test(NAME framework_settings_confirmation_fallback COMMAND framework_settings_storage fallback)
set_tests_properties(framework_settings_confirmation_fallback PROPERTIES LABELS xiaotai_framework TIMEOUT 10)
xiaotai_host_test(framework_caller_owner SOURCES caller_owner_test.c LINK xiaotai_framework)
xiaotai_host_test(framework_owner_publication SOURCES owner_publication_test.c LINK xiaotai_framework)
target_link_options(framework_owner_publication PRIVATE "LINKER:--wrap=xiaotai_task_create"
    "LINKER:--wrap=xiaotai_task_wait_notify" "LINKER:--wrap=xiaotai_signal_init"
    "LINKER:--wrap=xiaotai_signal_deinit")
xiaotai_host_test(framework_tagged_queue SOURCES queue_tagged_test.c INCLUDE ../src LINK xiaotai_framework)
target_link_options(framework_tagged_queue PRIVATE "LINKER:--wrap=xiaotai_queue_init"
    "LINKER:--wrap=xiaotai_queue_send")
xiaotai_host_test(framework_terminal_reserve SOURCES terminal_reserve_test.c LINK xiaotai_framework)
target_link_options(framework_terminal_reserve PRIVATE "LINKER:--wrap=xiaotai_queue_send")
