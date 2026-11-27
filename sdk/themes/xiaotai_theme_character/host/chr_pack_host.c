// SPDX-License-Identifier: MIT
/* The sprite pack on the host: the same file, all of it in the test binary. */
#include "chr_director.h"
#include "chr_pack_host.h"
#include "chr_raster.h"

__asm__(".section .rodata\n"
        ".balign 4\n"
        ".globl xiaotai_character_pack_start\n"
        "xiaotai_character_pack_start:\n"
        ".incbin \"" XIAOTAI_CHARACTER_PACK "\"\n"
        ".globl xiaotai_character_pack_end\n"
        "xiaotai_character_pack_end:\n"
        ".previous\n");

extern const unsigned char xiaotai_character_pack_start[], xiaotai_character_pack_end[];

const void *chr_host_pack(size_t *size)
{
    *size = (size_t)(xiaotai_character_pack_end - xiaotai_character_pack_start);
    return xiaotai_character_pack_start;
}

const struct chr_pack *chr_pack_carried(void)
{
    static chr_pack_t pack;
    size_t size = 0;
    const void *data = chr_host_pack(&size);
    return chr_pack_open(&pack, data, size) ? &pack : NULL;
}

void chr_pack_carried_close(void) {}

void chr_pack_carried_report(char *line, size_t capacity)
{
    (void)line;
    (void)capacity;
}
