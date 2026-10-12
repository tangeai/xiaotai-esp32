"""Compile the producer's actual cadence rule; never access a camera or SDK."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_camera.c").read_text(encoding="utf-8")
period = re.search(r"^#define VIEW_FRAME_US .+$", source, re.M)
function = re.search(r"static int64_t next_frame_deadline\([^)]*\)\s*\{.*?\n\}", source, re.S)
assert period and function
assert "next = next_frame_deadline(next, done);" in source
main = r'''
int main(void) {
    const int64_t p = VIEW_FRAME_US;
    assert(next_frame_deadline(0, 0) == p);
    assert(next_frame_deadline(0, p) == p);
    assert(next_frame_deadline(0, p + 1) == p + 1);
    assert(next_frame_deadline(0, 20 * p + 99) == 20 * p + 99);
    int64_t deadline = 123456789012LL;
    const int64_t start = deadline;
    for (int i = 0; i < 100000; ++i) {
        /* Wake-up jitter does not accumulate into a slower nominal rate. */
        deadline = next_frame_deadline(deadline, deadline + i % 5000 + 50000);
        assert(deadline == start + (int64_t)(i + 1) * p);
    }
    for (int64_t cost = 0; cost < 10 * p; cost += 997) {
        int64_t next = next_frame_deadline(deadline, deadline + cost);
        assert(next >= deadline + cost && next > deadline);
        assert(next >= deadline + p);
        if (cost > p) assert(next == deadline + cost);
        assert(next - (deadline + cost) <= p);
    }
}
'''
with tempfile.TemporaryDirectory(prefix="s3-camera-pacing-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text("#include <stdint.h>\n#include <assert.h>\n" +
                                  period[0] + "\n" + function[0] + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: camera 12fps cadence, wake jitter, no extra idle after overrun, long uptime")
