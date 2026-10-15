#pragma once
#define pdPASS 1
#include <stdint.h>
#define portBYTE_ALIGNMENT 16
typedef uint8_t StackType_t;
typedef struct { unsigned unused; } StaticTask_t;
void *xTaskCreateStatic(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                        unsigned priority, StackType_t *buffer, StaticTask_t *storage);
unsigned uxTaskGetStackHighWaterMark(void *task);
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle);
