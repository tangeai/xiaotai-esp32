"""Initialization probe preserves vendor allocation arguments and ownership."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec.c").read_text(encoding="utf-8")
allocation_state = "#define AFE_ALLOC_RECORDS" + source.split("#define AFE_ALLOC_RECORDS", 1)[1].split(
    "\n#if XIAOTAI_AFE_KERNEL_PROBE", 1)[0]
probe = source.split("/* Initialization-only allocation evidence.", 1)[1].split(
    "\n#endif\n\n#if !CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE", 1)[0]
probe = "/* Initialization-only allocation evidence." + probe
cmake = (root / "components/starter_media/CMakeLists.txt").read_text(encoding="utf-8")
for symbol in ("esp_bss_hps16fft_init", "dl_lib_calloc", "dl_lib_calloc_psram"):
    assert f'"-Wl,--wrap={symbol}"' in cmake
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
static uintptr_t task = 1;
static unsigned regular_calls, external_calls, init_calls;
static bool fail_alloc, fail_init, over_limit;
static int bands_expected, mode_expected;
static int data[2];
static void *xTaskGetCurrentTaskHandle(void) { return (void *)task; }
static bool esp_ptr_external_ram(void *p) { return p == &data[1]; }
static size_t heap_caps_get_free_size(unsigned caps) { assert(caps == 3 || caps == 6); return 100000; }
'''
allocators = r'''
static void *__real_dl_lib_calloc(int count, int size, int align) {
    assert(count == 12 && size == 4 && align == 16); ++regular_calls;
    return fail_alloc ? NULL : &data[0];
}
static void *__real_dl_lib_calloc_psram(int count, int size, int align) {
    assert(count == 20);
    assert(size == 2 && align == 16); ++external_calls;
    return fail_alloc ? NULL : &data[1];
}
static void *__real_esp_bss_hps16fft_init(int bands, int mode);
'''
test = r'''
static void *__real_esp_bss_hps16fft_init(int bands, int mode) {
    assert(bands == bands_expected && mode == mode_expected); ++init_calls;
    for (unsigned n = 0; n < (over_limit ? AFE_ALLOC_RECORDS + 3U : 1U); ++n) {
        assert(__wrap_dl_lib_calloc(12, 4, 16) == (fail_alloc ? NULL : &data[0]));
        assert(__wrap_dl_lib_calloc_psram(20, 2, 16) == (fail_alloc ? NULL : &data[1]));
    }
    return fail_init ? NULL : &data[0];
}
int main(void) {
    atomic_store(&s_afe_alloc.owner, 0);
    assert(__wrap_dl_lib_calloc(12, 4, 16) == &data[0]);
    assert(s_afe_alloc.count == 0 && regular_calls == 1);
    atomic_store(&s_afe_alloc.owner, 2);
    assert(__wrap_dl_lib_calloc_psram(20, 2, 16) == &data[1]);
    assert(s_afe_alloc.count == 0 && external_calls == 1);
    atomic_store(&s_afe_alloc.owner, 0);
    bands_expected = 64; mode_expected = 2;
    assert(__wrap_esp_bss_hps16fft_init(64, 2) == &data[0]);
    assert(!atomic_load(&s_afe_alloc.owner) && init_calls == 1);
    assert(s_afe_alloc.count == 2 && s_afe_alloc.omitted == 0);
    assert(s_afe_alloc.record[0].count == 12 && s_afe_alloc.record[0].size == 4);
    assert(s_afe_alloc.record[0].address == (uintptr_t)&data[0]);
    assert(s_afe_alloc.record[1].address == (uintptr_t)&data[1]);
    assert(!s_afe_alloc.record[0].psram_request && !s_afe_alloc.record[0].external);
    assert(s_afe_alloc.record[1].psram_request && s_afe_alloc.record[1].external);
    fail_init = fail_alloc = true;
    assert(__wrap_esp_bss_hps16fft_init(64, 2) == NULL);
    assert(!atomic_load(&s_afe_alloc.owner) && s_afe_alloc.count == 2);
    assert(s_afe_alloc.record[0].failed && s_afe_alloc.record[1].failed);
    fail_init = fail_alloc = false; over_limit = true;
    assert(__wrap_esp_bss_hps16fft_init(64, 2) == &data[0]);
    assert(s_afe_alloc.count == AFE_ALLOC_RECORDS && s_afe_alloc.omitted == 30U);
    assert(!atomic_load(&s_afe_alloc.owner));
    over_limit = false;
    atomic_store(&s_afe_alloc.owner, 2); /* Nested/non-owner initialization is untouched. */
    mode_expected = 2;
    unsigned recorded = s_afe_alloc.count;
    assert(__wrap_esp_bss_hps16fft_init(64, 2) == &data[0]);
    assert(atomic_load(&s_afe_alloc.owner) == 2 && s_afe_alloc.count == recorded);
    atomic_store(&s_afe_alloc.owner, 0);
    bands_expected = 128; mode_expected = 2;
    assert(__wrap_esp_bss_hps16fft_init(128, 2) == &data[0]);
    bands_expected = 64; mode_expected = 3;
    assert(__wrap_esp_bss_hps16fft_init(64, 3) == &data[0]);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-afe-alloc-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + allocation_state + allocators + probe + test, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: BSS allocation probe passes through pointers/arguments/failure and bounds records to its owner")
