"""Exercise the real development-only, per-owner codec timing accumulator."""
import os
from pathlib import Path
import re
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_media.c").read_text()
match = re.search(r"EXT_RAM_BSS_ATTR static struct \{\n    uint64_t total_us;.*?\} s_codec_diag\[2\]\[2\];", source, re.S)
assert match
begin = source.index("static void record_codec_timing(")
end = source.index("\n}\n", begin) + 3
helper = source[begin:end]
tick_fields = re.search(r"DRAM_ATTR static struct \{\n    atomic_uintptr_t owner\[2\];.*?\} s_codec_cpu;", source, re.S)
assert tick_fields
tick_begin = source.index("static void IRAM_ATTR codec_cpu_tick(void)")
tick_end = source.index("\n}\n", tick_begin) + 3
tick_helper = source[tick_begin:tick_end]
assert "s_codec_diag[2][2]" in match.group()
assert "record_codec_timing(0, (unsigned)xPortGetCoreID(), encode_us)" in source
assert "record_codec_timing(1, (unsigned)xPortGetCoreID()," in source

code = """
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#define EXT_RAM_BSS_ATTR
#define DRAM_ATTR
#define IRAM_ATTR
static unsigned fake_core;
static uintptr_t fake_task;
static unsigned xPortGetCoreID(void) { return fake_core; }
static uintptr_t xTaskGetCurrentTaskHandle(void) { return fake_task; }
""" + match.group() + "\n" + helper + "\n" + tick_fields.group() + "\n" + tick_helper + """
int main(void) {
    uint64_t sums[2][2] = {0};
    uint32_t counts[2][2] = {0};
    for (unsigned i = 0; i < 100000; ++i) {
        unsigned role = i % 2, core = (i / 2) % 2;
        uint32_t elapsed = i % 7 == 0 ? UINT32_MAX : i * 173U;
        sums[role][core] += elapsed;
        ++counts[role][core];
        record_codec_timing(role, core, elapsed);
        assert(s_codec_diag[role][core].total_us == sums[role][core]);
        assert(atomic_load(&s_codec_diag[role][core].frames) == counts[role][core]);
        assert(atomic_load(&s_codec_diag[role][core].avg_us) == sums[role][core] / counts[role][core]);
    }
    unsigned char before[sizeof(s_codec_diag)];
    memcpy(before, s_codec_diag, sizeof(before));
    record_codec_timing(2, 0, 100);
    record_codec_timing(0, 2, 100);
    assert(memcmp(before, s_codec_diag, sizeof(before)) == 0);
    atomic_store(&s_codec_diag[0][0].frames, UINT32_MAX);
    uint64_t sum = s_codec_diag[0][0].total_us;
    record_codec_timing(0, 0, 100);
    assert(s_codec_diag[0][0].total_us == sum);
    assert(atomic_load(&s_codec_diag[0][0].frames) == UINT32_MAX);
    fake_task = 123;
    for (fake_core = 0; fake_core < 2; ++fake_core) {
        for (unsigned role = 0; role < 2; ++role) {
            codec_cpu_tick();
            assert(atomic_load(&s_codec_cpu.running[role][fake_core]) == 0);
            atomic_store(&s_codec_cpu.owner[role], fake_task);
            for (unsigned i = 0; i < 10000; ++i) codec_cpu_tick();
            assert(atomic_load(&s_codec_cpu.running[role][fake_core]) == 10000);
            fake_task = 456;
            codec_cpu_tick();
            assert(atomic_load(&s_codec_cpu.running[role][fake_core]) == 10000);
            atomic_store(&s_codec_cpu.owner[role], 0);
            fake_task = 123;
        }
    }
    fake_core = 2;
    codec_cpu_tick();
    for (unsigned role = 0; role < 2; ++role)
        for (unsigned core = 0; core < 2; ++core)
            assert(atomic_load(&s_codec_cpu.running[role][core]) == 10000);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="codec-timing-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall",
                    "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: real codec timing; 100000 calls, 40000 tick samples, owner/core isolation, 64-bit totals, counter saturation")
