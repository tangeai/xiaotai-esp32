"""Opt-in BSS storage experiment: exact scope, zeroing, alignment and free ABI."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec_weight.inc").read_text()
cmake = (root / "components/starter_media/CMakeLists.txt").read_text()
assert 'option(XIAOTAI_BSS_WEIGHT_INTERNAL' in cmake
assert '"-Wl,--wrap=dl_lib_free"' in cmake
assert "241c2450b46bcf15d7f221f8afc398dea365bcdd170cdd9efc195318a6af31ca" in cmake
assert "fa3c9c299a566f2648b64e8136def1673238f8ea24a1aabeeddae5b4f9fcc2b3" in cmake

prefix = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static struct { atomic_uintptr_t owner; unsigned count; } s_afe_alloc;
static uintptr_t task = 1;
static bool fail_internal;
static unsigned allocated, freed, vendor_freed;
static void *xTaskGetCurrentTaskHandle(void) { return (void *)task; }
static void *heap_caps_aligned_calloc(size_t align, size_t count, size_t size, unsigned caps) {
    assert(align == 16 && count == 4096 && size == 4 && caps == 3);
    if (fail_internal) return NULL;
    void *p = NULL; assert(!posix_memalign(&p, align, count * size));
    memset(p, 0, count * size); ++allocated; return p;
}
static void heap_caps_free(void *p) { assert(p); ++freed; free(p); }
'''
test = r'''
/* Faithful vendor contract: base is stored immediately before aligned data. */
static void *vendor_allocate(void) {
    void *base = malloc(96); assert(base);
    uintptr_t aligned = ((uintptr_t)base + sizeof(void *) + 15U) & ~(uintptr_t)15U;
    ((void **)aligned)[-1] = base; return (void *)aligned;
}
void __real_dl_lib_free(void *p) { ++vendor_freed; if (p) free(((void **)p)[-1]); }
int main(void) {
    assert(!afe_weight_allocate(4096,4,16));
    atomic_store(&s_afe_alloc.owner,1);
    for (unsigned n=0; n<24; ++n) {
        s_afe_alloc.count=n;
        void *p=afe_weight_allocate(4096,4,16);
        if (n != 11) assert(!p);
        else {
            assert(p && !((uintptr_t)p & 15U));
            for (unsigned i=0; i<16384; ++i) assert(!((unsigned char *)p)[i]);
            assert(!afe_weight_allocate(4096,4,16));
            void *foreign=vendor_allocate(); __wrap_dl_lib_free(foreign);
            assert(atomic_load(&s_afe_weight)==(uintptr_t)p);
            __wrap_dl_lib_free(NULL); assert(atomic_load(&s_afe_weight)==(uintptr_t)p);
            __wrap_dl_lib_free(p); assert(!atomic_load(&s_afe_weight));
        }
    }
    assert(allocated==1 && freed==1 && vendor_freed==2);
    s_afe_alloc.count=11;
    task=2; assert(!afe_weight_allocate(4096,4,16)); task=1;
    assert(!afe_weight_allocate(2048,4,16));
    assert(!afe_weight_allocate(4096,2,16));
    assert(!afe_weight_allocate(4096,4,32));
    fail_internal=true; assert(!afe_weight_allocate(4096,4,16));
    assert(!atomic_load(&s_afe_weight)); fail_internal=false;
    for (unsigned n=0; n<100; ++n) {
        void *p=afe_weight_allocate(4096,4,16); assert(p);
        __wrap_dl_lib_free(p); assert(!atomic_load(&s_afe_weight));
    }
    assert(allocated==101 && freed==101);
    puts("PASS: opt-in 16-KiB BSS weight, owner/shape/index guard, alignment/zeroing, OOM and 101 vendor-safe releases");
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-bss-weight-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(prefix + source + test)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
