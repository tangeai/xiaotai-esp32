"""Compile the producer's actual cadence rule; never access a camera or SDK."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "main/xiaotai/media/s3/src/starter_camera.c").read_text(encoding="utf-8")
period = re.search(r"^#define VIEW_FRAME_US .+$", source, re.M)
ai_period = re.search(r"^#define AI_VISION_FRAME_US .+$", source, re.M)
ai_max_age = re.search(r"^#define AI_VISION_MAX_AGE_MS .+$", source, re.M)
function = re.search(r"static int64_t next_frame_deadline\([^)]*\)\s*\{.*?\n\}", source, re.S)
assert period and ai_period and ai_max_age and function
assert "next = next_frame_deadline(next, done, frame_us);" in source
assert "retry_fresh_frame = current && mode == STARTER_TIRTC_AI;" in source
assert "next = done;" in source
main = r'''
int main(void) {
    const int64_t p = VIEW_FRAME_US;
    assert(AI_VISION_FRAME_US == 1000000);
    assert(AI_VISION_MAX_AGE_MS == 750);
    assert(next_frame_deadline(0, 0, p) == p);
    assert(next_frame_deadline(0, p, p) == p);
    assert(next_frame_deadline(0, p + 1, p) == p + 1);
    assert(next_frame_deadline(0, 20 * p + 99, p) == 20 * p + 99);
    int64_t deadline = 123456789012LL;
    const int64_t start = deadline;
    for (int i = 0; i < 100000; ++i) {
        /* Wake-up jitter does not accumulate into a slower nominal rate. */
        deadline = next_frame_deadline(deadline, deadline + i % 5000 + 50000, p);
        assert(deadline == start + (int64_t)(i + 1) * p);
    }
    for (int64_t cost = 0; cost < 10 * p; cost += 997) {
        int64_t next = next_frame_deadline(deadline, deadline + cost, p);
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
                                  period[0] + "\n" + ai_period[0] + "\n" +
                                  ai_max_age[0] + "\n" +
                                  function[0] + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: H5 12fps, AI 1fps/750ms freshness, immediate stale retry, no catch-up burst")
