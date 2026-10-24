#pragma once
typedef unsigned UBaseType_t;
typedef int BaseType_t;
UBaseType_t heap_test_set_interrupt_mask(void);
void heap_test_clear_interrupt_mask(UBaseType_t previous);
BaseType_t xPortInIsrContext(void);
#define portSET_INTERRUPT_MASK_FROM_ISR() heap_test_set_interrupt_mask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(previous) heap_test_clear_interrupt_mask(previous)
