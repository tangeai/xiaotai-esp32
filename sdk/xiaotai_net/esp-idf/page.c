// SPDX-License-Identifier: MIT
#include "net_platform.h"

/* CMake embeds the NW7 asset as binary data, with no added terminator. */
extern const char setup_start[] __asm__("_binary_setup_html_start");
extern const char setup_end[] __asm__("_binary_setup_html_end");

const char *net_platform_page(size_t *size)
{
    if (size) *size = (size_t)((uintptr_t)setup_end - (uintptr_t)setup_start);
    return setup_start;
}
