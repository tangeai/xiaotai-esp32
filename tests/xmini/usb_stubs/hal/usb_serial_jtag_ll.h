#pragma once
#include <stdint.h>
typedef struct {
    struct { uint32_t sof_frame_index; } fram_num;
} usb_serial_jtag_test_dev_t;
extern volatile usb_serial_jtag_test_dev_t USB_SERIAL_JTAG;
