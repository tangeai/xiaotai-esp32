idf_component_register(SRCS "memory_probe.c" "memory_snapshot.c" INCLUDE_DIRS "include"
                       REQUIRES esp_timer heap freertos)
