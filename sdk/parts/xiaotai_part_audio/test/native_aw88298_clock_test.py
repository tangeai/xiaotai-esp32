# SPDX-License-Identifier: MIT
# Replaces legacy: tests/native/cores3/test_cores3_amplifier_clock.py
"""Run the real ESP clock hook with a register model, without ESP-IDF."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


MODEL = r"""
#include <assert.h>
typedef int xiaotai_err_t;
#define XIAOTAI_OK 0
#define XIAOTAI_FAIL (-1)
typedef struct { unsigned bits; } xt_audio_io_t;
typedef struct audio_codec_if audio_codec_if_t;
struct audio_codec_if {
    int (*get_reg)(const audio_codec_if_t *, int, int *);
    int (*set_reg)(const audio_codec_if_t *, int, int);
};
typedef struct { const audio_codec_if_t *driver; } xt_native_codec_t;
typedef struct { xt_native_codec_t native; unsigned bit_clocks; } aw88298_t;
static unsigned xt_audio_io_bit_clocks(xt_audio_io_t *io) { return io->bits; }
static int xt_native_codec_result(xt_native_codec_t *c, int result) { (void)c; return result; }

static int reg, reads, writes, fail_read, fail_write, corrupt;
static int get(const audio_codec_if_t *d, int address, int *value)
{
    assert(d && address == 0x06);
    if (++reads == fail_read) return XIAOTAI_FAIL;
    *value = reg ^ (reads == 2 && corrupt);
    return XIAOTAI_OK;
}
static int set(const audio_codec_if_t *d, int address, int value)
{
    assert(d && address == 0x06 && reads == 1);
    ++writes;
    if (fail_write) return XIAOTAI_FAIL;
    reg = value;
    return XIAOTAI_OK;
}
static void reset(int value)
{
    reg = value;
    reads = writes = fail_read = fail_write = corrupt = 0;
}
"""

CASES = r"""
int main(void)
{
    const audio_codec_if_t driver = {get, set};
    aw88298_t amp = {{&driver}, 64};
    xt_audio_io_t io = {64};
    for (int value = 0; value <= 0xffff; ++value) {
        reset(value);
        assert(clock_check(&amp.native, &io) == XIAOTAI_OK);
        assert(reg == ((value & ~0x30) | 0x20) && reads == 2 && writes == 1);
    }
    reset(0x3c03); fail_read = 1;
    assert(clock_check(&amp.native, &io) == XIAOTAI_FAIL && reads == 1 && !writes && reg == 0x3c03);
    reset(0x3c03); fail_write = 1;
    assert(clock_check(&amp.native, &io) == XIAOTAI_FAIL && reads == 1 && writes == 1 && reg == 0x3c03);
    reset(0x3c03); fail_read = 2;
    assert(clock_check(&amp.native, &io) == XIAOTAI_FAIL && reads == 2 && writes == 1);
    reset(0x3c03); corrupt = 1;
    assert(clock_check(&amp.native, &io) == XIAOTAI_FAIL && reads == 2 && writes == 1);
    reset(0x3c03); io.bits = 32;
    assert(clock_check(&amp.native, &io) == XIAOTAI_FAIL && !reads && !writes);
    amp.bit_clocks = 0;
    assert(clock_check(&amp.native, &io) == XIAOTAI_OK && !reads && !writes);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    cc = parser.parse_args().cc
    native = Path(__file__).resolve().parents[1] / "esp-idf" / "aw88298.c"
    hook = re.search(r"^static xiaotai_err_t clock_check\([^\n]+\)\n\{.*?^\}", native.read_text(encoding="utf-8"),
                     re.MULTILINE | re.DOTALL)
    assert hook is not None, "missing ESP AW88298 clock hook"
    assert not re.search(r"alloc|xiaotai_delay|vTaskDelay", hook[0])
    with tempfile.TemporaryDirectory(prefix="aw88298-clock-") as workspace:
        source = Path(workspace) / "clock_test.c"
        binary = Path(workspace) / "clock_test"
        source.write_text(MODEL + hook[0] + CASES, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([binary], check=True)
    print("PASS: native AW88298 64fs clock, 65536 register values, ordered I/O, and failure rejection")


if __name__ == "__main__":
    main()
