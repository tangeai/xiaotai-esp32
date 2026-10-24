"""Compile the actual DUMP branch and check main-task scratch lifetimes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / "main/app_main.c").read_text(encoding="utf-8")
    begin = source.index("    case XMINI_CMD_AUDIO_DUMP: {")
    end = source.index("    case XMINI_CMD_AI_START:", begin)
    block = source[begin:end]
    assert block.index("audio.busy || !audio.recording_ready") < block.index("malloc(8000)")
    assert "xmini_audio_recording_copy(snapshot, 8000)" in block
    assert "#define s_business s_workspace.business" in source
    assert "#define s_frame s_workspace.frame" in source
    update_begin = source.index("static void update_business_ui(")
    update_end = source.index("static void show_status(", update_begin)
    update = source[update_begin:update_end]
    assert "xmini_audio_prompt(" not in update and "now + 5000" not in update
    identity_check = "xmini_feedback_identity_changed(s_previous_feedback, feedback_identity)"
    assert identity_check in update
    assert "if (feedback_changed) {" in update
    assert update.index("xmini_product_feedback(feedback);") < update.index("s_previous_feedback = feedback_identity;")
    loop = source[source.index("void app_main("):]
    assert loop.index("xmini_business_status(&s_business);") < loop.index(identity_check)
    assert loop.index(identity_check) < loop.index("xmini_product_feedback(-1);") < loop.index("xmini_product_poll(&ui);")
    assert loop.index("xmini_product_poll(&ui);") < loop.index("update_business_ui(&ui, now);")
    assert loop.index("update_business_ui(&ui, now);") < loop.index("xmini_ui_render(&ui, s_frame)")
    assert "s_business" not in loop[loop.index("xmini_ui_render(&ui, s_frame)"):]
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define XMINI_CMD_AUDIO_DUMP 3
typedef struct { bool busy, recording_ready; unsigned recording_source; } xmini_audio_status_t;
static xmini_audio_status_t state;
static unsigned allocations, copies, releases, headers, lines;
static bool allocation_fails, copy_fails;
static uint8_t bytes[8000];
static xmini_audio_status_t xmini_audio_status(void) { return state; }
static void *allocate(size_t size) { assert(size == 8000); ++allocations; return allocation_fails ? NULL : bytes; }
static void release(void *ptr) { assert(!ptr || ptr == bytes); ++releases; }
static int xmini_audio_recording_copy(uint8_t *out, size_t size) {
    assert(out == bytes && size == 8000); ++copies;
    if (copy_fails) return ESP_ERR_INVALID_STATE;
    memset(out, 0xd5, size); return ESP_OK;
}
static int output(const char *format, ...) {
    if (!strncmp(format, "+PCMA: bytes=", 13)) ++headers;
    else { assert(!strncmp(format, "+PCMA: offset=", 14)); ++lines; }
    return 0;
}
#define malloc allocate
#define free release
#define printf output
static int dump(void) {
    int result = ESP_OK;
    switch (XMINI_CMD_AUDIO_DUMP) {
/* ACTUAL_DUMP */
    }
    return result;
}
int main(void) {
    state = (xmini_audio_status_t){.busy=true, .recording_ready=true};
    assert(dump() == ESP_ERR_INVALID_STATE && allocations == 0 && copies == 0);
    state.busy = false; state.recording_ready = false;
    assert(dump() == ESP_ERR_INVALID_STATE && allocations == 0);
    state.recording_ready = true; allocation_fails = true;
    assert(dump() == ESP_ERR_NO_MEM && allocations == 1 && copies == 0 && releases == 1);
    allocation_fails = false; copy_fails = true;
    assert(dump() == ESP_ERR_INVALID_STATE && allocations == 2 && copies == 1 && releases == 2);
    copy_fails = false;
    assert(dump() == ESP_OK && allocations == 3 && copies == 2 && releases == 3);
    assert(headers == 1 && lines == 250);
    return 0;
}
'''.replace("/* ACTUAL_DUMP */", block)
    with tempfile.TemporaryDirectory(prefix="xmini-app-memory-") as directory:
        unit, executable = Path(directory) / "dump.c", Path(directory) / "dump"
        unit.write_text(fixture, encoding="utf-8")
        command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", str(unit), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Actual DUMP admission/copy/error/free and main scratch/feedback lifetime PASS")


if __name__ == "__main__":
    main()
