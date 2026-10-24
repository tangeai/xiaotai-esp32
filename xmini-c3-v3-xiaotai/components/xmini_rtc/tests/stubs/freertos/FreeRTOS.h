#pragma once
#include <stddef.h>
#include <stdint.h>
#define pdTRUE 1
#define pdFALSE 0
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
