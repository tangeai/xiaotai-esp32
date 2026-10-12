"""Exercise the exact bounded histogram; Xtensa ISR PC semantics need hardware proof."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec_pc.inc").read_text(encoding="utf-8")
aec = (root / "components/starter_media/src/starter_aec.c").read_text(encoding="utf-8")
tick = aec.split("static void IRAM_ATTR afe_cpu_tick", 1)[1].split("static unsigned afe_cpu_begin", 1)[0]
assert "stage == 3U && running" in tick
assert '"rsr.epc1 %0\\nrsr.ccount %1"' in tick
sample = source.split("static void afe_pc_log", 1)[0]
for forbidden in ("ESP_LOG", "malloc", "esp_timer", "s_aec", "s_afe_alloc", "PSRAM"):
    assert forbidden not in sample
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define DRAM_ATTR
#define IRAM_ATTR
#define ESP_LOGI(tag, ...) do { (void)(tag); printf(__VA_ARGS__); puts(""); } while (0)
static const char *TAG = "test";
'''
test = r'''
static unsigned sum_hits(void) {
    unsigned result = 0;
    for (unsigned n = 0; n < AFE_PC_BUCKETS; ++n) result += atomic_load(&s_afe_pc.bucket[n].hits);
    return result;
}
static unsigned bucket(uint32_t pc) { return ((pc >> 5) ^ (pc >> 13)) & (AFE_PC_BUCKETS - 1); }
int main(void) {
    uint32_t cycle = 0;
    for (unsigned n = 0; n < AFE_PC_WARMUP; ++n) {
        afe_pc_sample(0x42000123, cycle); cycle += 240000;
    }
    assert(atomic_load(&s_afe_pc.seen) == AFE_PC_WARMUP && sum_hits() == 0);
    afe_pc_sample(0x42000123, cycle - 239999);
    assert(atomic_load(&s_afe_pc.duplicates) == 1 && sum_hits() == 0);
    afe_pc_sample(0x42000123, cycle); cycle += 240000;
    assert(sum_hits() == 1 && atomic_load(&s_afe_pc.bucket[bucket(0x42000120)].pc) == 0x42000120);
    afe_pc_sample(0x4200013f, cycle); cycle += 240000;
    assert(atomic_load(&s_afe_pc.bucket[bucket(0x42000120)].hits) == 2);
    s_afe_pc.last_cycle = UINT32_MAX - 200000;
    afe_pc_sample(0x42000120, 100000); cycle = 340000;
    assert(sum_hits() == 3); /* Unsigned cycle wrap remains one real sample. */
    unsigned collisions = 0;
    for (uint32_t pc = 0x42000200; collisions < 4; pc += 32) {
        if (bucket(pc) != bucket(0x42000120)) continue;
        afe_pc_sample(pc, cycle); cycle += 240000; ++collisions;
    }
    assert(sum_hits() == 6 && atomic_load(&s_afe_pc.omitted) == 1);
    afe_pc_sample(0, cycle); cycle += 240000;
    assert(atomic_load(&s_afe_pc.omitted) == 2);
    while (atomic_load(&s_afe_pc.seen) < AFE_PC_WARMUP + AFE_PC_SAMPLES) {
        afe_pc_sample(0x42000120, cycle); cycle += 240000;
    }
    assert(sum_hits() + atomic_load(&s_afe_pc.omitted) == AFE_PC_SAMPLES);
    unsigned sum = sum_hits(), duplicates = atomic_load(&s_afe_pc.duplicates);
    for (unsigned n = 0; n < 10000; ++n) afe_pc_sample(0x42009000, cycle);
    assert(sum_hits() == sum && atomic_load(&s_afe_pc.duplicates) == duplicates);
    afe_pc_log(); assert(sum_hits() == sum);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-aec-pc-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + source + test, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: PC histogram bounds/collisions/coverage/catch-up/wrap/freeze; no DSP data or heap access")
