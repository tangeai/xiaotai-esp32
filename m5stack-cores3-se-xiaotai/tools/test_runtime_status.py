"""Exercise the real lightweight status read and owner-only stack sampling."""
from pathlib import Path
import subprocess
import tempfile
from test_captive_portal_contract import function_body

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")
status = function_body(source, "starter_runtime_status")
sample = function_body(source, "runtime_sample_stack")
owner = function_body(source, "runtime_task")
assert "runtime_sample_stack(now_ms(), &next_stack_sample_ms)" in owner
assert "uxTaskGetStackHighWaterMark" not in status
assert "static EXT_RAM_BSS_ATTR atomic_uint s_public_stack_min_plus_one" in source
assert "#define RUNTIME_STACK_SAMPLE_MS 1000U" in source

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#define RUNTIME_STACK_SAMPLE_MS 1000U
typedef enum { WAITING, ACTIVE } starter_runtime_state_t;
typedef struct {
    starter_runtime_state_t state;
    uint32_t session_generation, connection_generation;
    int last_error;
    uint32_t stack_high_water_bytes;
} starter_runtime_status_t;
static atomic_int s_public_state, s_last_error;
static atomic_uint s_public_session_generation, s_public_connection_generation;
static atomic_uint s_public_stack_min_plus_one;
static unsigned scans, measured = 21568;
static unsigned uxTaskGetStackHighWaterMark(void *task) {
    assert(task == NULL); ++scans; return measured;
}
static void runtime_sample_stack(int64_t current_ms, int64_t *next_sample_ms) { /* SAMPLE_BODY */ }
static starter_runtime_status_t starter_runtime_status(void) { /* STATUS_BODY */ }
int main(void) {
    assert(starter_runtime_status().stack_high_water_bytes == 0 && scans == 0);
    int64_t due = 0;
    runtime_sample_stack(0, &due);
    assert(scans == 1 && due == 1000);
    atomic_store(&s_public_state, ACTIVE);
    atomic_store(&s_public_session_generation, 7);
    atomic_store(&s_public_connection_generation, 4);
    atomic_store(&s_last_error, -17);
    for (unsigned i = 0; i < 10000; ++i) {
        starter_runtime_status_t s = starter_runtime_status();
        assert(s.state == ACTIVE && s.session_generation == 7 && s.connection_generation == 4);
        assert(s.last_error == -17 && s.stack_high_water_bytes == 21568 && scans == 1);
    }
    runtime_sample_stack(999, &due);
    assert(scans == 1);
    measured = 11840;
    runtime_sample_stack(1000, &due);
    assert(scans == 2 && due == 2000 && starter_runtime_status().stack_high_water_bytes == 11840);
    measured = 16000;
    runtime_sample_stack(5000, &due);
    assert(scans == 3 && due == 6000 && starter_runtime_status().stack_high_water_bytes == 11840);
    runtime_sample_stack(5000, &due);
    assert(scans == 3);
    measured = 0;
    runtime_sample_stack(6000, &due);
    assert(scans == 4 && starter_runtime_status().stack_high_water_bytes == 0);
    measured = 1000;
    runtime_sample_stack(7000, &due);
    assert(scans == 5 && starter_runtime_status().stack_high_water_bytes == 0);
    puts("PASS: 10000 status reads do not scan; owner samples once/s, retains minimum and publishes state/error unchanged");
}
'''.replace("/* SAMPLE_BODY */", sample).replace("/* STATUS_BODY */", status)
with tempfile.TemporaryDirectory(prefix="runtime-status-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    c.write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
