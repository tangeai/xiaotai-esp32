#pragma once
#include <assert.h>
typedef int portMUX_TYPE;
extern unsigned test_critical_depth;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock)                                                                   \
    do {                                                                                           \
        (void)(lock);                                                                              \
        ++test_critical_depth;                                                                     \
    } while (0)
#define portEXIT_CRITICAL(lock)                                                                    \
    do {                                                                                           \
        (void)(lock);                                                                              \
        assert(test_critical_depth);                                                               \
        --test_critical_depth;                                                                     \
    } while (0)
